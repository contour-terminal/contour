// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Base64.hpp>
#include <core/Random.hpp>
#include <core/Utf8Path.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SoftwareSecurityKey.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>
#include <vthost/CommandBlockWire.hpp>
#include <vthost/DaemonControlWire.hpp>
#include <vthost/FlushPolicy.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/NodeMove.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TabMerge.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/proto/PduTrace.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/PreNegotiationCodec.hpp>
#include <vthost/testing/ShippedCodec.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/LayoutTree.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;

using core::async::Task;
using vtbackend::test::commandEnd;
using vtbackend::test::PromptStart;
using vtbackend::test::recordIds;
using vtbackend::test::reportedOutputStart;
using vthost::NativeSession;
using vthost::SessionHost;
using vthost::testing::cellOf;
using vthost::testing::eventually;
using vthost::testing::newSession;
namespace proto = vthost::proto;

namespace
{

/// Writes the pre-encoded request bytes onto the wire.
Task<void> feedBytes(core::net::ISocket* client, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// The longest a test client waits for what it counts on before it gives up and closes its end.
///
/// A bound, not a budget: an exchange finishes in milliseconds, and the longest snapshot runs here
/// took about 13 s on a machine loaded well past its cores. Without it, a PDU that never comes
/// leaves the loop waiting for I/O that never completes, and the suite hangs instead of failing
/// the count.
constexpr auto CollectBudget = std::chrono::milliseconds { 60'000 };

/// How a test client reads: as fast as the transport delivers, unless it is told to pause.
///
/// A client that pauses before every read keeps a frame larger than a bounded transport in flight
/// across that many pauses — the way to hold a server's send queue in a given state for longer
/// than a timer of its own takes to fire.
struct ReadPacing
{
    core::net::EventLoop* loop = nullptr; ///< What to pause on; null reads without pausing.
    std::chrono::milliseconds pause {};   ///< How long each pause lasts.
    std::size_t chunk = 4096;             ///< The most one read takes.
};

/// What a collector consults after each decoded frame: true once it has what it waits for.
using CollectDone = std::function<bool(std::vector<proto::DecodedFrame> const&)>;

/// Decodes server PDUs into @p out until @p done says to stop or the stream ends.
Task<void> decodeUntil(core::net::ISocket* client,
                       CollectDone done,
                       std::vector<proto::DecodedFrame>* out,
                       ReadPacing pacing)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(pacing.chunk);
    while (!done(*out))
    {
        auto decoded = proto::decodePdu(buffer);
        if (decoded)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(decoded->consumed));
            out->push_back(std::move(*decoded));
            continue;
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            break;
        if (pacing.loop != nullptr)
            co_await pacing.loop->delay(pacing.pause);
        auto const n = co_await client->read(scratch);
        if (!n.has_value() || *n == 0)
            break;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
    }
}

/// Decodes server PDUs until @p done says to stop, the stream ends or @p budget is spent, then
/// closes the client end — the session's read loop sees EOF and finishes.
///
/// The bound is a predicate rather than a count because the two things tests wait for are shaped
/// differently: a fixed number of PDUs, or a marker one of them carries. How many pieces a
/// snapshot run takes depends on how its rows happen to partition, which is exactly what a test
/// of the partitioning must not hardcode.
/// @param loop What the budget is timed on.
/// @param done Consulted after each decoded frame; the collector stops when it returns true.
/// @param pacing How fast the client reads.
/// @param budget How long to wait for @p done before giving up on it, with what arrived so far.
Task<void> collectUntil(core::net::EventLoop* loop,
                        core::net::ISocket* client,
                        CollectDone done,
                        std::vector<proto::DecodedFrame>* out,
                        ReadPacing pacing = {},
                        std::chrono::milliseconds budget = CollectBudget)
{
    co_await core::net::testing::anyOf(decodeUntil(client, std::move(done), out, pacing),
                                       core::net::testing::sleepFor(loop, budget));
    client->close();
}

/// Collects exactly @p expected PDUs, or what arrived within @p budget.
Task<void> collectPdus(core::net::EventLoop* loop,
                       core::net::ISocket* client,
                       std::size_t expected,
                       std::vector<proto::DecodedFrame>* out,
                       std::chrono::milliseconds budget = CollectBudget)
{
    co_await collectUntil(
        loop, client, [expected](auto const& got) { return got.size() >= expected; }, out, {}, budget);
}

/// @return Whether @p frame terminates a snapshot run.
bool endsSnapshotRun(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    return delta != nullptr && delta->snapshot != 0 && delta->completesSnapshot();
}

/// @return Whether @p frame is an increment: a Delta that belongs to no snapshot run.
bool isIncrement(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    return delta != nullptr && delta->snapshot == 0;
}

/// One session's snapshot run as the client received it.
struct ReceivedRun
{
    std::vector<proto::WireLine> rows;     ///< Every row of the run, in arrival order.
    std::vector<std::size_t> rowsPerPiece; ///< How many of them each piece carried.
};

/// @return @p session's snapshot run out of @p frames.
ReceivedRun receivedRun(std::vector<proto::DecodedFrame> const& frames, vtworkspace::SessionId session)
{
    auto run = ReceivedRun {};
    for (auto const& frame: frames)
    {
        auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
        if (delta == nullptr || delta->snapshot == 0 || delta->session != session.value)
            continue;
        run.rows.insert(run.rows.end(), delta->lines.begin(), delta->lines.end());
        run.rowsPerPiece.push_back(delta->lines.size());
    }
    return run;
}

/// @return How many rows each piece holds when @p rows are partitioned at @p budget.
std::vector<std::size_t> rowsPerPiece(std::span<proto::WireLine const> rows, std::size_t budget)
{
    auto counts = std::vector<std::size_t> {};
    for (auto const& piece: proto::partitionSnapshotRows(rows, budget))
        counts.push_back(piece.count);
    return counts;
}

/// Collects a whole snapshot run, plus @p extra PDUs after it, read at @p pacing.
Task<void> collectSnapshotRun(core::net::EventLoop* loop,
                              core::net::ISocket* client,
                              std::vector<proto::DecodedFrame>* out,
                              std::size_t extra = 0,
                              ReadPacing pacing = {})
{
    co_await collectUntil(
        loop,
        client,
        [extra](std::vector<proto::DecodedFrame> const& got) {
            auto const end = std::ranges::find_if(got, endsSnapshotRun);
            return end != got.end() && std::cmp_greater(std::distance(end, got.end()), extra);
        },
        out,
        pacing);
}

/// driveExchange, but bounded by a snapshot run's completion instead of a PDU count.
Task<void> driveSnapshotRun(core::net::EventLoop* loop,
                            NativeSession* session,
                            core::net::ISocket* client,
                            std::vector<std::byte> const* request,
                            std::vector<proto::DecodedFrame>* out,
                            std::size_t extra = 0,
                            ReadPacing pacing = {})
{
    co_await core::async::whenAll(
        session->run(), feedBytes(client, request), collectSnapshotRun(loop, client, out, extra, pacing));
}

Task<void> driveExchange(core::net::EventLoop* loop,
                         NativeSession* session,
                         core::net::ISocket* client,
                         std::vector<std::byte> const* request,
                         std::size_t expected,
                         std::vector<proto::DecodedFrame>* out)
{
    co_await core::async::whenAll(
        session->run(), feedBytes(client, request), collectPdus(loop, client, expected, out));
}

/// Encodes @p pdus into one contiguous request, serials counting from 1.
std::vector<std::byte> encodeRequest(std::vector<proto::DecodedPdu> const& pdus)
{
    auto request = proto::Writer {};
    auto serial = uint64_t { 1 };
    for (auto const& pdu: pdus)
        proto::encodePdu(request, serial++, pdu);
    return { request.view().begin(), request.view().end() };
}

/// The printable text of @p line, skipping the empty cells a trimmed row leaves behind.
/// @param line A row as it arrived on the wire.
/// @return Its codepoints as ASCII, in column order.
std::string textOf(proto::WireLine const& line)
{
    auto text = std::string {};
    for (auto const& cell: line.cells)
        if (cell.codepoint != 0)
            text += static_cast<char>(cell.codepoint);
    return text;
}

/// @param history The hosted terminals' scrollback depth.
/// @param blockLimits How many command-block records each hosted terminal keeps.
/// @return Host settings with @p history worth of scrollback.
vtbackend::Settings hostSettings(vtbackend::LineCount history,
                                 vtbackend::CommandBlockStoreLimits blockLimits = {})
{
    auto settings = vtbackend::Settings {};
    settings.historyLimits = vtbackend::HistoryLimits::plain(history);
    settings.commandBlockLimits = blockLimits;
    return settings;
}

/// What a NativeHarness is built with. One struct rather than positional defaults, so a
/// test needing only the second knob does not have to name the first.
/// Which control API a harness's connection calls.
enum class ControlServing : uint8_t
{
    Null = 0, ///< NativeSessionServices' default, which serves nothing.
    Served,   ///< A ControlService over the harness's host.
};

struct HarnessOptions
{
    std::size_t writeQueueBytes = NativeSession::DefaultWriteQueueBytes;
    vtbackend::LineCount history { 0 };   ///< The hosted terminals' scrollback depth.
    vtworkspace::Layout startupLayout {}; ///< Realized into the host BEFORE any handshake.
    /// Set: the connection is an in-memory pair holding at most this many bytes per direction,
    /// whose writes park until the client reads. Unset: a real socket pair, whose kernel buffers
    /// absorb a platform-dependent amount first.
    std::optional<std::size_t> pipeBytes = std::nullopt;
    vtbackend::CommandBlockStoreLimits blockLimits {}; ///< How many records each hosted terminal keeps.
    vtbackend::HostIdentity identity {};               ///< The machine the host says it runs on.
    uint64_t instance = 0;                             ///< Which run of the daemon the host is.
    /// The longest an exchange waits for the PDUs it counts on before it gives up on them.
    std::chrono::milliseconds collectBudget = CollectBudget;
    /// Who feeds the hosted terminals from their PTYs: the case itself, unless it asks for the pumps.
    vthost::SessionPumping pumping = vthost::SessionPumping::Manual;
    /// What makes each session's PTY; unset, a MockPty.
    vthost::PtyFactory ptyFactory {};
    /// Where a connection's input reads pause and resume.
    vthost::InputWatermarks inputWatermarks {};
    /// The status-line extensions the hosted sessions run; none by default.
    std::shared_ptr<vthost::IExtensionHosting> extensions {};
    /// What the hosted terminals' settings differ in from hostSettings()'s; nothing by default.
    std::function<void(vtbackend::Settings&)> adjustSettings {};
    /// What the connection is constructed with: its handshake policy among them.
    vthost::NativeSessionServices services {};
    /// What the loop, and so every flush deadline, is timed on; null for the process's steady clock.
    core::platform::IClock* clock = nullptr;
    /// The host's profiles; none by default.
    std::shared_ptr<vthost::ProfileCatalog const> profiles {};
    /// Whether the connection's control API is a ControlService over the harness's host, or the null one.
    ControlServing control = ControlServing::Null;
    /// The ControlService's reply cap, when it serves one.
    std::size_t maxReplyBytes = std::size_t { 1 } * 1024 * 1024;
    /// The endpoint the connection was accepted on, as its ConnectionId says ("native-tcp" for a network
    /// one).
    std::string endpoint = "test";
    /// Where the host looks up its workspaces' git state; none tracks none.
    std::optional<vthost::WorkspaceGitServices> workspaceGit = std::nullopt;
};

/// @param options What the harness is built with.
/// @return The settings its host's sessions are built with.
[[nodiscard]] vtbackend::Settings harnessSettings(HarnessOptions const& options)
{
    auto settings = hostSettings(options.history, options.blockLimits);
    if (options.adjustSettings)
        options.adjustSettings(settings);
    return settings;
}

/// @param loop The loop a real pair is driven by.
/// @param pipeBytes HarnessOptions::pipeBytes.
/// @return The connection: `first` the server's end, `second` the client's.
core::net::testing::SocketPair makeConnection(core::net::EventLoop& loop,
                                              std::optional<std::size_t> pipeBytes)
{
    if (!pipeBytes)
        return *core::net::testing::makeSocketPair(loop);
    auto pipe = core::net::testing::InMemorySocketPair::create(*pipeBytes);
    return { .first = std::move(pipe.server), .second = std::move(pipe.client) };
}

/// A send-queue bound too small to hold two snapshot pieces, so that one piece can outgrow it alone.
constexpr auto SmallQueueBytes = std::size_t { 64 } * 1024;

/// @return A connection bounded by SmallQueueBytes, over an in-memory pipe holding as much, whose
///         hosted terminals keep enough scrollback for a two-piece snapshot.
HarnessOptions smallQueueOptions()
{
    return { .writeQueueBytes = SmallQueueBytes,
             .history = vtbackend::LineCount(1000),
             .pipeBytes = SmallQueueBytes };
}

/// @return A client that reads at most SmallQueueBytes every 25ms. That is slower than the flush
///         debounce, so a piece larger than the pipe stays in flight across several flushes.
ReadPacing slowReader(core::net::EventLoop& loop)
{
    return ReadPacing { .loop = &loop, .pause = 25ms, .chunk = SmallQueueBytes };
}

/// Drives a NativeSession over an in-process connection (HarnessOptions::pipeBytes says which kind).
struct NativeHarness
{
    explicit NativeHarness(HarnessOptions opts = {}): options { std::move(opts) } {}

    HarnessOptions options {}; // set by the constructor, before `loop`, `host` and `session` read it
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source,
                                options.clock != nullptr ? *options.clock
                                                         : core::platform::defaultSteadyClock() };
    /// The directories the host finds on disk, so an inherited one need not exist on the machine.
    core::platform::testing::InMemoryFileSystem files;
    SessionHost host { loop,
                       options.ptyFactory
                           ? options.ptyFactory
                           : [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&)
                           -> std::unique_ptr<vtpty::Pty> { return std::make_unique<vtpty::MockPty>(size); },
                       harnessSettings(options),
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = options.pumping,
                                                   .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                   .startupLayout = options.startupLayout,
                                                   .identity = options.identity,
                                                   .fileSystem = files,
                                                   .workspaceGit = options.workspaceGit,
                                                   .inputWatermarks = options.inputWatermarks,
                                                   .extensions = options.extensions,
                                                   .profiles = options.profiles,
                                                   .instance = options.instance } };
    core::net::testing::SocketPair pair = makeConnection(loop, options.pipeBytes);
    core::platform::ManualWallClock wall {};
    /// The control API the connection calls, under ControlServing::Served; before the connection, which
    /// drops its subscriptions in it as it goes.
    std::unique_ptr<vthost::control::ControlService> control =
        options.control == ControlServing::Served
            ? std::make_unique<vthost::control::ControlService>(
                  host,
                  vthost::control::ControlServiceConfig { .version = "test",
                                                          .socketPath = {},
                                                          .clock = wall,
                                                          .maxReplyBytes = options.maxReplyBytes,
                                                          .upgrade = upgradeStarter() })
            : nullptr;
    std::unique_ptr<NativeSession> session =
        makeSession(pair, vthost::ConnectionId { .endpoint = options.endpoint, .index = 1 });

    /// @return How `daemon.upgrade` starts an upgrade of the services' daemon: the flow the daemon runs.
    [[nodiscard]] vthost::control::UpgradeStarter upgradeStarter()
    {
        return [this](vthost::handover::UpgradeTarget target,
                      vthost::UpgradeStarted onStarted,
                      vthost::UpgradeFailed onFailed) {
            loop.spawn(vthost::serveUpgrade(&options.services.daemon.get(),
                                            std::move(target),
                                            std::move(onStarted),
                                            std::move(onFailed)));
        };
    }

    /// @return A connection of this harness's host over @p connection, with its services and its control API.
    [[nodiscard]] std::unique_ptr<NativeSession> makeSession(core::net::testing::SocketPair& connection,
                                                             vthost::ConnectionId id)
    {
        auto services = options.services;
        if (control)
            services.control = *control;
        return std::make_unique<NativeSession>(loop,
                                               host,
                                               std::move(id),
                                               std::move(connection.first),
                                               options.writeQueueBytes,
                                               std::move(services));
    }

    /// What the client has decoded in the current exchange, as it arrives: what a step run beside
    /// the exchange waits on.
    std::vector<proto::DecodedFrame> received {};

    /// Encodes @p pdus, runs the exchange, returns the first @p expected server PDUs.
    /// @param alongside Run beside the exchange: writes and events, each gated on what arrived.
    template <typename... Alongside>
    std::vector<proto::DecodedFrame> exchange(std::vector<proto::DecodedPdu> const& pdus,
                                              std::size_t expected,
                                              Alongside... alongside)
    {
        return exchangeUntil(
            pdus, [expected](auto const& got) { return got.size() >= expected; }, std::move(alongside)...);
    }

    /// Encodes @p pdus, runs the exchange until @p done holds of what arrived, and returns what did.
    /// @param alongside Run beside the exchange: writes and events, each gated on what arrived.
    template <typename... Alongside>
    std::vector<proto::DecodedFrame> exchangeUntil(std::vector<proto::DecodedPdu> const& pdus,
                                                   CollectDone done,
                                                   Alongside... alongside)
    {
        return exchangeBytesUntil(encodeRequest(pdus), std::move(done), std::move(alongside)...);
    }

    /// Writes @p bytes as they are, runs the exchange until @p done holds of what arrived, and returns what
    /// did: for frames no catalog PDU encodes to.
    /// @param alongside Run beside the exchange: writes and events, each gated on what arrived.
    template <typename... Alongside>
    std::vector<proto::DecodedFrame> exchangeBytesUntil(std::vector<std::byte> const& bytes,
                                                        CollectDone done,
                                                        Alongside... alongside)
    {
        received.clear();
        loop.blockOn(core::net::testing::allOf(
            session->run(),
            feedBytes(pair.second.get(), &bytes),
            collectUntil(&loop, pair.second.get(), std::move(done), &received, {}, options.collectBudget),
            std::move(alongside)...));
        return received;
    }

    /// Attaches with a bare ClientHello: exchange() of that alone.
    template <typename... Alongside>
    std::vector<proto::DecodedFrame> attach(std::size_t expected, Alongside... alongside)
    {
        return exchange({ proto::DecodedPdu { proto::ClientHello {} } }, expected, std::move(alongside)...);
    }
};

/// The options of a harness whose sessions run their pumps, each over a StallingPty.
/// @param pty Receives the last StallingPty made.
/// @param options The rest of the options.
/// @return @p options, pumped and stalling.
[[nodiscard]] HarnessOptions stallingHarness(vthost::testing::StallingPty** pty, HarnessOptions options = {})
{
    options.pumping = vthost::SessionPumping::Threaded;
    options.ptyFactory =
        [pty](vtbackend::PageSize size,
              std::optional<vtpty::Process::ExecInfo> const&) -> std::unique_ptr<vtpty::Pty> {
        auto made = std::make_unique<vthost::testing::StallingPty>(size);
        *pty = made.get();
        return made;
    };
    return options;
}

/// @return Whether @p frame is an ImageGone.
[[nodiscard]] bool isImageGone(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::ImageGone>(frame.pdu);
}

/// @return Whether an ImageGone is among @p got.
[[nodiscard]] bool imageGoneArrived(std::vector<proto::DecodedFrame> const& got)
{
    return std::ranges::any_of(got, isImageGone);
}

/// @param session The session the input is for.
/// @param text The input, as a client encodes it.
/// @return The Input request.
[[nodiscard]] proto::DecodedPdu inputRequest(uint64_t session, std::string_view text)
{
    auto input = proto::Input { .session = session, .data = {} };
    for (auto const ch: text)
        input.data.push_back(static_cast<std::byte>(ch));
    return input;
}

/// @param session The session whose screen is written.
/// @param text What is written, as if the session's application had written it.
/// @return The WriteScreen request.
[[nodiscard]] proto::DecodedPdu writeScreenRequest(uint64_t session, std::string text)
{
    return proto::PerformOperation { .session = session,
                                     .operation = proto::EngineOperation::WriteScreen,
                                     .text = std::move(text) };
}

/// What exchangeWhileStalled() saw.
struct StalledExchange
{
    std::future_status answered;               ///< Ready when the exchange ended within five seconds.
    std::vector<proto::DecodedFrame> received; ///< What the client received.
};

/// Runs @p h's exchange of @p pdus until @p done holds, while @p pty stalls, and drains @p pty once the
/// exchange is done or after five seconds of it: a loop parked on a write then ends the case rather than the
/// run. The PDUs end with a FetchImage of an unknown id, whose ImageGone shows the loop answering.
/// @param done What the exchange waits for; by default, that ImageGone.
/// @return Whether the exchange ended within those five seconds, and what it received.
[[nodiscard]] StalledExchange exchangeWhileStalled(NativeHarness& h,
                                                   vthost::testing::StallingPty& pty,
                                                   std::vector<proto::DecodedPdu> const& pdus,
                                                   CollectDone done = imageGoneArrived)
{
    auto exchanged = std::promise<void> {};
    auto answered = std::future_status::timeout;
    auto watchdog = std::jthread { [&answered, &pty, done = exchanged.get_future()] {
        answered = done.wait_for(std::chrono::seconds { 5 });
        pty.drain();
    } };
    auto received = h.exchangeUntil(pdus, std::move(done));
    exchanged.set_value();
    watchdog.join();
    return { .answered = answered, .received = std::move(received) };
}

/// Waits up to five seconds for what @p pty was written to satisfy @p done, as a pump writes it on its own
/// thread.
/// @return What was written by then.
[[nodiscard]] std::string writtenOnceDone(vthost::testing::StallingPty const& pty,
                                          std::function<bool(std::string const&)> const& done)
{
    std::ignore = eventually([&] { return done(pty.written()); });
    return pty.written();
}

/// What the backpressure case's coroutines share, by pointer.
struct BackpressureWatch
{
    core::net::EventLoop* loop;        ///< What the watch waits on.
    core::net::ISocket* client;        ///< Streams the input; closed once it is written.
    vthost::testing::StallingPty* pty; ///< Stalled until the watch drains it.
    std::size_t total;                 ///< How many bytes the client streams.
    std::size_t expectedWritten;       ///< What the PTY was written once everything is delivered.
    std::vector<proto::DecodedFrame> const* otherReceived; ///< What the other client received so far.
    std::size_t fed = 0;                                   ///< What the transport took so far.
    std::size_t fedBeforeDrain = 0;                        ///< What it had taken when the stream stalled.
    std::vector<proto::DecodedFrame> otherBeforeDrain {};  ///< What the other client had by then.
};

/// Writes @p bytes onto the wire in @p chunk pieces, counting what the transport took into @p fed.
Task<void> feedCounting(core::net::ISocket* client,
                        std::vector<std::byte> const* bytes,
                        std::size_t chunk,
                        std::size_t* fed)
{
    while (*fed < bytes->size())
    {
        auto const piece =
            std::span<std::byte const> { *bytes }.subspan(*fed, std::min(chunk, bytes->size() - *fed));
        auto const written = co_await client->write(piece);
        if (!written || *written == 0)
            co_return;
        *fed += *written;
    }
}

/// Waits until the stream stalls -- the transport took nothing for a fifth of a second -- or ends, records
/// what it took, waits for the other client's answer, then drains the PTY and waits until the rest is taken
/// and written. Closes the streaming client last, which ends its session's read loop.
Task<void> watchBackpressure(BackpressureWatch* watch)
{
    auto last = watch->fed;
    auto stillTicks = 0;
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 20'000))
    {
        if (watch->fed == watch->total || stillTicks >= 200)
            break;
        co_await watch->loop->delay(std::chrono::milliseconds { 1 });
        stillTicks = watch->fed == last ? stillTicks + 1 : 0;
        last = watch->fed;
    }
    watch->fedBeforeDrain = watch->fed;

    std::ignore = co_await core::net::testing::waitUntil(
        watch->loop, [watch] { return std::ranges::any_of(*watch->otherReceived, isImageGone); }, 5'000);
    watch->otherBeforeDrain = *watch->otherReceived;

    watch->pty->drain();
    std::ignore = co_await core::net::testing::waitUntil(
        watch->loop,
        [watch] {
            return watch->fed == watch->total && watch->pty->written().size() == watch->expectedWritten;
        },
        20'000);
    watch->client->close();
}

/// How many PDUs the attach snapshot of a single quiet session takes: ServerHello, LayoutState,
/// SessionState and the Delta.
constexpr auto AttachSnapshotFrames = std::size_t { 4 };

/// What a step run beside an exchange waits for before it acts; consulted once per loop tick.
using StepGate = std::function<bool()>;

/// How many loop ticks (1 ms each) a step waits for its gate: CollectBudget's worth, so a step never
/// gives up on a loaded machine sooner than the collector it runs beside would.
constexpr auto StepTicks = static_cast<int>(CollectBudget.count());

/// @param frames What a client has decoded so far; only read.
/// @param count How many frames to wait for.
/// @return A gate that holds once @p frames holds at least @p count frames.
[[nodiscard]] StepGate holdsFrames(std::vector<proto::DecodedFrame> const* frames, std::size_t count)
{
    return [frames, count] {
        return frames->size() >= count;
    };
}

/// Waits until @p gate holds, failing the test when it never does within StepTicks.
///
/// Gated on what happened rather than on a delay: a delay guesses how long the frames before it
/// take, and a loaded machine that runs the loop late makes the guess wrong. A write then lands
/// inside the snapshot, or beside another write in one increment, and a client counting frames
/// waits for one that never comes.
Task<void> awaitGate(core::net::EventLoop* loop, StepGate gate)
{
    auto const held = co_await core::net::testing::waitUntil(loop, std::move(gate), StepTicks);
    CHECK(held);
}

/// Once @p gate holds: writes @p bytes into @p id and kicks the debounced flush. By value: a
/// coroutine frame outlives the full-expression that created it.
Task<void> writeOnce(NativeHarness* h, vtworkspace::SessionId id, std::string bytes, StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    h->host.terminal(id)->writeToScreen(bytes);
    h->session->sessionScreenUpdated(id);
}

/// Once the attach snapshot has landed in @p frames: flips the hosted terminal to the alternate
/// screen and kicks the debounced flush.
Task<void> flipToAltScreen(NativeHarness* h,
                           vtworkspace::SessionId id,
                           std::vector<proto::DecodedFrame> const* frames)
{
    return writeOnce(h, id, "\033[?1049hALT!", holdsFrames(frames, AttachSnapshotFrames));
}

/// Once the attach snapshot, or its first piece, has landed in @p frames: appends to the SAME
/// primary screen and kicks the debounced flush, so a NON-snapshot (incremental) delta follows.
Task<void> appendThenUpdate(NativeHarness* h,
                            vtworkspace::SessionId id,
                            std::vector<proto::DecodedFrame> const* frames)
{
    return writeOnce(h, id, "more", holdsFrames(frames, AttachSnapshotFrames));
}

/// Writes @p rows lines of wide, tagged text into @p session, so its snapshot is a real multiple
/// of a send-queue bound rather than a handful of trimmed lines.
/// @param tag Prefixes each row, so a test can tell one pane's rows from another's.
void fillRows(NativeHarness& h, vtworkspace::SessionId session, int rows, std::string_view tag)
{
    auto lines = std::string {};
    for (auto const row: std::views::iota(0, rows))
        lines += std::format("{}-{}{}\r\n", tag, row, std::string(60, 'x'));
    h.host.terminal(session)->writeToScreen(lines);
}

/// Once the client holds @p id's SessionState -- so the run's pieces are being written behind it --
/// rings the bell in @p id and posts a notification from it: frames neither the debounce nor a
/// snapshot holds back.
/// @param received What the client has decoded so far; only read.
Task<void> ringWhileStreaming(NativeHarness* h,
                              vtworkspace::SessionId id,
                              std::vector<proto::DecodedFrame> const* received)
{
    auto const streaming = co_await core::net::testing::waitUntil(&h->loop, [received] {
        return std::ranges::any_of(*received, [](proto::DecodedFrame const& frame) {
            return std::holds_alternative<proto::SessionState>(frame.pdu);
        });
    });
    CHECK(streaming);
    h->session->sessionBell(id);
    h->session->sessionNotify(id, "build", "finished", std::nullopt);
}

/// Writes @p bursts bursts of four rows into each of @p sessions, @p interval apart, kicking the
/// flush after each, then one row tagged "end" into each.
Task<void> writeBursts(NativeHarness* h,
                       std::vector<vtworkspace::SessionId> const* sessions,
                       int bursts,
                       std::chrono::milliseconds interval)
{
    for (auto const burst: std::views::iota(0, bursts))
    {
        co_await h->loop.delay(interval);
        for (auto const id: *sessions)
        {
            fillRows(*h, id, 4, std::format("burst{}", burst));
            h->session->sessionScreenUpdated(id);
        }
    }
    for (auto const id: *sessions)
    {
        fillRows(*h, id, 1, "end");
        h->session->sessionScreenUpdated(id);
    }
}

/// Once the attach snapshot has landed in @p frames: repositions ONLY the cursor (writing no
/// cell content) and kicks the debounced flush.
Task<void> moveCursorThenUpdate(NativeHarness* h,
                                vtworkspace::SessionId id,
                                std::vector<proto::DecodedFrame> const* frames)
{
    // CUP: move cursor to row 10, col 5
    return writeOnce(h, id, "\033[10;5H", holdsFrames(frames, AttachSnapshotFrames));
}

/// Once the attach snapshot has landed in @p frames: sets the window title (OSC 2) and kicks the
/// debounced flush, so an incremental delta carrying the new title follows.
Task<void> setTitleThenUpdate(NativeHarness* h,
                              vtworkspace::SessionId id,
                              std::vector<proto::DecodedFrame> const* frames)
{
    // OSC 2: set window title
    return writeOnce(h, id, "\033]2;my-title\033\\", holdsFrames(frames, AttachSnapshotFrames));
}

/// Once the attach snapshot has landed in @p frames: evicts the scrollback (ED 3) and kicks the debounced
/// flush. Deliberately the one operation that changes NO row and moves NO cursor — Grid::clearHistory
/// bumps no generation and dirties no line, so the floor is the only thing that moves.
Task<void> clearHistoryThenUpdate(NativeHarness* h,
                                  vtworkspace::SessionId id,
                                  std::vector<proto::DecodedFrame> const* frames)
{
    // ED 3: erase the saved lines
    return writeOnce(h, id, "\033[3J", holdsFrames(frames, AttachSnapshotFrames));
}

/// Schedules a debounce flush, then disconnects before it can fire.
Task<void> kickThenDisconnect(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->session->sessionScreenUpdated(id); // parks the 20ms debounce flush
    h->pair.second->close();              // client gone: run() must settle the flush
}

/// Feeds @p bytes, lets the session answer (and its 20ms debounce fire), then closes the
/// client end.
///
/// The close is what lets a test count what the session sent: `collectPdus` otherwise stops
/// at a guessed number, which can only assert that at least that many arrived — useless when
/// the claim is that NOTHING more follows. Stopping at EOF makes the count exact.
Task<void> feedThenDrain(NativeHarness* h, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await h->pair.second->write(std::span<std::byte const> { bytes->data(), bytes->size() });
    co_await h->loop.delay(60ms);
    h->pair.second->close();
}

/// Runs one request to completion and returns EVERY PDU the session sent in answer.
std::vector<proto::DecodedFrame> exchangeAll(NativeHarness* h, std::vector<proto::DecodedPdu> const& pdus)
{
    auto const bytes = encodeRequest(pdus);
    auto received = std::vector<proto::DecodedFrame> {};
    // A ceiling far above any expected answer: the drain below ends collection, not this.
    h->loop.blockOn(core::net::testing::allOf(h->session->run(),
                                              feedThenDrain(h, &bytes),
                                              collectPdus(&h->loop, h->pair.second.get(), 1000, &received)));
    return received;
}

/// One host, TWO connections — the shape every multi-client property needs, and which the
/// single-connection harness above cannot express: a claim about what the OTHER client received
/// has no other client to receive it.
struct TwoClientHarness
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       hostSettings(vtbackend::LineCount(0)),
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    core::net::testing::SocketPair firstPair = *core::net::testing::makeSocketPair(loop);
    core::net::testing::SocketPair secondPair = *core::net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> serverOne = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(firstPair.first));
    std::unique_ptr<NativeSession> serverTwo = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 2 }, std::move(secondPair.first));
};

/// Feeds @p bytes to @p client once @p ready holds for what another client has received, so a request
/// is ordered after that client's exchange rather than after a guess at how long it takes.
/// @param frames What the other client has decoded so far; only read.
Task<void> feedOnce(core::net::EventLoop* loop,
                    core::net::ISocket* client,
                    std::vector<std::byte> const* bytes,
                    std::vector<proto::DecodedFrame> const* frames,
                    CollectDone ready)
{
    co_await awaitGate(loop, [frames, ready = std::move(ready)] { return ready(*frames); });
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// @return The last SessionState in @p frames, or nullptr if there is none.
[[nodiscard]] proto::SessionState const* lastSessionState(std::vector<proto::DecodedFrame> const& frames)
{
    proto::SessionState const* found = nullptr;
    for (auto const& frame: frames)
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            found = state;
    return found;
}

/// @return How many SessionState PDUs @p frames holds — one per snapshot, so this counts resyncs.
[[nodiscard]] std::size_t sessionStateCount(std::vector<proto::DecodedFrame> const& frames)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        frames, [](auto const& frame) { return std::holds_alternative<proto::SessionState>(frame.pdu); }));
}

Task<void> runThenMark(NativeSession* session, bool* done)
{
    co_await session->run();
    *done = true;
}

/// Bounds the overflow test on regression: if the session never disconnects the
/// client, close it from outside after ~1s so the test fails instead of hanging.
Task<void> closeWatchdog(NativeHarness* h, bool const* done, bool* fired)
{
    if (!co_await core::net::testing::waitUntil(&h->loop, [done] { return *done; }))
    {
        *fired = true;
        h->pair.second->close();
    }
}

/// One whole command cycle on the CURRENT line — a prompt, C with @p commandLine as kitty's cmdline_url,
/// D with @p exitCode — writing no newline, so a burst of them scrolls nothing and stays an increment
/// even with the harness's zero scrollback.
std::string commandCycle(std::string_view commandLine, int exitCode)
{
    return std::string { PromptStart } + reportedOutputStart(commandLine) + commandEnd(exitCode);
}

/// @return A burst of @p count command cycles, each with @p commandLine, exit codes counting from 0.
std::string commandBurst(std::string_view commandLine, int count)
{
    auto burst = std::string {};
    for (auto const exitCode: std::views::iota(0, count))
        burst += commandCycle(commandLine, exitCode % 256);
    return burst;
}

/// Holds the loop's thread for @p duration once the exchange is under way, the way a loaded machine
/// does: every timer and completion that comes due meanwhile is then found in a single turn.
Task<void> stallLoop(NativeHarness* h, std::chrono::milliseconds duration)
{
    co_await h->loop.delay(1ms);
    std::this_thread::sleep_for(duration);
}

/// @return What @p blocks cost by proto::estimatedEncodedSize — the measure every record budget uses.
std::size_t recordBytes(std::span<proto::WireCommandBlock const> blocks)
{
    auto bytes = std::size_t { 0 };
    for (auto const& block: blocks)
        bytes += proto::estimatedEncodedSize(block);
    return bytes;
}

/// @return The incremental (non-snapshot) deltas in @p frames, in arrival order.
std::vector<proto::Delta const*> increments(std::vector<proto::DecodedFrame> const& frames)
{
    auto deltas = std::vector<proto::Delta const*> {};
    for (auto const& frame: frames)
        if (isIncrement(frame))
            deltas.push_back(&std::get<proto::Delta>(frame.pdu));
    return deltas;
}

/// @return Every command-block id @p frames delivered, by SessionState or by Delta.
std::set<uint32_t> deliveredBlockIds(std::vector<proto::DecodedFrame> const& frames)
{
    auto ids = std::set<uint32_t> {};
    for (auto const& frame: frames)
    {
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            for (auto const& block: state->commandBlocks)
                ids.insert(block.id);
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
            for (auto const& block: delta->commandBlocks)
                ids.insert(block.id);
    }
    return ids;
}

/// Collects until @p count distinct command-block ids have arrived.
Task<void> collectBlocks(core::net::EventLoop* loop,
                         core::net::ISocket* client,
                         std::size_t count,
                         std::vector<proto::DecodedFrame>* out)
{
    co_await collectUntil(
        loop, client, [count](auto const& got) { return deliveredBlockIds(got).size() >= count; }, out);
}

/// A client that stops reading: it reads nothing for @p stall, then collects until @p count distinct
/// command-block ids have arrived. Everything the server sends meanwhile fills the pipe, then the
/// send queue's backlog.
Task<void> stallThenCollectBlocks(NativeHarness* h,
                                  std::chrono::milliseconds stall,
                                  std::size_t count,
                                  std::vector<proto::DecodedFrame>* out)
{
    co_await h->loop.delay(stall);
    co_await collectBlocks(&h->loop, h->pair.second.get(), count, out);
}

/// Writes nothing: what an attach with no output after it runs beside the exchange.
Task<void> noOutput()
{
    co_return;
}

/// Attaches and collects until @p count distinct command-block ids have arrived, or the collector gave up on
/// them.
/// @param during Runs beside the exchange — the output written after the attach, if any.
/// @return Every PDU the client received.
std::vector<proto::DecodedFrame> attachUntilBlocks(NativeHarness& h,
                                                   std::size_t count,
                                                   Task<void> during = noOutput())
{
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             collectBlocks(&h.loop, h.pair.second.get(), count, &received),
                                             std::move(during)));
    return received;
}

} // namespace

namespace vthost
{
/// Exposes NativeSession's private follow map to the leak regression test, and lets a record test build
/// one push at a time — exactly what the streamer or one debounce window would build, unsent — and read
/// where a record walk stands between them.
struct NativeSessionFollowTester
{
    static bool follows(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._followed.contains(id.value);
    }
    static std::size_t followedCount(NativeSession const& session) { return session._followed.size(); }

    /// Queues a snapshot of @p id, as a selection does for the sessions it brings in.
    static void requestSnapshotOf(NativeSession& session, vtworkspace::SessionId id)
    {
        session.requestSnapshot(id);
    }

    /// @return The session whose snapshot the streamer has taken off its queue and is sending, if any.
    static std::optional<uint64_t> streaming(NativeSession const& session)
    {
        return session._streamingSession;
    }

    /// @return Whether the streamer has @p id off its queue and is sending it (or waiting for room to).
    static bool streamingOffQueue(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._streamingSession == id.value && session._snapshotQueue.empty();
    }

    /// Selects @p workspace as a SelectWorkspace does.
    static void select(NativeSession& session, vtworkspace::WorkspaceId workspace)
    {
        session.selectWorkspace(workspace);
    }

    /// @return The SessionState of the snapshot the streamer would send for @p id now.
    static proto::SessionState snapshot(NativeSession& session, vtworkspace::SessionId id)
    {
        auto built = session.buildPush(id, NativeSession::SnapshotMode::Forced);
        REQUIRE(built.has_value());
        REQUIRE(built->state.has_value());
        return *std::move(built->state);
    }

    /// @return The increment a debounce window would send for @p id now, or nullopt when it would send none.
    static std::optional<proto::Delta> increment(NativeSession& session, vtworkspace::SessionId id)
    {
        auto built = session.buildPush(id, NativeSession::SnapshotMode::Delta);
        if (!built)
            return std::nullopt;
        return std::move(built->delta);
    }

    /// @return The record @p id's walk resumes at, or nullopt while no walk is open.
    static std::optional<vtbackend::CommandBlockId> walkFrom(NativeSession const& session,
                                                             vtworkspace::SessionId id)
    {
        auto const& follow = session._followed.at(id.value);
        return follow.blockWalk ? std::optional { follow.blockWalk->from } : std::nullopt;
    }

    /// Leaves @p bytes of backlog in @p session's send queue, the way a client that stopped reading
    /// leaves it: the loop does not run, so nothing drains it.
    static void stallSendQueue(NativeSession& session, std::size_t bytes)
    {
        session.send(0,
                     proto::DecodedPdu { proto::SessionNotify {
                         .session = 0, .title = {}, .body = std::string(bytes, 'x') } });
    }

    /// @return Whether @p id's command-block records wait for room in the send queue.
    static bool recordsAwaitRoom(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._recordsAwaitingRoom.contains(id.value);
    }

    /// @return Whether @p id is queued for the next debounce window.
    static bool incrementQueued(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._pendingSessions.contains(id.value);
    }

    /// Puts @p delta in @p session's send queue as pushDelta does, tagged with its session — where a
    /// snapshot of that session supersedes it while it is still unwritten.
    static void enqueue(NativeSession& session, proto::Delta delta)
    {
        auto const tag = delta.session;
        session.send(0, proto::DecodedPdu { std::move(delta) }, tag);
    }

    /// @return Whether the increment a debounce window would build for @p id now cannot describe the
    ///         batch, and asks for a snapshot instead.
    static bool incrementNeedsResync(NativeSession& session, vtworkspace::SessionId id)
    {
        auto const built = session.buildPush(id, NativeSession::SnapshotMode::Delta);
        return !built && built.error() == NativeSession::BuildFailure::ResyncRequired;
    }

    /// Lets go of @p session as run() does when its client leaves: closed, and every flow it spawned —
    /// the debounce flush, the waiter for send room — run out. What a test that built pushes by hand
    /// does last: the loop starts a spawned flow it never ran when it is torn down, after the session.
    static void letGo(NativeSession& session, core::net::EventLoop& loop)
    {
        session._closed = true;
        loop.blockOn(core::net::pollUntil(&loop, [&session] { return session.spawnedFlowsEnded(); }));
    }
};

/// Exposes when NativeSession's next flush is due, and drives a connection through its handshake and out of
/// it by hand: what the flush cases need on a ManualClock, where run()'s own waits would never end.
struct NativeSessionFlushTester
{
    /// @return The earliest flush deadline armed, or nullopt when no flush is due.
    static std::optional<core::platform::SteadyTimePoint> armedDeadline(NativeSession const& session)
    {
        return session._flushDeadline;
    }

    /// @return Whether @p id has an echo state this connection last sent or still owes.
    static bool tracksInputEcho(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._inputEchoSent.contains(id.value) || session._inputEchoPending.contains(id.value);
    }

    /// @return How many flush coroutines are live, superseded ones included.
    static std::size_t flushesInFlight(NativeSession const& session) { return session._flushesInFlight; }

    /// @return Whether the connection keeps a mirror state for @p id: it has pushed the session to its
    /// client.
    static bool mirrors(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._followed.contains(id.value);
    }

    /// Makes each flow the connection spawns -- a flush, the snapshot streamer, the waiter for send room --
    /// with its claim on @p session, and destroys it without ever resuming it, as a loop freeing a flow it
    /// never started does.
    /// @return Whether each had given back its claim once its frame was gone, while it held it before.
    [[nodiscard]] static bool unstartedFlowsGiveBackClaims(NativeSession& session,
                                                           vtworkspace::SessionId streamed)
    {
        auto const releasedAfter = [&session](auto makeFlow) {
            auto heldWhileAlive = false;
            {
                auto const flow = makeFlow();
                heldWhileAlive = !session.spawnedFlowsEnded();
            }
            return heldWhileAlive && session.spawnedFlowsEnded();
        };
        return releasedAfter(
                   [&] { return session.flushAt(session.claimFlush(), session._loop.clock().now()); })
               && releasedAfter([&] { return session.streamSnapshots(session.claimStreamer(streamed)); })
               && releasedAfter([&] { return session.requeueOnSendRoom(session.claimRoomWaiter()); });
    }

    /// @return Whether @p id's snapshot is queued or streaming.
    static bool snapshotPending(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session.snapshotPending(id);
    }

    /// Serves @p pdu as a workspace verb that arrived on the connection.
    static void serveWorkspaceVerb(NativeSession& session, proto::DecodedPdu pdu)
    {
        std::ignore = session.serveWorkspaceVerb(proto::DecodedFrame { .pdu = std::move(pdu) });
    }

    /// Accepts @p hello as if it had arrived: the connection attaches and queues its snapshots.
    static void handshake(NativeSession& session, proto::ClientHello hello = {})
    {
        auto const accepted = session.completeHandshake(proto::DecodedFrame { .pdu = std::move(hello) });
        REQUIRE(accepted);
    }

    /// Lets go of @p session as run() does when its client leaves, advancing @p clock past every deadline
    /// a flush coroutine sleeps to, a turn of @p loop at a time.
    /// @return Whether every flow the session spawned ran out.
    [[nodiscard]] static bool letGo(NativeSession& session,
                                    core::net::EventLoop& loop,
                                    core::platform::ManualClock& clock)
    {
        session._closed = true;
        for ([[maybe_unused]] auto const turn: std::views::iota(0, 1000))
        {
            if (session.spawnedFlowsEnded())
                return true;
            clock.advance(session._services.flush.coalesceDelay);
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
        }
        return false;
    }
};
} // namespace vthost

namespace
{

/// A NativeHarness whose session is driven by hand — pushes built one at a time through
/// NativeSessionFollowTester, the loop never run — and let go as run() lets it go before the harness is
/// torn down, a failed assertion included.
struct ManualHarness
{
    explicit ManualHarness(HarnessOptions options = {}): h { std::move(options) } {}

    NativeHarness h;
    core::Finally settle { [this] { vthost::NativeSessionFollowTester::letGo(*h.session, h.loop); } };
};

/// @return The command-block ids @p client holds, oldest first.
std::vector<uint32_t> heldBlockIds(vthost::client::RemoteScreen const& client)
{
    auto ids = std::vector<uint32_t> {};
    for (auto const& [id, block]: client.commandBlocks)
        ids.push_back(id);
    return ids;
}

/// Hands @p client every increment @p h's session has for @p id, one debounce window at a time, until it
/// has none.
/// @return Every record id they carried, in the order they carried them.
std::vector<uint32_t> drainIncrements(NativeHarness& h,
                                      vtworkspace::SessionId id,
                                      vthost::client::RemoteScreen& client)
{
    auto carried = std::vector<uint32_t> {};
    for ([[maybe_unused]] auto const workspace: std::views::iota(0, 1000))
    {
        auto const delta = vthost::NativeSessionFollowTester::increment(*h.session, id);
        if (!delta)
            break;
        for (auto const& block: delta->commandBlocks)
            carried.push_back(block.id);
        client.apply(*delta);
    }
    return carried;
}

/// A gate on the server rather than on what the client holds, for a client that is not reading yet.
/// @return A gate that holds once @p h's session has built @p id's attach snapshot, so what is written
///         next travels as increments.
[[nodiscard]] StepGate followsSession(NativeHarness* h, vtworkspace::SessionId id)
{
    return [h, id] {
        return vthost::NativeSessionFollowTester::follows(*h->session, id);
    };
}

} // namespace

TEST_CASE("the native handshake answers ServerHello and a full snapshot", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("hello native");

    // Expect: ServerHello, LayoutState, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);

    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->codecVersion == proto::NegotiatingHelloMarker);

    // The layout leads the snapshot so the client builds its tabs before content.
    auto const* layout = std::get_if<proto::LayoutState>(&received[1].pdu);
    REQUIRE(layout != nullptr);
    REQUIRE(layout->tabs.size() == 1);
    CHECK(layout->tabs.front().root.session == sessionId.value);

    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->session == sessionId.value);
    CHECK(state->columns == 80);
    CHECK(state->lines == 25);

    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    REQUIRE(delta->lines.size() == 25); // the whole page ships on attach

    // The written text arrived cell by cell on the first row.
    CHECK(textOf(delta->lines.front()) == "hello native");
}

TEST_CASE("an exchange counting on more PDUs than the session sends ends instead of hanging",
          "[vthost][native]")
{
    // The attach answers four PDUs; the client counts on a fifth that never comes. The exchange must
    // give up once its budget is spent and hand back what did arrive, so a test whose count is wrong
    // fails on the count. Driven from a second thread, so a regression reports itself here instead
    // of as a suite that never ends. Not through std::async: its future joins the thread when
    // destroyed, so the failing REQUIRE would unwind straight into the hang it reports.
    auto exchange = std::packaged_task<std::size_t()> { [] {
        auto h = NativeHarness { { .collectBudget = 200ms } };
        std::ignore = newSession(h.host);
        return h.exchange({ proto::ClientHello {} }, 5).size();
    } };
    auto collected = exchange.get_future();
    auto worker = std::thread { std::move(exchange) };
    auto const outcome = collected.wait_for(30s);
    // A stuck exchange is left behind, still stuck, so the test can fail instead of joining it.
    if (outcome == std::future_status::ready)
        worker.join();
    else
        worker.detach();
    REQUIRE(outcome == std::future_status::ready);
    CHECK(collected.get() == 4);
}

TEST_CASE("a ClientHello attaching to a daemon with a configured startup layout sees the real "
          "tab/pane shape on the very first LayoutState, not a single default tab",
          "[vthost][native][layout]")
{
    // End-to-end regression for the reported symptom: contour client always showed one flat pane
    // on first attach, even when the profile's default_layout configured several tabs -- because
    // SessionHost never realized one and completeHandshake's "attaching to an empty daemon spawns
    // the first session" fallback always built exactly one. With the daemon already populated
    // (as runDaemon() now does via DaemonConfig::startupLayout), that fallback must not fire, and
    // the FIRST LayoutState a client ever sees must already show both tabs.
    auto layout = vtworkspace::Layout {};
    auto firstTab = vtworkspace::LayoutTab {};
    firstTab.root.command = "nvim";
    layout.tabs.push_back(firstTab);
    auto secondTab = vtworkspace::LayoutTab {};
    secondTab.root.command = "htop";
    layout.tabs.push_back(secondTab);

    auto h = NativeHarness { HarnessOptions { .startupLayout = layout } };

    // Expect at least ServerHello, LayoutState, then a SessionState + Delta snapshot PER hosted
    // session (two sessions now, not one). Asserted structurally (find each PDU by type) rather
    // than by exact count/position, so an unrelated protocol addition elsewhere in the attach
    // sequence cannot make this LAYOUT test fail and misdirect whoever investigates it.
    auto const received = h.exchange({ proto::ClientHello {} }, 6);
    REQUIRE(received.size() >= 6);

    auto const helloIt = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::ServerHello>(frame.pdu); });
    REQUIRE(helloIt != received.end());

    auto const layoutIt = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutState>(frame.pdu); });
    REQUIRE(layoutIt != received.end());
    auto const* wireLayout = std::get_if<proto::LayoutState>(&layoutIt->pdu);
    REQUIRE(wireLayout != nullptr);
    REQUIRE(wireLayout->tabs.size() == 2);

    // Both tabs are single-leaf (no split in this layout), each naming a distinct live session.
    CHECK(wireLayout->tabs[0].root.split == 0);
    CHECK(wireLayout->tabs[1].root.split == 0);
    CHECK(wireLayout->tabs[0].root.session != wireLayout->tabs[1].root.session);

    // The host itself already had two sessions before this ClientHello was ever sent -- proving
    // completeHandshake's empty-daemon fallback did not run (it would have left exactly one).
    CHECK(h.host.sessionCount() == 2);
}

TEST_CASE("the attach snapshot carries scrollback that predates the attach", "[vthost][native]")
{
    // The property the whole feature rests on, and the one the user documentation got backwards:
    // a client that was not there when the output happened still receives it. The snapshot is
    // built by Grid::forEachValidLine, which walks from -historyLineCount() rather than from the
    // page top -- so the rows that scrolled off BEFORE the ClientHello travel with the page.
    //
    // Distinct from "a burst that outran the scrollback floor" below, which asserts on the resync
    // snapshot a later burst provokes. This one asserts on the ATTACH snapshot itself, with no
    // output at all after the handshake.
    auto h = NativeHarness { { .history = vtbackend::LineCount(50) } };
    auto const sessionId = newSession(h.host);

    // 40 rows into a 25-line page: 16 of them (the page holds the last 24 plus the cursor row)
    // are scrollback by the time anyone attaches.
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, 40))
        lines += std::format("row-{}\r\n", i);
    h.host.terminal(sessionId)->writeToScreen(lines);

    // Expect: ServerHello, LayoutState, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    // More rows than the page holds: the surplus IS the scrollback.
    CHECK(delta->lines.size() > 25);

    // And it is the right scrollback, not merely the right amount of it. row-0 scrolled off long
    // before the attach, so finding it proves the history travelled rather than the page.
    auto const carries = [delta](std::string_view row) {
        return std::ranges::any_of(delta->lines, [row](auto const& line) { return textOf(line) == row; });
    };
    CHECK(carries("row-0"));
    CHECK(carries("row-39"));
}

TEST_CASE("a row's trailing fill cells do not travel", "[vthost][native]")
{
    // The mirror clears every row under its fill before writing the cells it received, so
    // the columns padding a short line out to the grid's width say nothing the clear did
    // not already say — at ~24 wire bytes each. Across a scrollback that padding IS the
    // snapshot: it is what made a resize able to overflow a client's send queue.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("hello native");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    REQUIRE(delta->lines.size() == 25);

    auto const& written = delta->lines.front();
    CHECK(written.columns == 80);             // the row is still eighty columns wide...
    CHECK(written.cells.size() == 12);        // ...but only "hello native" travels
    CHECK(delta->lines.back().cells.empty()); // an untouched row ships no cells at all

    // What does travel is unchanged — the trim may only remove what the fill reproduces.
    auto text = std::string {};
    for (auto const& cell: written.cells)
        text += static_cast<char>(cell.codepoint);
    CHECK(text == "hello native");
}

TEST_CASE("a trailing run that differs from the fill still travels", "[vthost][native]")
{
    // The trim's predicate is "indistinguishable from this line's fill", not "blank": a run
    // of spaces carrying a background colour is invisible as text and load-bearing as paint.
    // Getting this wrong silently truncates coloured regions, so it is pinned here.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    // Four cells of text, then four blanks on a red background, on a default-filled line.
    h.host.terminal(sessionId)->writeToScreen("bar\033[41m    \033[m");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);

    auto const& written = delta->lines.front();
    REQUIRE(written.cells.size() == 7); // "bar" + four painted blanks, nothing beyond
    for (auto const& cell: written.cells | std::views::drop(3))
    {
        CHECK(cell.codepoint == U' ');                    // nothing to read...
        CHECK(cell.background != written.fillBackground); // ...but not the line's fill either
    }
}

TEST_CASE("Input PDUs land in the target pane's PTY", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    auto const input = inputRequest(sessionId.value, "ls\r");

    std::ignore = h.exchange({ proto::ClientHello {}, input }, 3);

    auto& tapped = dynamic_cast<vthost::TappingPty&>(h.host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(mock.stdinBuffer() == "ls\r");
}

TEST_CASE("an Input PDU leaves the loop answering while the application reads no input", "[vthost][native]")
{
    // A client's input is written to the session's PTY, and a PTY write parks once the application leaves its
    // input unread. Written on the loop thread, it parked the loop, and every client of the daemon with it.
    // The session's pump writes it instead: the loop answers what follows, and the input still arrives, in
    // order, once the application reads.
    auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
    auto h = NativeHarness { stallingHarness(&pty) };
    auto const sessionId = newSession(h.host);
    REQUIRE(pty != nullptr);
    pty->stall();

    auto const input = inputRequest(sessionId.value, "ls\r");

    auto const exchange = exchangeWhileStalled(
        h,
        *pty,
        { proto::ClientHello {}, input, proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } });

    CHECK(exchange.answered == std::future_status::ready);
    CHECK(std::ranges::any_of(exchange.received, isImageGone));
    CHECK(writtenOnceDone(*pty, [](auto const& written) { return written == "ls\r"; }) == "ls\r");
}

TEST_CASE("a session's input backlog counts what its pump is writing", "[vthost][native]")
{
    // The pump takes a session's queued input off the queue before it writes it, and the write parks while
    // the application reads nothing. What it took is held all the same, so the backlog the high-water mark is
    // measured against counts it: otherwise a connection would be read on while the session holds a mark's
    // worth of input in its pump and another in its queue.
    auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
    auto h = NativeHarness { stallingHarness(&pty) };
    auto const sessionId = newSession(h.host);
    REQUIRE(pty != nullptr);
    pty->stall();

    auto const input = std::string(1000, 'a');
    h.host.writeInput(sessionId, input);
    REQUIRE(eventually([&] { return pty->parkedWrites() == 1; }));
    CHECK(h.host.inputBacklog(sessionId) == input.size());

    pty->drain();
    CHECK(eventually([&] { return h.host.inputBacklog(sessionId) == 0; }));
    CHECK(pty->written() == input);
}

TEST_CASE("a client streaming input into a session that reads none is paused while others are served",
          "[vthost][native]")
{
    // While the application reads nothing, a client's input backs up in its session. Past the high-water mark
    // the connection it comes from is read no further, so the client's writes park in the transport and the
    // daemon holds no more than about the mark; below the low-water mark it is read again. Other clients are
    // served meanwhile, and nothing is dropped. Text for the session's screen queues for the same pump, and
    // backs up alike behind input the pump is writing.
    auto constexpr HighWater = std::size_t { 64 } * 1024;
    auto constexpr LowWater = std::size_t { 16 } * 1024;
    auto constexpr PduBytes = std::size_t { 8 } * 1024;
    auto constexpr PduCount = 64;
    auto constexpr PipeBytes = std::size_t { 16 } * 1024;
    auto constexpr ReadChunk = std::size_t { 16 } * 1024; // what one read of the connection takes at most
    auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
    auto h = NativeHarness { stallingHarness(&pty,
                                             HarnessOptions { .pipeBytes = PipeBytes,
                                                              .inputWatermarks = vthost::InputWatermarks {
                                                                  .high = HighWater, .low = LowWater } }) };
    auto const sessionId = newSession(h.host);
    REQUIRE(pty != nullptr);
    pty->stall();

    // What the client streams, and what the PTY was written once the session drained.
    auto pdus = std::vector<proto::DecodedPdu> { proto::ClientHello {} };
    auto const someInput = proto::Input { .session = sessionId.value,
                                          .data = std::vector<std::byte>(PduBytes, std::byte { 0x61 }) };
    auto expectedWritten = std::size_t { 0 };
    SECTION("input for the application")
    {
        for ([[maybe_unused]] auto const pdu: std::views::iota(0, PduCount))
            pdus.emplace_back(someInput);
        expectedWritten = PduBytes * PduCount;
    }
    SECTION("text for its screen, behind input the pump is writing")
    {
        // The pump parks writing the input, and the text queues behind it. The last text asks for a status
        // report (DSR 5), which is answered once it is parsed, after all the rest.
        pdus.emplace_back(someInput);
        for ([[maybe_unused]] auto const pdu: std::views::iota(0, PduCount))
            pdus.push_back(writeScreenRequest(sessionId.value, std::string(PduBytes, 'a')));
        pdus.push_back(writeScreenRequest(sessionId.value, "\033[5n"));
        expectedWritten = PduBytes + std::string_view { "\033[0n" }.size();
    }
    auto const request = encodeRequest(pdus);

    // Another client, on a connection of its own.
    auto otherPair = makeConnection(h.loop, PipeBytes);
    auto other = std::make_unique<NativeSession>(h.loop,
                                                 h.host,
                                                 vthost::ConnectionId { .endpoint = "other", .index = 2 },
                                                 std::move(otherPair.first),
                                                 NativeSession::DefaultWriteQueueBytes);
    auto const otherRequest = encodeRequest(
        { vthost::testing::showingAll(h.host), proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } });
    auto otherReceived = std::vector<proto::DecodedFrame> {};

    auto watch = BackpressureWatch { .loop = &h.loop,
                                     .client = h.pair.second.get(),
                                     .pty = pty,
                                     .total = request.size(),
                                     .expectedWritten = expectedWritten,
                                     .otherReceived = &otherReceived };
    h.loop.blockOn(
        core::net::testing::allOf(h.session->run(),
                                  feedCounting(h.pair.second.get(), &request, 4096, &watch.fed),
                                  other->run(),
                                  feedBytes(otherPair.second.get(), &otherRequest),
                                  collectUntil(
                                      &h.loop,
                                      otherPair.second.get(),
                                      [](auto const& got) { return std::ranges::any_of(got, isImageGone); },
                                      &otherReceived),
                                  watchBackpressure(&watch)));

    CHECK(watch.fedBeforeDrain < watch.total);
    CHECK(watch.fedBeforeDrain <= HighWater + PduBytes + ReadChunk + PipeBytes);
    CHECK(std::ranges::any_of(watch.otherBeforeDrain, isImageGone));
    CHECK(watch.fed == watch.total);
    CHECK(pty->written().size() == watch.expectedWritten);
    CHECK(LowWater < HighWater);
}

TEST_CASE("a WriteScreen operation's replies are written by the session's pump", "[vthost][native]")
{
    // An engine operation that writes to the screen parses text that can ask for replies. Parsed on the loop
    // thread, a reply sent on the spot (DSR 996) was written there, and parked the loop while the application
    // read nothing; a reply only queued (DA1) waited for the pump's next read, which only the application's
    // output brings. The session's pump parses the text as it parses that output, and writes the replies once
    // the parse has let go of the terminal lock.
    auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
    auto h = NativeHarness { stallingHarness(&pty) };
    auto const sessionId = newSession(h.host);
    REQUIRE(pty != nullptr);
    auto const fetchUnknownImage = proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } };

    SECTION("a reply sent on the spot leaves the loop answering")
    {
        pty->stall();
        auto const exchange = exchangeWhileStalled(
            h,
            *pty,
            { proto::ClientHello {}, writeScreenRequest(sessionId.value, "\033[?996n"), fetchUnknownImage });
        CHECK(exchange.answered == std::future_status::ready);
        CHECK(std::ranges::any_of(exchange.received, isImageGone));
        CHECK(writtenOnceDone(*pty, [](auto const& written) {
                  return written.contains("\033[?997;");
              }).contains("\033[?997;"));
    }

    SECTION("a reply only queued reaches the application")
    {
        std::ignore = h.exchangeUntil(
            { proto::ClientHello {}, writeScreenRequest(sessionId.value, "\033[c"), fetchUnknownImage },
            [](auto const& got) { return std::ranges::any_of(got, isImageGone); });
        CHECK(writtenOnceDone(*pty, [](auto const& written) {
                  return written.starts_with("\033[?");
              }).starts_with("\033[?"));
    }

    SECTION("a reply goes out in order with the input around it")
    {
        // The pump parks writing the first input, and the write and the second input queue behind it.
        pty->stall();
        auto const exchange = exchangeWhileStalled(h,
                                                   *pty,
                                                   { proto::ClientHello {},
                                                     inputRequest(sessionId.value, "a"),
                                                     writeScreenRequest(sessionId.value, "\033[c"),
                                                     inputRequest(sessionId.value, "b"),
                                                     fetchUnknownImage });
        CHECK(exchange.answered == std::future_status::ready);
        auto const written =
            writtenOnceDone(*pty, [](auto const& written) { return written.ends_with("b"); });
        CHECK(written.starts_with("a\033[?"));
        CHECK(written.ends_with("cb"));
    }
}

TEST_CASE("a WriteScreen operation's smooth scrolling pauses the session's pump, not the loop",
          "[vthost][native]")
{
    // Under smooth scrolling (DECSCLM) every linefeed shows the screen so far and waits before the next line.
    // Parsed on the loop thread, WriteScreen's text slept there for each of its lines, and every client of
    // the daemon waited with it. The session's pump parses it, between two reads, as it parses the
    // application's output, and the loop answers what follows at once.
    auto constexpr Lines = 20;
    auto constexpr Pause = std::chrono::milliseconds { 250 };
    auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
    auto h = NativeHarness { stallingHarness(&pty) };
    auto const sessionId = newSession(h.host);
    REQUIRE(pty != nullptr);
    auto& terminal = *h.host.terminal(sessionId);
    // Under the lock, as a reload sets it: the parse reads it under the lock.
    auto const setPause = [&terminal](std::chrono::milliseconds pause) {
        auto const _ = std::scoped_lock { terminal };
        terminal.settings().smoothLineScrolling = pause;
    };
    setPause(Pause);

    auto const started = std::chrono::steady_clock::now();
    auto const received = h.exchangeUntil(
        { proto::ClientHello {},
          writeScreenRequest(sessionId.value, "\033[?4h" + std::string(std::size_t { Lines }, '\n')),
          proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } },
        [](auto const& got) { return std::ranges::any_of(got, isImageGone); });
    auto const answeredAfter = std::chrono::steady_clock::now() - started;
    setPause(std::chrono::milliseconds { 0 }); // the lines still to come need not wait

    CHECK(std::ranges::any_of(received, isImageGone));
    CHECK(answeredAfter < Pause * Lines);
}

TEST_CASE("an application's reply sent on the spot leaves the loop answering while it reads no input",
          "[vthost][native]")
{
    // Some sequences send their reply on the spot: a colour preference report (DSR 996), and the in-band
    // resize report as DECSET 2048 turns it on and as a resize changes what it reports. The pump wrote it
    // inside the parse, under the session's state lock, and a PTY write parks while the application reads
    // nothing: the loop then waited for that lock as soon as it built the session's delta, and every client
    // of the daemon waited with it. The pump writes it once the parse has let go of the lock, and the loop
    // answers: this session's delta, a resize, another session's input.
    struct OnTheSpot
    {
        std::string_view name;   ///< The sequence, for the section's name.
        std::string_view output; ///< What the application writes.
        std::string_view reply;  ///< What the reply it asks for contains.
    };
    auto constexpr Rows = std::array {
        OnTheSpot { .name = "DSR 996", .output = "\033[?996n", .reply = "\033[?997;" },
        OnTheSpot { .name = "DECSET 2048", .output = "\033[?2048h", .reply = "\033[48;" },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
            auto h = NativeHarness { stallingHarness(&pty) };
            auto const replyingId = newSession(h.host);
            auto* const replying = pty;
            auto const otherId = newSession(h.host);
            auto* const other = pty;
            REQUIRE(replying != nullptr);
            REQUIRE(other != replying);

            replying->stall();
            replying->sendOutput(row.output);
            REQUIRE(eventually([replying] { return replying->parkedWrites() == 1; }));

            auto const isReplyingDelta = [&replyingId](proto::DecodedFrame const& frame) {
                auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
                return delta != nullptr && delta->session == replyingId.value;
            };
            auto const exchange = exchangeWhileStalled(
                h,
                *replying,
                { proto::ClientHello {},
                  proto::DecodedPdu { proto::ResizeRequest { .columns = 100, .lines = 30 } },
                  inputRequest(otherId.value, "x"),
                  proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } },
                [&isReplyingDelta](auto const& got) {
                    return imageGoneArrived(got) && std::ranges::any_of(got, isReplyingDelta);
                });
            CHECK(exchange.answered == std::future_status::ready);
            CHECK(std::ranges::any_of(exchange.received, isReplyingDelta));
            CHECK(writtenOnceDone(*other, [](auto const& written) { return written == "x"; }) == "x");
            CHECK(writtenOnceDone(*replying, [&row](auto const& written) {
                      return written.contains(row.reply);
                  }).contains(row.reply));
        }
    }
}

TEST_CASE("FetchImage for an unknown id answers ImageGone", "[vthost][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    // ServerHello, LayoutState, then ImageGone, and only then the attach snapshot
    // (SessionState + Delta). The snapshot is QUEUED by the handshake and emitted by the streamer
    // on a later loop turn, so a request the client pipelined behind its hello is answered first.
    // Nothing depends on the old order: replies are correlated by serial, not by position — and
    // the client asserting its client area right after the hello now RESIZES before the snapshot
    // is captured, so the one grid it receives is already the right shape.
    auto const received =
        h.exchange({ proto::ClientHello {}, proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } }, 5);
    REQUIRE(received.size() == 5);
    auto const* gone = std::get_if<proto::ImageGone>(&received[2].pdu);
    REQUIRE(gone != nullptr);
    CHECK(gone->imageId == 4242);
    CHECK(received[2].serial == 2); // correlated to the request's serial
    CHECK(std::get_if<proto::SessionState>(&received[3].pdu) != nullptr);
    CHECK(std::get_if<proto::Delta>(&received[4].pdu) != nullptr);
}

TEST_CASE("a version-mismatched hello is answered and the session ends", "[vthost][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    auto const received = h.exchange({ proto::ClientHello { .codecVersion = 9999 } }, 2);
    // Only the ServerHello arrives — no snapshot follows a failed handshake.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ServerHello>(received[0].pdu));
}

namespace
{
/// @return The identity the hosts in the two cases below run on.
vtbackend::HostIdentity daemonIdentity()
{
    return vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };
}
} // namespace

TEST_CASE("an accepted hello names the daemon's machine, once for the connection", "[vthost][native]")
{
    // Every record's locality was judged against this machine, and a client must know whose "local" that
    // is before it mirrors the first one -- so the hello says it, and no snapshot repeats it.
    auto h = NativeHarness { HarnessOptions { .identity = daemonIdentity() } };
    h.host.createTab();

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->machineId == vthost::appSpecificMachineId(daemonIdentity().machineId));
    CHECK(hello->hostname == "zeta");

    // machine-id(5) is confidential: the id itself never travels, in no PDU, and never reaches a trace.
    auto pdus = std::vector<proto::DecodedPdu> {};
    for (auto const& frame: received)
    {
        CHECK_FALSE(proto::summarize(frame.pdu).contains(daemonIdentity().machineId));
        pdus.push_back(frame.pdu);
    }
    auto const wire = encodeRequest(pdus);
    auto const bytes = std::string_view { reinterpret_cast<char const*>(wire.data()), wire.size() };
    CHECK_FALSE(bytes.contains(daemonIdentity().machineId));
    CHECK(bytes.contains(hello->machineId));
}

TEST_CASE("a refused hello says nothing about the daemon's machine", "[vthost][native]")
{
    // The refusal is shaped to reveal nothing to a peer probing the daemon; which machine it runs on is
    // part of that nothing.
    auto h = NativeHarness { HarnessOptions { .identity = daemonIdentity() } };
    h.host.createTab();

    auto const received = h.exchange({ proto::ClientHello { .codecVersion = 9999 } }, 2);
    REQUIRE(received.size() == 1);
    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->machineId.empty());
    CHECK(hello->hostname.empty());
}

TEST_CASE("a context's machine id travels derived, never as the context named it", "[vthost][native]")
{
    // A context's machineid= is, as a rule, the id machine-id(5) asks to keep confidential, read on the
    // system its shell runs on: it travels derived for this application, as the daemon's own does. One that
    // is no machine id travels empty, and stays unknown.
    constexpr auto Named = std::string_view { "77aa5353d3ba43d08201c136a47ead7b" };
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(
        std::format("\033]3008;start=outer;type=session;machineid={};hostname=eta\033\\", Named));
    // A UUID spelled with dashes: an id OSC 3008 takes, but not as machine-id(5) spells one.
    h.host.terminal(sessionId)->writeToScreen(
        "\033]3008;start=inner;type=shell;"
        "machineid=3deb5353-d3ba-43d0-8201-c136a47ead7b;hostname=eta\033\\");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    auto const contextNamed = [&](std::string_view identifier) {
        auto const it = std::ranges::find(state->contexts, identifier, &proto::WireContext::identifier);
        REQUIRE(it != state->contexts.end());
        return *it;
    };
    CHECK(contextNamed("outer").machineId == vthost::appSpecificMachineId(Named));
    CHECK(contextNamed("outer").hostname == "eta");
    CHECK(contextNamed("inner").machineId.empty());

    auto pdus = std::vector<proto::DecodedPdu> {};
    for (auto const& frame: received)
        pdus.push_back(frame.pdu);
    auto const wire = encodeRequest(pdus);
    auto const bytes = std::string_view { reinterpret_cast<char const*>(wire.data()), wire.size() };
    CHECK_FALSE(bytes.contains(Named));
}

TEST_CASE("an alternate-screen flip forces a resync snapshot with SessionState", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    auto const received = h.attach(6, flipToAltScreen(&h, sessionId, &h.received));
    REQUIRE(received.size() == 6);

    // [0] ServerHello, [1] LayoutState, [2] SessionState(primary), [3] Delta,
    // [4] SessionState(alternate), [5] Delta.
    auto const* primary = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(primary != nullptr);
    CHECK(primary->screenType == std::to_underlying(vtbackend::ScreenType::Primary));

    // The flip is announced (SessionState is the only carrier of the screen
    // type) and served as a snapshot of the alternate grid — not diffed against
    // the primary grid's unrelated delta cursor.
    auto const* alt = std::get_if<proto::SessionState>(&received[4].pdu);
    REQUIRE(alt != nullptr);
    CHECK(alt->screenType == std::to_underlying(vtbackend::ScreenType::Alternate));

    auto const* delta = std::get_if<proto::Delta>(&received[5].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    CHECK(textOf(delta->lines.front()) == "ALT!");
}

TEST_CASE("a snapshot carries the input-encoding state, resets included", "[vthost][native]")
{
    // The two protocols that decide how a CLIENT encodes keys are pulled and diffed, and on a
    // snapshot they travel in SessionState rather than a Delta changed-flag. A snapshot must
    // therefore state them outright — including at their defaults, which is where an app lands
    // when it stops asking. The server records what it sent and never repeats it, so a default
    // omitted here is a default the client never hears about.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    // Negotiate both BEFORE attaching, so the attach snapshot has something to carry.
    h.host.terminal(sessionId)->writeToScreen("\033[>5u\033[>4;2m");

    auto const received = h.attach(6, flipToAltScreen(&h, sessionId, &h.received));
    REQUIRE(received.size() == 6);

    auto const* primary = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(primary != nullptr);
    CHECK(primary->kittyKeyboardFlags == 5);
    CHECK(primary->modifyOtherKeys == 2);

    // The alternate-screen flip snapshots again; the state must be restated, not assumed.
    auto const* alt = std::get_if<proto::SessionState>(&received[4].pdu);
    REQUIRE(alt != nullptr);
    CHECK(alt->kittyKeyboardFlags == 5);
    CHECK(alt->modifyOtherKeys == 2);
}

TEST_CASE("a burst that outran the scrollback floor is served as a snapshot", "[vthost][native]")
{
    // An incremental delta can only name rows back to the grid's scrollback floor
    // (Grid::scrolledOutDepthSince clamps there). When more rows scrolled off than the floor
    // still covers, reporting the remainder and staying quiet about the rest is worse than
    // resynchronizing: the client scrolls every unreported id through its page as a BLANK row,
    // fabricating scrollback the session never contained.
    auto h = NativeHarness { { .history = vtbackend::LineCount(10) } };
    auto const sessionId = newSession(h.host);

    // Far more rows than the 10-line floor can name, in one batch.
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, 100))
        lines += std::format("line-{}\r\n", i);

    auto const received = h.attach(
        6, writeOnce(&h, sessionId, std::move(lines), holdsFrames(&h.received, AttachSnapshotFrames)));
    REQUIRE(received.size() == 6);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta (the attach snapshot),
    // then the burst's [4] SessionState + [5] Delta. That the burst produced a SessionState
    // at all is already the tell: only a snapshot emits one.
    REQUIRE(std::get_if<proto::SessionState>(&received[4].pdu) != nullptr);
    auto const* delta = std::get_if<proto::Delta>(&received[5].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    // A snapshot carries the whole addressable grid: the page plus the history still held.
    CHECK(delta->lines.size() > 10);
}

TEST_CASE("a snapshot anchors the cursor so the following delta is incremental", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("first");

    auto const received = h.attach(5, appendThenUpdate(&h, sessionId, &h.received));
    REQUIRE(received.size() == 5);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta(snapshot),
    // [4] Delta(incremental).
    auto const* snapshot = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot == 1);

    auto const* incremental = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(incremental != nullptr);
    // Anchored past the snapshot: the follow-up is a real diff, not another resync
    // (a stale cursor would force snapshot==1) and not a rescan of every row.
    CHECK(incremental->snapshot == 0);
    REQUIRE(incremental->lines.size() == 1);

    auto const text = textOf(incremental->lines.front());
    CHECK(text.starts_with("first")); // the snapshot content is still there
    CHECK(text.contains("more"));     // with the newly appended bytes
}

TEST_CASE("a cursor-only move still produces a delta so the mirror's cursor tracks", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("first"); // leaves the cursor after the text

    auto const received = h.attach(5, moveCursorThenUpdate(&h, sessionId, &h.received));
    REQUIRE(received.size() == 5);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta(snapshot), [4] cursor-only move.
    // Without the cursor gate the [4] send is suppressed (no changed cell, no mode flip) and the
    // mirror's cursor would stay where the snapshot left it.
    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 0);
    CHECK(delta->cursorLine == 9);   // CUP row 10, 0-based
    CHECK(delta->cursorColumn == 4); // CUP column 5, 0-based
}

TEST_CASE("a scrollback eviction reaches the client though no row changed", "[vthost][native]")
{
    // `clear`/CSI 3 J (and tmux's clear-history) evicts the scrollback through Grid::clearHistory,
    // which deliberately bumps no generation and dirties no line — the floor is the ONLY signal.
    // Gated on what the delta itself knows, the PDU was dropped: the mirror's own detector for this
    // case (ScreenMirror's `floorOutranScroll`) waited on a delta that never arrived, so an attached
    // window kept rendering the entire scrollback the daemon had thrown away.
    auto h = NativeHarness { { .history = vtbackend::LineCount(10) } };
    auto const sessionId = newSession(h.host);
    // More than the 25-line page holds, so rows actually reach the scrollback: with no history at
    // all the eviction moves no floor and the test would assert nothing.
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, 40))
        lines += std::format("line-{}\r\n", i);
    h.host.terminal(sessionId)->writeToScreen(lines);
    REQUIRE(h.host.terminal(sessionId)->primaryScreen().grid().historyLineCount() > vtbackend::LineCount(0));

    auto const received = h.attach(5, clearHistoryThenUpdate(&h, sessionId, &h.received));
    REQUIRE(received.size() == 5);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta(snapshot), [4] the eviction.
    auto const* snapshot = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(snapshot != nullptr);
    auto const* evicted = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(evicted != nullptr);
    CHECK(evicted->lines.empty());                       // nothing was rewritten...
    CHECK(evicted->stableFloor > snapshot->stableFloor); // ...the floor alone moved

    // Each carries the host's eviction count, so the client numbers lines as the host does: the rows
    // scrolled away before the attach, then every row the clear dropped.
    CHECK(snapshot->evictedRowCount > 0);
    CHECK(evicted->evictedRowCount - snapshot->evictedRowCount
          == static_cast<uint64_t>(evicted->stableFloor - snapshot->stableFloor));
    CHECK(evicted->evictedRowCount == h.host.terminal(sessionId)->primaryScreen().grid().evictedRowCount());
}

TEST_CASE("a window-title change is carried in the following incremental delta", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("first");

    auto const received = h.attach(5, setTitleThenUpdate(&h, sessionId, &h.received));
    REQUIRE(received.size() == 5);

    // [4] the incremental delta carries the title, gated on the diff against the last-sent one
    // (windowTitle() is compared as a string_view, so an UNCHANGED title allocates nothing).
    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 0);
    CHECK(delta->titleChanged == 1);
    CHECK(delta->title == "my-title");
}

TEST_CASE("an OSC 30 tab-name change is carried in the following incremental delta", "[vthost][native]")
{
    // Issue #2170: a client's tab bar shows the application's tab name, so the host must send it as it
    // sends the window title -- gated on the diff, so an unchanged name costs two bytes, not the string.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("first");

    auto const received = h.attach(
        5, writeOnce(&h, sessionId, "\033]30;my-tab\033\\", holdsFrames(&h.received, AttachSnapshotFrames)));
    REQUIRE(received.size() == 5);

    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 0);
    CHECK(delta->tabNameChanged == 1);
    CHECK(delta->tabName == "my-tab");
    CHECK(delta->titleChanged == 0); // the window title did not move
}

TEST_CASE("an attach snapshot states the session's OSC 30 tab name", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("\033]30;my-tab\033\\");

    auto const received = h.attach(AttachSnapshotFrames);
    REQUIRE(received.size() == AttachSnapshotFrames);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->tabName == "my-tab");
}

namespace
{
/// The ProgramStatus frames among @p frames.
std::vector<proto::ProgramStatus> programStatusFrames(std::vector<proto::DecodedFrame> const& frames)
{
    auto found = std::vector<proto::ProgramStatus> {};
    for (auto const& frame: frames)
        if (auto const* status = std::get_if<proto::ProgramStatus>(&frame.pdu))
            found.push_back(*status);
    return found;
}

/// A hello from a viewer that mirrors program status.
proto::ClientHello programStatusHello()
{
    return proto::ClientHello { .capabilities =
                                    proto::CapabilitySet::of({ proto::Capability::ProgramStatus }) };
}

/// Once @p gate holds: a program reporting in a loop, @p bursts times ten fresh ids of maximum-size records,
/// one burst every 30 ms -- longer than the debounce, so every burst earns a flush of its own.
Task<void> reportInBursts(NativeHarness* h, vtworkspace::SessionId id, int bursts, StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    auto const encoded = core::base64::encode(std::string(2048, 'x')); // the longest message there is
    for (auto const burst: std::views::iota(0, bursts))
    {
        auto bytes = std::string {};
        for (auto const index: std::views::iota(0, 10))
            bytes +=
                std::format("\033]7501;state=working:id=job{}:msg={}\033\\", (burst * 10) + index, encoded);
        h->host.terminal(id)->writeToScreen(bytes);
        co_await h->loop.delay(30ms);
    }
}

/// Reads nothing for @p stall, then collects until @p done holds.
Task<void> stallThenCollect(NativeHarness* h,
                            std::chrono::milliseconds stall,
                            CollectDone done,
                            std::vector<proto::DecodedFrame>* out)
{
    co_await h->loop.delay(stall);
    co_await collectUntil(&h->loop, h->pair.second.get(), std::move(done), out);
}
} // namespace

TEST_CASE("a viewer sharing ProgramStatus receives the set", "[vthost][native][programstatus]")
{
    auto const hasProgramStatus = [](auto const& got) {
        return std::ranges::any_of(got, [](proto::DecodedFrame const& frame) {
            return std::holds_alternative<proto::ProgramStatus>(frame.pdu);
        });
    };

    SECTION("a report after the attach arrives with the next flush, once")
    {
        auto h = NativeHarness {};
        auto const sessionId = newSession(h.host);
        auto const received = h.exchangeUntil(
            { proto::DecodedPdu { programStatusHello() } },
            [&](auto const& got) {
                // The set, and the increment the text written beside it earns, both from one flush.
                return hasProgramStatus(got) && got.size() >= AttachSnapshotFrames + 2;
            },
            writeOnce(
                &h,
                sessionId,
                "\033]7501;state=working:app=brew:progress=30\033\\\033]7501;state=working:app=brew:progress="
                "40\033\\text",
                holdsFrames(&h.received, AttachSnapshotFrames)));
        auto const sets = programStatusFrames(received);
        // Two reports inside one coalescing window are one set, read when it goes: the newest.
        REQUIRE(sets.size() == 1);
        CHECK(sets.front().session == sessionId.value);
        REQUIRE(sets.front().records.size() == 1);
        CHECK(sets.front().records.front().state == std::to_underlying(vtbackend::ProgramState::Working));
        CHECK(sets.front().records.front().app == "brew");
        CHECK(sets.front().records.front().progress == 40);
    }

    SECTION("the timing block rides only a viewer that shares ProgramStatusTiming")
    {
        auto const timingHello =
            proto::ClientHello { .capabilities =
                                     proto::CapabilitySet::of({ proto::Capability::ProgramStatus,
                                                                proto::Capability::ProgramStatusTiming }) };
        auto h = NativeHarness {};
        auto const sessionId = newSession(h.host);
        auto const received = h.exchangeUntil(
            { proto::DecodedPdu { timingHello } },
            [&](auto const& got) { return hasProgramStatus(got); },
            writeOnce(&h,
                      sessionId,
                      "\033]7501;state=working\033\\",
                      holdsFrames(&h.received, AttachSnapshotFrames)));
        auto const sets = programStatusFrames(received);
        REQUIRE(sets.size() == 1);
        CHECK(sets.front().timing == proto::StatusTimingBlock::Included);
        REQUIRE(sets.front().records.size() == 1);
        CHECK(sets.front().records.front().generation != 0);
        CHECK(sets.front().records.front().sinceNs != 0);
    }

    SECTION("a viewer without the capability gets none")
    {
        auto h = NativeHarness { HarnessOptions { .collectBudget = std::chrono::milliseconds { 300 } } };
        auto const sessionId = newSession(h.host);
        auto const received = h.exchangeUntil({ proto::DecodedPdu { proto::ClientHello {} } },
                                              hasProgramStatus,
                                              writeOnce(&h,
                                                        sessionId,
                                                        "\033]7501;state=working\033\\",
                                                        holdsFrames(&h.received, AttachSnapshotFrames)));
        CHECK(received.size() >= AttachSnapshotFrames);
        CHECK(programStatusFrames(received).empty());
    }

    SECTION("a viewer attaching later gets the current set in its snapshot")
    {
        auto h = NativeHarness {};
        auto const sessionId = newSession(h.host);
        h.host.terminal(sessionId)->writeToScreen("\033]7501;state=done:id=build:app=make\033\\");
        // ServerHello, LayoutState, SessionState, ProgramStatus, then the rows.
        auto const received =
            h.exchange({ proto::DecodedPdu { programStatusHello() } }, AttachSnapshotFrames + 1);
        REQUIRE(received.size() == AttachSnapshotFrames + 1);
        auto const* status = std::get_if<proto::ProgramStatus>(&received[3].pdu);
        REQUIRE(status != nullptr);
        REQUIRE(status->records.size() == 1);
        CHECK(status->records.front().id == "build");
        CHECK(std::get_if<proto::Delta>(&received[4].pdu) != nullptr);
    }

    SECTION("an empty set says nothing in a snapshot")
    {
        auto h = NativeHarness {};
        std::ignore = newSession(h.host);
        auto const received =
            h.exchange({ proto::DecodedPdu { programStatusHello() } }, AttachSnapshotFrames);
        CHECK(programStatusFrames(received).empty());
    }
}

namespace
{
/// The SessionInputEcho frames among @p frames, and where each stands.
std::vector<std::pair<std::size_t, proto::SessionInputEcho>> inputEchoFrames(
    std::vector<proto::DecodedFrame> const& frames)
{
    auto found = std::vector<std::pair<std::size_t, proto::SessionInputEcho>> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, frames.size()))
        if (auto const* echo = std::get_if<proto::SessionInputEcho>(&frames[index].pdu))
            found.emplace_back(index, *echo);
    return found;
}

/// How many Delta frames @p frames holds.
std::size_t deltaCount(std::vector<proto::DecodedFrame> const& frames)
{
    return static_cast<std::size_t>(std::ranges::count_if(frames, [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::Delta>(frame.pdu);
    }));
}

/// A hello from a viewer that mirrors the echo state.
proto::ClientHello inputEchoHello()
{
    return proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::InputEcho }) };
}

/// The hosted session @p id's mock PTY.
vtpty::MockPty& mockPtyOf(NativeHarness& h, vtworkspace::SessionId id)
{
    // The host taps every device it makes; the factory's MockPty is the tap's inner one.
    return dynamic_cast<vtpty::MockPty&>(
        dynamic_cast<vthost::TappingPty&>(h.host.terminal(id)->device()).inner());
}

/// Writes @p bytes into @p id and kicks the debounced flush, then waits past the debounce, so the next step
/// earns a flush of its own.
Task<void> writeAndSettle(NativeHarness* h, vtworkspace::SessionId id, std::string bytes)
{
    h->host.terminal(id)->writeToScreen(bytes);
    h->session->sessionScreenUpdated(id);
    co_await h->loop.delay(30ms);
}

/// Once @p gate holds: a password prompt as sudo shows one -- echo off, then the prompt; more output while
/// it waits; echo back on, then the newline that ends it; and output after.
Task<void> promptAndAnswer(NativeHarness* h, vtworkspace::SessionId id, StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    mockPtyOf(*h, id).setInputEcho(vtpty::InputEcho::Hidden);
    co_await writeAndSettle(h, id, "Password:");
    co_await writeAndSettle(h, id, "*");
    mockPtyOf(*h, id).setInputEcho(vtpty::InputEcho::Echoed);
    co_await writeAndSettle(h, id, "\r\n");
    co_await writeAndSettle(h, id, "$ ");
}
} // namespace

TEST_CASE("a viewer sharing InputEcho is told what the hosted terminal does with typed text",
          "[vthost][native][inputecho]")
{
    constexpr auto Writes = std::size_t { 4 };

    SECTION("each change once, before the output that follows it, and nothing while unchanged")
    {
        auto h = NativeHarness {};
        auto const sessionId = newSession(h.host);
        auto const received = h.exchangeUntil(
            { proto::DecodedPdu { inputEchoHello() } },
            [&](auto const& got) { return deltaCount(got) >= 1 + Writes; },
            promptAndAnswer(&h, sessionId, holdsFrames(&h.received, AttachSnapshotFrames)));
        auto const echoes = inputEchoFrames(received);
        REQUIRE(echoes.size() == 2);
        CHECK(echoes[0].second.session == sessionId.value);
        CHECK(echoes[0].second.echo == std::to_underlying(vtpty::InputEcho::Hidden));
        CHECK(echoes[1].second.echo == std::to_underlying(vtpty::InputEcho::Echoed));
        // Ahead of the prompt's delta, so a client's mirror has the state when the output lands.
        REQUIRE(echoes[0].first + 1 < received.size());
        CHECK(std::holds_alternative<proto::Delta>(received[echoes[0].first + 1].pdu));
    }

    SECTION("a viewer without the capability gets none")
    {
        auto h = NativeHarness {};
        auto const sessionId = newSession(h.host);
        auto const received = h.exchangeUntil(
            { proto::DecodedPdu { proto::ClientHello {} } },
            [&](auto const& got) { return deltaCount(got) >= 1 + Writes; },
            promptAndAnswer(&h, sessionId, holdsFrames(&h.received, AttachSnapshotFrames)));
        CHECK(deltaCount(received) >= 1 + Writes);
        CHECK(inputEchoFrames(received).empty());
    }

    SECTION("a viewer attaching to a waiting prompt is told in its snapshot")
    {
        auto h = NativeHarness {};
        auto const sessionId = newSession(h.host);
        mockPtyOf(h, sessionId).setInputEcho(vtpty::InputEcho::Hidden);
        auto const received =
            h.exchange({ proto::DecodedPdu { inputEchoHello() } }, AttachSnapshotFrames + 1);
        auto const echoes = inputEchoFrames(received);
        REQUIRE(echoes.size() == 1);
        CHECK(echoes[0].second.echo == std::to_underlying(vtpty::InputEcho::Hidden));
    }
}

TEST_CASE("program status waits for a client that stopped reading instead of costing the connection",
          "[vthost][native][programstatus]")
{
    // A full set of maximum-size records is twice this queue's bound. Pushed every debounce window
    // regardless, a client that reads nothing for a while would have the sets fill the pipe and then the
    // backlog, until the queue refused one and the connection went. Gated on room, they wait; the newest set
    // arrives once the client reads again, and the viewer is not otherwise affected.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    auto const sessionId = newSession(h.host);
    auto const hello = encodeRequest({ proto::DecodedPdu { programStatusHello() } });
    auto const newestArrived = [](std::vector<proto::DecodedFrame> const& got) {
        auto const sets = programStatusFrames(got);
        return !sets.empty() && sets.back().records.size() == vtbackend::MaxProgramStatusRecords
               && sets.back().records.back().id == "job199";
    };
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(
        core::net::testing::allOf(h.session->run(),
                                  feedBytes(h.pair.second.get(), &hello),
                                  stallThenCollect(&h, 800ms, newestArrived, &received),
                                  reportInBursts(&h, sessionId, 20, followsSession(&h, sessionId))));

    CHECK_FALSE(capture.contains("send queue overflow"));
    CHECK(newestArrived(received));
    // The host never held more than its cap, whatever the program sent.
    CHECK(h.host.terminal(sessionId)->programStatus().records().size() == vtbackend::MaxProgramStatusRecords);
}

TEST_CASE("a hyperlink URI is delivered on first reference", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    // OSC 8 hyperlink around "link"; the URI must ride the snapshot's side table.
    h.host.terminal(sessionId)->writeToScreen("\033]8;;https://contour.example\033\\link\033]8;;\033\\");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    REQUIRE(delta->hyperlinks.size() == 1);
    CHECK(delta->hyperlinks.front().uri == "https://contour.example");
}

TEST_CASE("a debounce flush pending at disconnect resolves before run() returns", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    std::ignore = h.attach(4, kickThenDisconnect(&h, sessionId));

    // The daemon frees the session the moment run() returns; a flush coroutine
    // still parked in its debounce delay would then resume on freed memory
    // (ASan turns that into a hard failure right here).
    h.session.reset();
    h.loop.blockOn(core::net::testing::sleepFor(&h.loop, 30ms));
    SUCCEED("the debounce flush settled before the session was destroyed");
}

namespace
{

/// Sends @p hello and decodes the attach snapshot, leaving the connection open.
Task<void> attachAndStay(NativeHarness* h, std::vector<std::byte> const* hello)
{
    auto frames = std::vector<proto::DecodedFrame> {};
    co_await feedBytes(h->pair.second.get(), hello);
    co_await core::net::testing::anyOf(
        decodeUntil(h->pair.second.get(),
                    [](auto const& got) { return got.size() >= AttachSnapshotFrames; },
                    &frames,
                    {}),
        core::net::testing::sleepFor(&h->loop, CollectBudget));
}

/// Runs @p session to its end, however it ends, and records how many flushes it left behind then: the
/// daemon frees a connection the moment its run() ends.
Task<void> runThenCountFlushes(NativeSession* session, std::optional<std::size_t>* flushesLeft)
{
    // However run() ends: a stopped loop ends it by throwing.
    auto const count = core::Finally { [session, flushesLeft]() noexcept {
        *flushesLeft = vthost::NativeSessionFlushTester::flushesInFlight(*session);
    } };
    co_await session->run();
}

} // namespace

TEST_CASE("a flow the loop frees before it ever ran gives back its claim on the connection",
          "[vthost][native]")
{
    // Otherwise run()'s barrier would wait for a flow that is gone, and the connection would never end.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    CHECK(vthost::NativeSessionFlushTester::unstartedFlowsGiveBackClaims(*h.session, sessionId));
}

TEST_CASE("a stopped loop ends a connection only once the flushes it armed have", "[vthost][native]")
{
    auto options = HarnessOptions {};
    // Long enough that the flush is still asleep when the loop stops.
    options.services.flush.coalesceDelay = std::chrono::hours { 1 };
    auto h = NativeHarness { std::move(options) };
    auto const sessionId = newSession(h.host);
    auto flushesLeft = std::optional<std::size_t> {};
    h.loop.spawn(runThenCountFlushes(h.session.get(), &flushesLeft));
    auto const hello = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    h.loop.blockOn(attachAndStay(&h, &hello));
    // The first increment after the snapshot goes at once; the second coalesces, and sleeps.
    for ([[maybe_unused]] auto const update: std::views::iota(0, 2))
    {
        h.host.terminal(sessionId)->writeToScreen("output");
        h.session->sessionScreenUpdated(sessionId);
        h.loop.blockOn(core::net::testing::sleepFor(&h.loop, 10ms));
    }
    CHECK(vthost::NativeSessionFlushTester::flushesInFlight(*h.session) > 0);

    // Every wait throws on a stopped loop, the connection's barrier included; a flush it armed must not
    // outlive it all the same, or the daemon frees the session under the flush.
    h.loop.requestStop();
    {
        auto const drain = vthost::LoopDrain { h.loop };
    }
    REQUIRE(flushesLeft.has_value());
    CHECK(*flushesLeft == 0);
}

TEST_CASE("a closed session's follow state is pruned", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    // The handshake snapshot makes the session followed (pushDelta seeds _followed).
    std::ignore = h.exchange({ proto::ClientHello {} }, 3);
    REQUIRE(vthost::NativeSessionFollowTester::follows(*h.session, sessionId));

    // The host learning its own session closed must fan out to stream subscribers, the attached
    // connection among them, and the session must drop the per-session follow state (otherwise it
    // leaks for the connection's whole lifetime -- one entry per session ever opened).
    h.host.handleSessionExit(sessionId);

    CHECK_FALSE(vthost::NativeSessionFollowTester::follows(*h.session, sessionId));
    CHECK(vthost::NativeSessionFollowTester::followedCount(*h.session) == 0);
}

TEST_CASE("a client that overflows the write queue is disconnected", "[vthost][native]")
{
    auto done = false;
    auto watchdogFired = false;
    // No reply fits an 8-byte bound: the very first send overflows, and the
    // session must apply the queue's disconnect contract instead of silently
    // under-serving the client from then on.
    auto h = NativeHarness { { .writeQueueBytes = 8 } };
    h.host.createTab();

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(runThenMark(h.session.get(), &done),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             collectPdus(&h.loop, h.pair.second.get(), 1, &received),
                                             closeWatchdog(&h, &done, &watchdogFired)));

    CHECK(!watchdogFired); // the SESSION closed the connection, not the watchdog
    CHECK(received.empty());
}

TEST_CASE("attaching to several scrollback-heavy panes is not refused", "[vthost][native]")
{
    // THE regression this whole pacing mechanism exists for. A daemon holding a couple of panes
    // with real scrollback became permanently unattachable: completeHandshake pushed one full-grid
    // snapshot per leaf pane synchronously, inside the PDU pump's handler, so the drain coroutine
    // -- which EventLoop::spawn only queues -- could not run until the burst finished. The whole
    // attach payload therefore had to fit under the send-queue bound at once, the second pane's
    // snapshot did not fit, and the connection was dropped before a single byte reached the client.
    // Nothing on the daemon changed between attempts, so every reconnect reproduced it exactly.
    auto capture = core::log::ScopedCapture {};

    // A bound far smaller than the snapshots it must carry, which is the shape of the report:
    // 4 MiB against two panes of ~1.8 MB each. The point is that the SUM exceeding the bound must
    // no longer matter, because a run never has more than two pieces in the queue.
    auto h = NativeHarness { { .writeQueueBytes = std::size_t { 64 } * 1024,
                               .history = vtbackend::LineCount(30) } };

    auto sessions = std::vector<vtworkspace::SessionId> {};
    for (auto const tab: std::views::iota(0, 3))
    {
        auto const id = newSession(h.host);
        sessions.push_back(id);
        fillRows(h, id, 50, std::format("tab{}-row", tab));
    }

    // ServerHello, LayoutState, then per pane a SessionState and a run of however many pieces its
    // rows partition into under this bound. The runs go out in tab order, so the last pane's run
    // ending is the attach ending.
    auto const lastRunEnded = [last = sessions.back()](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, [last](proto::DecodedFrame const& frame) {
            return endsSnapshotRun(frame) && std::get<proto::Delta>(frame.pdu).session == last.value;
        });
    };
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(
        core::net::testing::allOf(h.session->run(),
                                  feedBytes(h.pair.second.get(), &bytes),
                                  collectUntil(&h.loop, h.pair.second.get(), lastRunEnded, &received)));
    CHECK_FALSE(capture.contains("send queue overflow"));

    // Every pane arrived, and arrived whole: a snapshot that is not superseded and not truncated.
    for (auto const& frame: received)
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
            CHECK(delta->snapshot == 1);
    for (auto const tab: std::views::iota(0, 3))
    {
        auto const run = receivedRun(received, sessions[static_cast<std::size_t>(tab)]);
        auto const carries = [&](std::string_view row) {
            return std::ranges::any_of(run.rows,
                                       [row](auto const& line) { return textOf(line).starts_with(row); });
        };
        // The oldest row is scrollback that predates the attach; the newest is on the page.
        CHECK(carries(std::format("tab{}-row-0x", tab)));
        CHECK(carries(std::format("tab{}-row-49x", tab)));
    }
}

TEST_CASE("a snapshot larger than one frame travels as a marked run", "[vthost][native]")
{
    // The other cliff: one pane's snapshot is the whole grid INCLUDING all scrollback, built into
    // a single PDU. Past proto::MaxFrameSize the peer's own decoder rejects it, so no amount of
    // pacing would deliver it. It is split instead, and each piece names its place so the mirror
    // knows which one may clear what it holds and which one completes the run.
    auto h = NativeHarness { { .history = vtbackend::LineCount(4000) } };
    auto const sessionId = newSession(h.host);

    fillRows(h, sessionId, 4000, "row");

    // ServerHello, LayoutState, SessionState, then however many pieces the rows partition into.
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(driveSnapshotRun(&h.loop, h.session.get(), h.pair.second.get(), &bytes, &received));

    auto pieces = std::vector<proto::Delta> {};
    for (auto const& frame: received)
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
            pieces.push_back(*delta);
    REQUIRE(pieces.size() >= 2);

    // Every piece says it is a snapshot -- a piece must never be mistaken for an increment -- and
    // the run is marked First, then Middle, then Last.
    for (auto const& piece: pieces)
        CHECK(piece.snapshot == 1);
    CHECK(pieces.front().part() == proto::SnapshotPart::First);
    CHECK(pieces.back().part() == proto::SnapshotPart::Last);
    for (auto const& middle: pieces | std::views::drop(1) | std::views::take(pieces.size() - 2))
        CHECK(middle.part() == proto::SnapshotPart::Middle);

    // No piece is anywhere near the frame cap, and the rows are partitioned across them rather
    // than repeated: the run delivers each row exactly once.
    auto seen = std::set<int64_t> {};
    auto total = std::size_t { 0 };
    for (auto const& piece: pieces)
    {
        for (auto const& line: piece.lines)
            seen.insert(line.stableId);
        total += piece.lines.size();
    }
    CHECK(seen.size() == total);
    // Every addressable row travelled exactly once: the 4000 rows written, plus the empty row the
    // cursor sits on after the last CRLF. None is dropped at a piece boundary and none is repeated.
    CHECK(total == 4001);

    // The production bound cuts at SnapshotChunkBytes: a quarter of it is more than that.
    auto const run = receivedRun(received, sessionId);
    CHECK(run.rowsPerPiece == rowsPerPiece(run.rows, NativeSession::SnapshotChunkBytes));
}

TEST_CASE("an increment is held back until the snapshot it would interleave with finishes",
          "[vthost][native]")
{
    // A snapshot travels as a run of PDUs, and the mirror only repaints when the run completes.
    // An incremental delta slipped BETWEEN two pieces would be published immediately, having the
    // client repaint a grid it has only half received -- every row the run has not delivered yet
    // rendering as absent. So a session with a snapshot queued or in flight gets no increment; it
    // stays pending and goes out once the run is done.
    auto h = NativeHarness { { .writeQueueBytes = std::size_t { 64 } * 1024,
                               .history = vtbackend::LineCount(4000) } };
    auto const sessionId = newSession(h.host);

    fillRows(h, sessionId, 4000, "row");

    // Output arriving while the attach run is still streaming: the debounce fires 20ms later,
    // well inside a run that pauses for the write queue to drain between each of its several hundred
    // pieces.
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(
        driveSnapshotRun(&h.loop, h.session.get(), h.pair.second.get(), &bytes, &received, 1),
        appendThenUpdate(&h, sessionId, &received)));

    // The run is intact: every Delta up to and including the terminating piece is a snapshot
    // piece, with no increment wedged among them.
    auto sawRunEnd = false;
    auto increments = 0;
    for (auto const& frame: received)
    {
        auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
        if (delta == nullptr)
            continue;
        if (delta->snapshot == 0)
        {
            CHECK(sawRunEnd); // the whole point: never before the run ended
            ++increments;
            continue;
        }
        CHECK_FALSE(sawRunEnd);
        sawRunEnd = delta->completesSnapshot();
    }
    CHECK(sawRunEnd);
    // And the increment was held, not dropped: the rows written mid-run still arrive.
    CHECK(increments == 1);
}

TEST_CASE("a session's held increment follows its snapshot run without costing the connection",
          "[vthost][native]")
{
    // The increment held back for a whole run is released as the run ends, while a slow client
    // still has the run's tail in the send queue, and it must land beside that tail. It once did
    // not, twice over: a piece cut at a fixed size waited in the backlog already over a small
    // bound, and the session was let go the moment its last piece was enqueued rather than once
    // the backlog was back under a piece. Either way the queue refused the increment and the
    // connection went, with the run's last piece. A client that pauses before every read keeps the
    // run's tail in the queue across several debounce windows, so the release always meets it.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    auto const sessionId = newSession(h.host);
    fillRows(h, sessionId, 250, "row");

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(
        driveSnapshotRun(
            &h.loop, h.session.get(), h.pair.second.get(), &bytes, &received, 1, slowReader(h.loop)),
        appendThenUpdate(&h, sessionId, &received)));

    CHECK_FALSE(capture.contains("send queue overflow"));
    // A run of several pieces, so the increment was held across one rather than beside a lone piece...
    CHECK(receivedRun(received, sessionId).rowsPerPiece.size() > 1);
    auto const last = std::ranges::find_if(received, endsSnapshotRun);
    REQUIRE(last != received.end());
    // ...and the increment released behind it arrived instead of costing the connection.
    CHECK(std::ranges::count_if(std::next(last), received.end(), isIncrement) == 1);
}

TEST_CASE("another session's increment during a snapshot run does not cost the connection",
          "[vthost][native]")
{
    // A DIFFERENT session's increment meets the run's backlog: one whose own run is over, writing
    // output while the next session's run is still streaming. Nothing holds it back -- the run
    // says nothing about it -- so it is enqueued beside whatever part of the run is waiting, and
    // that part must leave it room. The slow reader keeps the second session's pieces queued across
    // several debounce windows.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    // The attach streams tabs in order, so the quiet session's run is over before the large one's
    // begins.
    auto const quiet = newSession(h.host);
    auto const large = newSession(h.host);
    fillRows(h, large, 250, "row");

    auto const endsLargeRun = [large](proto::DecodedFrame const& frame) {
        return endsSnapshotRun(frame) && std::get<proto::Delta>(frame.pdu).session == large.value;
    };
    auto const isQuietIncrement = [quiet](proto::DecodedFrame const& frame) {
        return isIncrement(frame) && std::get<proto::Delta>(frame.pdu).session == quiet.value;
    };
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    auto const largeRunAndQuietIncrement = [&](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, endsLargeRun) && std::ranges::any_of(got, isQuietIncrement);
    };
    h.loop.blockOn(core::net::testing::allOf(
        h.session->run(),
        feedBytes(h.pair.second.get(), &bytes),
        collectUntil(&h.loop, h.pair.second.get(), largeRunAndQuietIncrement, &received, slowReader(h.loop)),
        appendThenUpdate(&h, quiet, &received)));

    CHECK_FALSE(capture.contains("send queue overflow"));
    // The large run arrived whole, as several pieces...
    CHECK(std::ranges::any_of(received, endsLargeRun));
    CHECK(receivedRun(received, large).rowsPerPiece.size() > 1);
    // ...and the quiet session's output still reached the client.
    CHECK(std::ranges::count_if(received, isQuietIncrement) == 1);
}

TEST_CASE("snapshotPieceBytes is a quarter of the send-queue bound up to SnapshotChunkBytes",
          "[vthost][native]")
{
    // The production bound keeps the cap, so its snapshots are cut exactly where they always were.
    STATIC_CHECK(NativeSession::snapshotPieceBytes(NativeSession::DefaultWriteQueueBytes)
                 == NativeSession::SnapshotChunkBytes);
    CHECK(NativeSession::snapshotPieceBytes(NativeSession::SnapshotChunkBytes * 8)
          == NativeSession::SnapshotChunkBytes);
    CHECK(NativeSession::snapshotPieceBytes(SmallQueueBytes) == SmallQueueBytes / 4);
    CHECK(NativeSession::snapshotPieceBytes(8) == 2);
    // Never zero: it is also the streamer's watermark, and a backlog never gets below zero bytes.
    CHECK(NativeSession::snapshotPieceBytes(0) == 1);
}

TEST_CASE("under a small send-queue bound a snapshot is cut into quarter-bound pieces", "[vthost][native]")
{
    // A piece cut at SnapshotChunkBytes under a bound a quarter of that size waits in the backlog
    // already over the bound, and refuses whatever is enqueued behind it. Cut at a quarter of the
    // bound, a run never leaves more than half the bound behind it.
    auto h = NativeHarness { smallQueueOptions() };
    auto const sessionId = newSession(h.host);
    fillRows(h, sessionId, 250, "row");

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(driveSnapshotRun(&h.loop, h.session.get(), h.pair.second.get(), &bytes, &received));

    auto const run = receivedRun(received, sessionId);
    CHECK(run.rowsPerPiece == rowsPerPiece(run.rows, SmallQueueBytes / 4));
}

TEST_CASE("a session event sent during a snapshot run does not cost the connection", "[vthost][native]")
{
    // A bell or a notification cannot pace itself and is not held back: it is enqueued the moment
    // it happens, beside whatever part of a run is waiting in the backlog. That part has to leave
    // it room, or the event is refused and the connection goes with it.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    auto const sessionId = newSession(h.host);
    fillRows(h, sessionId, 250, "row");

    auto const isBell = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::SessionBell>(frame.pdu);
    };
    auto const isNotify = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::SessionNotify>(frame.pdu);
    };
    auto const runAndEvents = [&](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, endsSnapshotRun) && std::ranges::any_of(got, isBell)
               && std::ranges::any_of(got, isNotify);
    };
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(
        h.session->run(),
        feedBytes(h.pair.second.get(), &bytes),
        collectUntil(&h.loop, h.pair.second.get(), runAndEvents, &received, slowReader(h.loop)),
        ringWhileStreaming(&h, sessionId, &received)));

    CHECK_FALSE(capture.contains("send queue overflow"));
    CHECK(std::ranges::any_of(received, endsSnapshotRun));
    CHECK(std::ranges::any_of(received, isBell));
    CHECK(std::ranges::any_of(received, isNotify));
}

TEST_CASE("other panes' output during a long snapshot run does not pile up into an overflow",
          "[vthost][native]")
{
    // Two panes keep writing while a third pane's run streams. Their increments must go out as
    // they are made, a debounce window at a time, each landing beside at most two pieces of
    // backlog. Held back until the run is over instead, a whole run's worth of two panes' output
    // is released at once -- and together it is more than the bound.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    // The attach streams tabs in order: the two quiet panes first, then the large one.
    auto const quietOne = newSession(h.host);
    auto const quietTwo = newSession(h.host);
    auto const quiet = std::vector { quietOne, quietTwo };
    auto const large = newSession(h.host);
    fillRows(h, large, 600, "row");

    auto const endsLargeRun = [large](proto::DecodedFrame const& frame) {
        return endsSnapshotRun(frame) && std::get<proto::Delta>(frame.pdu).session == large.value;
    };
    // Whether the row each quiet pane writes last has reached the client.
    auto const sawEnd = [](std::vector<proto::DecodedFrame> const& got, vtworkspace::SessionId id) {
        return std::ranges::any_of(got, [id](proto::DecodedFrame const& frame) {
            auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
            return delta != nullptr && delta->snapshot == 0 && delta->session == id.value
                   && std::ranges::any_of(delta->lines,
                                          [](auto const& line) { return textOf(line).starts_with("end-0"); });
        });
    };
    auto const done = [&](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, endsLargeRun) && sawEnd(got, quietOne) && sawEnd(got, quietTwo);
    };
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(
        h.session->run(),
        feedBytes(h.pair.second.get(), &bytes),
        collectUntil(&h.loop, h.pair.second.get(), done, &received, slowReader(h.loop)),
        writeBursts(&h, &quiet, 12, 30ms)));

    CHECK_FALSE(capture.contains("send queue overflow"));
    CHECK(std::ranges::any_of(received, endsLargeRun));
    CHECK(sawEnd(received, quietOne));
    CHECK(sawEnd(received, quietTwo));
}

// ---------------------------------------------------------------------------
// Diagnostics. Every failure below was completely SILENT before the logging
// pass: the daemon dropped the connection and said nothing, which is precisely
// the class of bug these regression tests exist to prevent recurring.

namespace
{
/// Collects every byte the server writes until it closes the connection, or CollectBudget runs out.
Task<void> readUntilClosed(core::net::EventLoop* loop,
                           core::net::ISocket* client,
                           std::vector<std::byte>* out)
{
    auto const drain = [](core::net::ISocket* socket, std::vector<std::byte>* bytes) -> Task<void> {
        auto scratch = std::vector<std::byte>(4096);
        while (true)
        {
            auto const n = co_await socket->read(scratch);
            if (!n.has_value() || *n == 0)
                co_return;
            bytes->insert(bytes->end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
        }
    };
    co_await core::net::testing::anyOf(drain(client, out), core::net::testing::sleepFor(loop, CollectBudget));
    client->close();
}
} // namespace

TEST_CASE("a client of contour 0.7.0 is refused with a ServerHello it reads", "[vthost][native][handshake]")
{
    // That client reads a ServerHello as its version alone and refuses a byte more: a refusal it cannot read
    // drops it with a decode error, where one it reads tells its user which revision the daemon speaks.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness {};
    h.host.createTab();
    auto const hello = vthost::testing::shippedClientHello(1);
    auto reply = std::vector<std::byte> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &hello),
                                             readUntilClosed(&h.loop, h.pair.second.get(), &reply)));

    auto const frame = proto::readFrame(reply);
    REQUIRE(frame.has_value());
    CHECK(frame->ident == vthost::testing::ShippedServerHelloTag);
    CHECK(vthost::testing::shippedServerHelloVersion(frame->body) == proto::NegotiatingHelloMarker);
    CHECK(frame->consumed == reply.size()); // and nothing follows it: no snapshot for a refused client
    CHECK(capture.contains(std::format("the client speaks protocol {}, from a release before",
                                       vthost::testing::ShippedProtocolVersion)));
}

namespace
{
/// Feeds @p hello to a fresh session and reads everything it writes until it closes.
/// @param hello The ClientHello frame's bytes.
/// @param options What the session's harness is built with.
/// @return The bytes the session wrote.
std::vector<std::byte> answerTo(std::vector<std::byte> const& hello, HarnessOptions options = {})
{
    auto h = NativeHarness { std::move(options) };
    h.host.createTab();
    auto reply = std::vector<std::byte> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &hello),
                                             readUntilClosed(&h.loop, h.pair.second.get(), &reply)));
    return reply;
}

/// Feeds @p hello, a pre-negotiation client's ClientHello frame, to a fresh session and reads the one
/// frame it answers with.
/// @param hello The frame's bytes, as that client writes them.
/// @return The answer's body, after checking it is a lone ServerHello the session sends before closing.
std::vector<std::byte> refusalOf(std::vector<std::byte> const& hello)
{
    auto const reply = answerTo(hello);
    auto const frame = proto::readFrame(reply);
    REQUIRE(frame.has_value());
    CHECK(frame->ident == std::to_underlying(proto::PduType::ServerHello));
    CHECK(frame->consumed == reply.size()); // nothing follows it: no snapshot for a refused client
    auto const decoded = proto::decodePdu(reply);
    REQUIRE(decoded.has_value());
    auto const* answer = std::get_if<proto::ServerHello>(&decoded->pdu);
    REQUIRE(answer != nullptr);
    CHECK(answer->layout == proto::HelloLayout::VersionAlone);
    CHECK(answer->machineId.empty()); // a refusal names no machine
    return { frame->body.begin(), frame->body.end() };
}
} // namespace

TEST_CASE("a codec-version mismatch is reported with both versions", "[vthost][native][diagnostics]")
{
    // A codec 3 (status-line extensions) client, the release before negotiation: it reads the refusal and
    // reports it, and the daemon's log names both versions.
    auto capture = core::log::ScopedCapture {};
    auto const body = refusalOf(vthost::testing::codec3ClientHello(1));
    CHECK(vthost::testing::codec3ServerHelloVersion(body) == proto::NegotiatingHelloMarker);

    CHECK(capture.contains("test#1"));
    CHECK(capture.contains("handshake rejected"));
    CHECK(capture.contains(std::format("speaks protocol {}", vthost::testing::StatusLineExtensionCodec)));
    CHECK(capture.contains(
        std::format("this daemon speaks {}-{}", proto::SupportedVersions.min, proto::SupportedVersions.max)));
}

TEST_CASE("a peer of the command-block revision is refused", "[vthost][native][handshake]")
{
    // Revision 2 (#1010) lays a Delta out without the status rows' display byte, and knows neither
    // permission PDU: past a handshake it would mis-read every Delta, so the handshake must refuse it,
    // in the one ServerHello layout that client reads.
    auto capture = core::log::ScopedCapture {};
    auto const body = refusalOf(vthost::testing::codec2ClientHello(1));
    CHECK(vthost::testing::codec2ServerHelloVersion(body) == proto::NegotiatingHelloMarker);
    CHECK(capture.contains("handshake rejected"));
}

TEST_CASE("a missing ClientHello names what arrived instead", "[vthost][native][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};
    auto harness = NativeHarness {};
    std::ignore = harness.exchange({ proto::DecodedPdu { proto::CreateTab {} } }, 1);

    CHECK(capture.contains("handshake rejected"));
    CHECK(capture.contains("expected ClientHello, got CreateTab"));
}

TEST_CASE("a token mismatch is reported without either token", "[vthost][native][diagnostics]")
{
    // The wire answer deliberately makes this indistinguishable from a version mismatch, so the
    // daemon-side log is the ONLY place the real reason exists. It must still not write the
    // secret down.
    auto capture = core::log::ScopedCapture {};

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto session = std::make_unique<NativeSession>(
        loop,
        host,
        vthost::ConnectionId { .endpoint = "native-tcp", .index = 3, .peer = "10.0.0.7" },
        std::move(pair.first),
        NativeSession::DefaultWriteQueueBytes,
        vthost::NativeSessionServices {
            .handshake = { .acceptedTokens = { { .token = "the-right-token" } } } });

    auto const bytes =
        encodeRequest({ proto::DecodedPdu { proto::ClientHello { .token = "the-wrong-token" } } });
    auto received = std::vector<proto::DecodedFrame> {};
    loop.blockOn(driveExchange(&loop, session.get(), pair.second.get(), &bytes, 1, &received));

    CHECK(capture.contains("native-tcp#3@10.0.0.7"));
    CHECK(capture.contains("authentication failed"));
    CHECK_FALSE(capture.contains("the-right-token"));
    CHECK_FALSE(capture.contains("the-wrong-token"));
}

TEST_CASE("a client area that did not change is not answered with a resync", "[vthost][native]")
{
    // THE regression these two cases exist to hold: a resync is a full-grid snapshot including
    // all scrollback, and clients re-assert their client area on every attach. Answering an
    // unchanged area with one snapshot per pane is what overflowed the send queue and got
    // healthy clients disconnected — every time, since the daemon's history does not shrink.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const spawned = h.host.pageSize();

    auto const received = exchangeAll(
        &h,
        { proto::DecodedPdu { proto::ClientHello {} },
          proto::DecodedPdu { proto::ResizeRequest { .columns = unbox<uint32_t>(spawned.columns),
                                                     .lines = unbox<uint32_t>(spawned.lines) } } });

    // Exactly the attach answer: ServerHello, LayoutState, SessionState, Delta.
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->columns == unbox<uint32_t>(spawned.columns));
    CHECK(state->lines == unbox<uint32_t>(spawned.lines));
}

TEST_CASE("a client area that did change is answered with a resync", "[vthost][native]")
{
    // The other half, so the case above cannot pass vacuously: if reprojection stopped
    // answering real resizes, the dimensions below stay at the spawned size.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const spawned = h.host.pageSize();
    auto const grown = vtpty::PageSize { .lines = spawned.lines + vtpty::LineCount(3),
                                         .columns = spawned.columns + vtpty::ColumnCount(7) };

    auto const received =
        exchangeAll(&h,
                    { proto::DecodedPdu { proto::ClientHello {} },
                      proto::DecodedPdu { proto::ResizeRequest { .columns = unbox<uint32_t>(grown.columns),
                                                                 .lines = unbox<uint32_t>(grown.lines) } } });

    // Still four PDUs, and that is the coalescing working rather than the resync missing: the
    // attach snapshot was still unwritten when the resize superseded it, so the client is told
    // the new geometry ONCE instead of being handed two whole grids back to back.
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->columns == unbox<uint32_t>(grown.columns));
    CHECK(state->lines == unbox<uint32_t>(grown.lines));
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
}

TEST_CASE("an out-of-range resize proposal is reported", "[vthost][native][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};
    auto harness = NativeHarness {};
    std::ignore =
        harness.exchange({ proto::DecodedPdu { proto::ClientHello {} },
                           proto::DecodedPdu { proto::ResizeRequest { .columns = 1'000'000, .lines = 24 } } },
                         1);

    CHECK(capture.contains("rejecting client area 1000000x24 (out of range)"));
}

TEST_CASE("an attach that states its area spawns the first session at that size", "[vthost][native]")
{
    // No ResizeRequest follows: the hello alone sizes the session. (One sent at once would hide a
    // missing hello area, its resync coalescing into the still-unwritten attach snapshot.)
    auto h = NativeHarness {};
    auto const received =
        exchangeAll(&h,
                    { proto::DecodedPdu { proto::ClientHello {
                        .clientArea = proto::WireClientArea { .columns = 132, .lines = 43 } } } });

    REQUIRE(h.host.sessionCount() == 1);
    auto const layout = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutState>(frame.pdu); });
    REQUIRE(layout != received.end());
    auto const& tabs = std::get<proto::LayoutState>(layout->pdu).tabs;
    REQUIRE(tabs.size() == 1);
    auto const* terminal = h.host.terminal(vtworkspace::SessionId { tabs.front().root.session });
    REQUIRE(terminal != nullptr);
    CHECK(terminal->pageSize()
          == vtbackend::PageSize { vtbackend::LineCount(43), vtbackend::ColumnCount(132) });

    // One SessionState and one snapshot: the attach's, at the stated size.
    auto const isState = [](auto const& frame) {
        return std::holds_alternative<proto::SessionState>(frame.pdu);
    };
    auto const isSnapshot = [](auto const& frame) {
        auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
        return delta != nullptr && delta->snapshot != 0;
    };
    CHECK(std::ranges::count_if(received, isState) == 1);
    CHECK(std::ranges::count_if(received, isSnapshot) == 1);
    auto const state = std::ranges::find_if(received, isState);
    REQUIRE(state != received.end());
    CHECK(std::get<proto::SessionState>(state->pdu).columns == 132);
    CHECK(std::get<proto::SessionState>(state->pdu).lines == 43);
}

TEST_CASE("an out-of-range hello area is refused and the session spawns at the host size", "[vthost][native]")
{
    struct Row
    {
        proto::WireClientArea area;
        std::string_view logged;
    };
    auto const rows = std::array {
        Row { .area = { .columns = 0, .lines = 0 },
              .logged = "rejecting hello client area 0x0 (out of range)" },
        Row { .area = { .columns = 100'000, .lines = 100'000 },
              .logged = "rejecting hello client area 100000x100000 (out of range)" },
    };
    for (auto const& row: rows)
    {
        auto capture = core::log::ScopedCapture {};
        auto h = NativeHarness {};
        auto const hostSize = h.host.pageSize();
        auto const received =
            exchangeAll(&h, { proto::DecodedPdu { proto::ClientHello { .clientArea = row.area } } });
        REQUIRE_FALSE(received.empty());
        REQUIRE(std::holds_alternative<proto::ServerHello>(received.front().pdu));
        CHECK(std::get<proto::ServerHello>(received.front().pdu).agreedVersion != 0);
        auto const state = std::ranges::find_if(received, [](auto const& frame) {
            return std::holds_alternative<proto::SessionState>(frame.pdu);
        });
        REQUIRE(state != received.end());
        CHECK(std::get<proto::SessionState>(state->pdu).columns == unbox<uint32_t>(hostSize.columns));
        CHECK(std::get<proto::SessionState>(state->pdu).lines == unbox<uint32_t>(hostSize.lines));
        CHECK(h.host.pageSize() == hostSize);
        CHECK(h.host.sessionCount() == 1);
        CHECK(capture.contains(row.logged));
    }
}

TEST_CASE("a Control hello's client area is ignored", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const hostSize = h.host.pageSize();
    std::ignore = h.exchange(
        { proto::ClientHello { .role = proto::ConnectionRole::Control,
                               .clientArea = proto::WireClientArea { .columns = 132, .lines = 43 } } },
        1);
    CHECK(h.host.pageSize() == hostSize);
    CHECK(h.host.sessionCount() == 0);
}

namespace
{

/// @return A harness whose sessions run on CountingPty, so a case can tell how often each PTY was resized.
HarnessOptions countingPtys()
{
    return HarnessOptions {
        .ptyFactory = [](vtbackend::PageSize size,
                         std::optional<vtpty::Process::ExecInfo> const&) -> std::unique_ptr<vtpty::Pty> {
            return std::make_unique<vthost::testing::CountingPty>(size);
        }
    };
}

/// @return The CountingPty under @p id's terminal.
vthost::testing::CountingPty& countingPtyOf(NativeHarness& h, vtworkspace::SessionId id)
{
    return dynamic_cast<vthost::testing::CountingPty&>(
        dynamic_cast<vthost::TappingPty&>(h.host.terminal(id)->device()).inner());
}

} // namespace

TEST_CASE("NativeSession.applies_the_hello_cell_size_before_the_first_spawn", "[vthost][native][cell]")
{
    auto h = NativeHarness { countingPtys() };
    std::ignore =
        exchangeAll(&h,
                    { proto::DecodedPdu { proto::ClientHello {
                        .clientArea = proto::WireClientArea { .columns = 132, .lines = 43 },
                        .cellPixelSize = proto::WireCellPixelSize { .width = 9, .height = 18 } } } });

    REQUIRE(h.host.sessionCount() == 1);
    auto const id = h.host.sessionIds().front();
    auto const& terminal = *h.host.terminal(id);
    CHECK(terminal.cellPixelSize() == cellOf(9, 18));
    auto const& pty = countingPtyOf(h, id);
    CHECK(pty.pixelSize() == cellOf(9, 18) * terminal.pageSize());
    // Spawned at the client's cell: not at the default first and corrected after.
    CHECK(pty.resizes() == 1);
}

TEST_CASE("NativeSession.applies_the_hello_cell_size_beside_a_refused_area", "[vthost][native][cell]")
{
    auto h = NativeHarness { countingPtys() };
    std::ignore =
        exchangeAll(&h,
                    { proto::DecodedPdu { proto::ClientHello {
                        .clientArea = proto::WireClientArea { .columns = 0, .lines = 43 },
                        .cellPixelSize = proto::WireCellPixelSize { .width = 9, .height = 18 } } } });

    // The area is refused, so the session spawns at the host's size; the cell describes the client and
    // counts.
    REQUIRE(h.host.sessionCount() == 1);
    CHECK(h.host.learnedCellPixelSize() == cellOf(9, 18));
    auto const id = h.host.sessionIds().front();
    auto const& terminal = *h.host.terminal(id);
    CHECK(terminal.cellPixelSize() == cellOf(9, 18));
    CHECK(countingPtyOf(h, id).pixelSize() == cellOf(9, 18) * terminal.pageSize());
}

TEST_CASE("NativeSession.applies_a_resize_cell_size", "[vthost][native][cell]")
{
    // The session the attach spawns is the host's first.
    auto constexpr First = vtworkspace::SessionId { 1 };
    auto h = NativeHarness { countingPtys() };
    SECTION("a ResizeRequest's")
    {
        std::ignore =
            exchangeAll(&h,
                        { proto::DecodedPdu { proto::ClientHello {} },
                          proto::DecodedPdu { proto::ResizeRequest {
                              .columns = 100,
                              .lines = 30,
                              .cellPixelSize = proto::WireCellPixelSize { .width = 9, .height = 18 } } } });
        REQUIRE(h.host.sessionIds() == std::vector { First });
        auto const& terminal = *h.host.terminal(First);
        CHECK(terminal.cellPixelSize() == cellOf(9, 18));
        CHECK(terminal.totalPageSize().columns == vtpty::ColumnCount(100));
        CHECK(countingPtyOf(h, First).pixelSize() == cellOf(9, 18) * terminal.pageSize());
    }
    SECTION("a ResizePane's")
    {
        std::ignore =
            exchangeAll(&h,
                        { proto::DecodedPdu { proto::ClientHello {} },
                          proto::DecodedPdu { proto::ResizeRequest { .columns = 100, .lines = 30 } },
                          proto::DecodedPdu { proto::ResizePane {
                              .session = First.value,
                              .columns = 100,
                              .lines = 30,
                              .cellPixelSize = proto::WireCellPixelSize { .width = 18, .height = 36 } } } });
        REQUIRE(h.host.sessionIds() == std::vector { First });
        auto const& terminal = *h.host.terminal(First);
        CHECK(terminal.cellPixelSize() == cellOf(18, 36));
        CHECK(countingPtyOf(h, First).pixelSize() == cellOf(18, 36) * terminal.pageSize());
    }
}

TEST_CASE("NativeSession.rejects_an_out_of_range_cell_size", "[vthost][native][cell]")
{
    struct Row
    {
        proto::WireCellPixelSize cell;
        std::string_view logged;
    };
    auto const rows = std::array {
        Row { .cell = { .width = 0, .height = 20 },
              .logged = "rejecting client cell size 0x20 (out of range)" },
        Row { .cell = { .width = 65535, .height = 65535 },
              .logged = "rejecting client cell size 65535x65535 (out of range)" },
    };
    for (auto const& row: rows)
    {
        auto capture = core::log::ScopedCapture {};
        auto h = NativeHarness {};
        std::ignore = exchangeAll(&h,
                                  { proto::DecodedPdu { proto::ClientHello {} },
                                    proto::DecodedPdu { proto::ResizeRequest {
                                        .columns = 100, .lines = 30, .cellPixelSize = row.cell } } });

        REQUIRE(h.host.sessionCount() == 1);
        auto const* terminal = h.host.terminal(h.host.sessionIds().front());
        CHECK(terminal->cellPixelSize() == cellOf(10, 20));                  // the default, untouched
        CHECK(terminal->totalPageSize().columns == vtpty::ColumnCount(100)); // the grid still applies
        CHECK(h.host.learnedCellPixelSize() == std::nullopt);
        CHECK(capture.contains(row.logged));
    }
}

TEST_CASE("NativeSession.takes_a_stated_0x0_cell_size_as_none_without_a_log", "[vthost][native][cell]")
{
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness {};
    std::ignore =
        exchangeAll(&h,
                    { proto::DecodedPdu { proto::ClientHello {} },
                      proto::DecodedPdu { proto::ResizeRequest {
                          .columns = 100,
                          .lines = 30,
                          .cellPixelSize = proto::WireCellPixelSize { .width = 0, .height = 0 } } } });

    REQUIRE(h.host.sessionCount() == 1);
    CHECK(h.host.terminal(h.host.sessionIds().front())->cellPixelSize() == cellOf(10, 20));
    CHECK(h.host.learnedCellPixelSize() == std::nullopt);
    CHECK_FALSE(capture.contains("rejecting"));
}

TEST_CASE("an unexpected PDU is reported rather than silently dropped", "[vthost][native][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};
    auto harness = NativeHarness {};
    // A server-to-client PDU sent the wrong way: valid on the wire, meaningless here.
    std::ignore = harness.exchange(
        { proto::DecodedPdu { proto::ClientHello {} }, proto::DecodedPdu { proto::ServerHello {} } }, 1);

    CHECK(capture.contains("ignoring unexpected ServerHello"));
}

TEST_CASE("the PDU trace records both directions when enabled", "[vthost][native][diagnostics]")
{
    auto capture = core::log::ScopedCapture { "vthost.trace.proto" };
    auto harness = NativeHarness {};
    std::ignore = harness.exchange({ proto::DecodedPdu { proto::ClientHello {} } }, 1);

    CHECK(capture.contains("test#1 recv #1 ClientHello"));
    CHECK(capture.contains("test#1 send #1 ServerHello"));
    // Sizes, never contents.
    CHECK(capture.contains("bytes)"));
}

TEST_CASE("the PDU trace is disabled by default", "[vthost][native][diagnostics]")
{
    // It is one line per frame on a busy session; enabling it by accident would be a
    // performance and a privacy problem at once.
    auto const* const category = core::log::get("vthost.trace.proto");
    REQUIRE(category != nullptr);
    CHECK_FALSE(category->isEnabled());
}

TEST_CASE("a ClientHello's settings reach the sessions that client creates", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    // An EMPTY daemon, so the session below exists precisely because this client attached — it must
    // inherit the client's profile just like a tab the client goes on to open.
    auto hello = proto::ClientHello {};
    hello.sessionSettings = proto::WireSessionSettings {
        .historyLineCount = 7000,
        .terminalId = 41, // VT420
        .graphemeClustering = 0,
        .allowReflowOnResize = 0,
        .maxImageRegisterCount = 512,
        .sixelScrolling = 0,
    };

    // ServerHello, LayoutState, SessionState, Delta.
    std::ignore = h.exchange({ proto::DecodedPdu { hello } }, 4);

    auto const sessionId = h.host.model().workspace(h.host.workspaceId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(sessionId);
    REQUIRE(terminal != nullptr);
    CHECK(terminal->terminalId() == vtbackend::VTType::VT420);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount()) == 7000);
    // A configured setting: the session's DECSDM from the start, and again after a RIS.
    CHECK(terminal->isModeEnabled(vtbackend::DECMode::NoSixelScrolling));
    terminal->writeToScreen("\033[?80l\033c");
    CHECK(terminal->isModeEnabled(vtbackend::DECMode::NoSixelScrolling));

    // The host's own settings are NOT rewritten: they remain the default for anything the daemon or
    // another client creates.
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(h.host.settings().historyLimits.capacity));
    CHECK(unbox<int>(std::get<vtbackend::LineCount>(h.host.settings().historyLimits.capacity)) == 500);
}

TEST_CASE("restated settings reach the sessions the client creates afterwards", "[vthost][native]")
{
    // A client whose configuration was reloaded restates its preference; the next session it creates
    // takes the new one, while the sessions that exist keep theirs.
    auto h = NativeHarness {};

    auto hello = proto::ClientHello {};
    hello.sessionSettings = proto::WireSessionSettings { .terminalId = 41 }; // VT420
    // VT220, in vtbackend::VTType's numeric encoding.
    auto const restated =
        proto::SessionSettingsChanged { .sessionSettings = proto::WireSessionSettings { .terminalId = 1 } };

    std::ignore = h.exchange({ proto::DecodedPdu { hello },
                               proto::DecodedPdu { restated },
                               proto::DecodedPdu { proto::CreateTab {} } },
                             4);

    auto const* workspace = h.host.model().workspace(h.host.workspaceId());
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->tabCount() == 2);
    auto* first = h.host.terminal(workspace->tabAt(0)->rootPane()->session());
    auto* second = h.host.terminal(workspace->tabAt(1)->rootPane()->session());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->terminalId() == vtbackend::VTType::VT420);
    CHECK(second->terminalId() == vtbackend::VTType::VT220);
}

TEST_CASE("restating no settings returns later sessions to the host's own", "[vthost][native]")
{
    auto h = NativeHarness {};

    auto hello = proto::ClientHello {};
    hello.sessionSettings = proto::WireSessionSettings { .terminalId = 41 }; // VT420
    REQUIRE(h.host.settings().terminalId != vtbackend::VTType::VT420);

    std::ignore = h.exchange({ proto::DecodedPdu { hello },
                               proto::DecodedPdu { proto::SessionSettingsChanged {} },
                               proto::DecodedPdu { proto::CreateTab {} } },
                             4);

    auto const* workspace = h.host.model().workspace(h.host.workspaceId());
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->tabCount() == 2);
    auto* second = h.host.terminal(workspace->tabAt(1)->rootPane()->session());
    REQUIRE(second != nullptr);
    CHECK(second->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a ClientHello without settings leaves the host's own in force", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    std::ignore = h.exchange({ proto::ClientHello {} }, 4);

    auto const sessionId = h.host.model().workspace(h.host.workspaceId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(sessionId);
    REQUIRE(terminal != nullptr);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount()) == 500);
    CHECK(terminal->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a client cannot ask a session out of the host invariants", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    // The wire is remote input, so the request is re-normalized rather than trusted. Zero scrollback
    // is the configuration that makes every scrolling batch a full snapshot; an unknown terminalId
    // number cannot be clamped into anything sensible, so the daemon's own stands.
    auto hello = proto::ClientHello {};
    hello.sessionSettings = proto::WireSessionSettings { .historyLineCount = 0, .terminalId = 42 };

    std::ignore = h.exchange({ proto::DecodedPdu { hello } }, 4);

    auto const sessionId = h.host.model().workspace(h.host.workspaceId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(sessionId);
    REQUIRE(terminal != nullptr);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount())
          == vthost::DefaultSessionHistoryLineCount);
    CHECK(terminal->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a resize one client asked for resyncs EVERY attached client", "[vthost][native]")
{
    // Maximizing one window left the other rendering the old grid. The resync was issued by the
    // connection that received the ResizeRequest, over its OWN follow map, so no other client heard
    // about it -- and `Terminal::resizeScreen` raises no screen update, so nothing else would tell
    // them either until the session next produced output. A shared grid has to announce itself.
    auto h = TwoClientHarness {};
    h.host.createTab();

    auto const hello = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    // 40x10 is nothing like the host's default, so the resize cannot be a no-op once clamped.
    auto const helloThenResize = encodeRequest({
        proto::DecodedPdu { proto::ClientHello {} },
        proto::DecodedPdu { proto::ResizeRequest { .columns = 40, .lines = 10 } },
    });

    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};

    // Client two attaches first and then sits idle; client one attaches and resizes once client two
    // holds its attach snapshot, so client two is attached when the grid moves -- which is the whole
    // scenario. Each client stops once it holds what the resize should have sent it.
    auto const attached = [](std::vector<proto::DecodedFrame> const& got) {
        return sessionStateCount(got) >= 1;
    };
    auto const resized = [](std::vector<proto::DecodedFrame> const& got) {
        auto const* const state = lastSessionState(got);
        return state != nullptr && state->columns == 40;
    };
    auto const resynced = [](std::vector<proto::DecodedFrame> const& got) {
        return sessionStateCount(got) >= 2;
    };
    h.loop.blockOn(core::net::testing::allOf(
        h.serverOne->run(),
        h.serverTwo->run(),
        feedBytes(h.secondPair.second.get(), &hello),
        feedOnce(&h.loop, h.firstPair.second.get(), &helloThenResize, &fromTwo, attached),
        collectUntil(&h.loop, h.firstPair.second.get(), resized, &fromOne),
        collectUntil(&h.loop, h.secondPair.second.get(), resynced, &fromTwo)));

    // The client that asked: its attach snapshot, then the resize snapshot.
    auto const* const oneState = lastSessionState(fromOne);
    REQUIRE(oneState != nullptr);
    CHECK(oneState->columns == 40);
    CHECK(oneState->lines == 10);

    // The client that did NOT ask, which is the regression: it must have been resynced too, and its
    // LAST snapshot must describe the new grid rather than the one it attached at.
    auto const* const twoState = lastSessionState(fromTwo);
    REQUIRE(twoState != nullptr);
    CHECK(twoState->columns == 40);
    CHECK(twoState->lines == 10);
    // Two snapshots, not one: the attach plus the resize. Equal sizes alone would also pass if the
    // second client had simply attached late, after the resize.
    CHECK(sessionStateCount(fromTwo) == 2);
}

TEST_CASE("the attach snapshot states every command block outright", "[vthost][native][blocks]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(commandCycle("make%20test", 2));

    // Expect: ServerHello, LayoutState, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    REQUIRE(state->commandBlocks.size() == 1);
    auto const& block = state->commandBlocks.front();
    CHECK(block.id == recordIds(h.host.terminal(sessionId)->commandBlocks()).front());
    CHECK(block.state == std::to_underlying(vtbackend::CommandBlockState::Finished));
    CHECK(block.commandLine == "make test");
    CHECK((block.present & proto::CommandBlockHasExitCode) != 0);
    CHECK(block.exitCode == 2);

    // Stated once, in SessionState — the snapshot's rows do not repeat it.
    auto const* snapshot = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->commandBlocks.empty());
}

TEST_CASE("the host states the epoch its command-block ids count in", "[vthost][native][blocks]")
{
    // A row holds its block's id, and an id names a block only within its epoch: a mirror holding rows the
    // host no longer sends learns from the epoch that their ids name nothing now.
    constexpr auto LastId = std::numeric_limits<uint32_t>::max() - 1; // the largest id a store mints
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    auto& terminal = *h.host.terminal(sessionId);
    terminal.adoptCommandBlock(vthost::fromWireCommandBlock(vthost::testing::runningWire(LastId), nullptr),
                               vtbackend::AdoptMode::Snapshot);
    auto const startOver = "\r\n" + std::string { PromptStart }; // the next prompt's id is 1 again

    SECTION("in a snapshot")
    {
        terminal.writeToScreen(startOver);
        REQUIRE(terminal.commandBlocks().idEpoch() == 1);

        auto const received = h.attach(AttachSnapshotFrames);
        REQUIRE(received.size() == AttachSnapshotFrames);
        auto const* const state = std::get_if<proto::SessionState>(&received[2].pdu);
        REQUIRE(state != nullptr);
        CHECK(state->commandBlockIdEpoch == 1);
    }
    SECTION("in an increment")
    {
        auto const received =
            h.attach(AttachSnapshotFrames + 1,
                     writeOnce(&h, sessionId, startOver, holdsFrames(&h.received, AttachSnapshotFrames)));
        REQUIRE(received.size() == AttachSnapshotFrames + 1);
        REQUIRE(terminal.commandBlocks().idEpoch() == 1);
        auto const* const state = std::get_if<proto::SessionState>(&received[2].pdu);
        REQUIRE(state != nullptr);
        CHECK(state->commandBlockIdEpoch == 0);
        auto const* const increment = std::get_if<proto::Delta>(&received[4].pdu);
        REQUIRE(increment != nullptr);
        CHECK(increment->commandBlockIdEpoch == 1);
    }
}

TEST_CASE("an increment carries only the command blocks that changed", "[vthost][native][blocks]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(commandCycle("ls", 0));

    auto const received =
        h.attach(6,
                 writeOnce(&h,
                           sessionId,
                           "\r\n" + std::string { PromptStart },
                           holdsFrames(&h.received, AttachSnapshotFrames)),
                 writeOnce(&h, sessionId, "typed", holdsFrames(&h.received, AttachSnapshotFrames + 1)));
    REQUIRE(received.size() == 6);

    // [4] the new prompt: exactly one record, and it is not the finished one again.
    auto const ids = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(ids.size() == 2);
    auto const* prompt = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(prompt != nullptr);
    REQUIRE(prompt->snapshot == 0);
    REQUIRE(prompt->commandBlocks.size() == 1);
    CHECK(prompt->commandBlocks.front().id == ids.back());
    CHECK(prompt->commandBlocks.front().state == std::to_underlying(vtbackend::CommandBlockState::Prompting));

    // [5] typing at the prompt moved a row and no record: the revision gate kept it record-free.
    auto const* typing = std::get_if<proto::Delta>(&received[5].pdu);
    REQUIRE(typing != nullptr);
    CHECK_FALSE(typing->lines.empty());
    CHECK(typing->commandBlocks.empty());
    CHECK(typing->retiredCommandBlocks.empty());
}

TEST_CASE("an attach's steps stay apart however late the loop gets to them", "[vthost][native]")
{
    // A loaded machine runs the loop late: every timer and completion that came due meanwhile is
    // found in one turn. Steps spaced by delays then land together -- the prompt and the typing in
    // one increment, or either one inside the attach snapshot -- and the client counts frames
    // that never come. Each step must wait for the frame it follows instead.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(commandCycle("ls", 0));

    auto const received =
        h.attach(6,
                 stallLoop(&h, 100ms),
                 writeOnce(&h,
                           sessionId,
                           "\r\n" + std::string { PromptStart },
                           holdsFrames(&h.received, AttachSnapshotFrames)),
                 writeOnce(&h, sessionId, "typed", holdsFrames(&h.received, AttachSnapshotFrames + 1)));
    REQUIRE(received.size() == 6);
    CHECK(isIncrement(received[4]));
    CHECK(isIncrement(received[5]));
}

TEST_CASE("a prompt the host discarded is retired on the client", "[vthost][native][blocks]")
{
    // An empty Enter: the next OSC 133;A discards the Prompting record that never reached C. The
    // client was told about it, so it must be told it is gone — or an attached pane keeps a phantom
    // block for every empty prompt.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(PromptStart);
    auto const discarded = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(discarded.size() == 1);

    auto const received = h.attach(5,
                                   writeOnce(&h,
                                             sessionId,
                                             "\r\n" + std::string { PromptStart },
                                             holdsFrames(&h.received, AttachSnapshotFrames)));
    REQUIRE(received.size() == 5);

    auto const remaining = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(remaining.size() == 1);
    REQUIRE(remaining.front() != discarded.front());
    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->retiredCommandBlocks == std::vector<uint32_t> { discarded.front() });
    REQUIRE(delta->commandBlocks.size() == 1);
    CHECK(delta->commandBlocks.front().id == remaining.front());
}

TEST_CASE("a burst of records larger than one delta's budget drains oldest first", "[vthost][native][blocks]")
{
    // Forty 4 KB command lines are ~160 KB of records: built into one delta they would be a frame the
    // send queue may refuse on a non-empty backlog. Budgeted, they arrive over several deltas, each
    // within CommandBlockDeltaBytes, oldest first, each exactly once.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    auto const received = attachUntilBlocks(
        h,
        40,
        writeOnce(&h, sessionId, commandBurst(std::string(4000, 'a'), 40), followsSession(&h, sessionId)));

    auto delivered = std::vector<uint32_t> {};
    auto carrying = 0;
    for (auto const* delta: increments(received))
    {
        CHECK((delta->commandBlocks.size() <= 1
               || recordBytes(delta->commandBlocks) <= NativeSession::CommandBlockDeltaBytes));
        for (auto const& block: delta->commandBlocks)
            delivered.push_back(block.id);
        carrying += delta->commandBlocks.empty() ? 0 : 1;
    }
    CHECK(delivered
          == recordIds(h.host.terminal(sessionId)->commandBlocks())); // every record, once, oldest first
    CHECK(carrying > 1);                                              // and it really took several deltas
}

TEST_CASE("a snapshot states the newest records within its cap; the rest follow", "[vthost][native][blocks]")
{
    // A hundred 4 KB records (~410 KB) exceed SnapshotChunkBytes. SessionState is one unchunked frame,
    // so it states the newest that fit, and the older remainder arrives as increments right after.
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(commandBurst(std::string(4000, 'b'), 100));
    auto const ids = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(ids.size() == 100);

    auto const received = attachUntilBlocks(h, 100);

    auto const* state = lastSessionState(received);
    REQUIRE(state != nullptr);
    REQUIRE_FALSE(state->commandBlocks.empty());
    CHECK(recordBytes(state->commandBlocks) <= NativeSession::SnapshotChunkBytes);
    CHECK(state->commandBlocks.back().id == ids.back());   // the newest made it ...
    CHECK(state->commandBlocks.front().id != ids.front()); // ... the oldest did not ...
    CHECK(deliveredBlockIds(received) == std::set<uint32_t>(ids.begin(), ids.end())); // ... but followed
}

TEST_CASE("under a small send-queue bound command blocks travel in quarter-bound frames",
          "[vthost][native][blocks]")
{
    // SessionState is one unchunked frame. Capped at SnapshotChunkBytes — four times this bound — it
    // would state all twenty records, ~80 KB: a frame over the bound on its own, which refuses
    // whatever is enqueued behind it while it waits in the backlog — the overflow quarter-bound
    // snapshot pieces exist to prevent. Capped at the piece, it is no larger than the pieces that
    // follow it, and neither is any increment carrying the rest.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(commandBurst(std::string(4000, 'c'), 20));
    auto const ids = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(ids.size() == 20);

    auto const received = attachUntilBlocks(h, 20);

    auto const piece = NativeSession::snapshotPieceBytes(SmallQueueBytes);
    auto const* state = lastSessionState(received);
    REQUIRE(state != nullptr);
    REQUIRE_FALSE(state->commandBlocks.empty());
    CHECK(recordBytes(state->commandBlocks) <= piece);
    CHECK(state->commandBlocks.back().id == ids.back());
    auto carrying = 0;
    for (auto const* delta: increments(received))
    {
        CHECK((delta->commandBlocks.size() <= 1 || recordBytes(delta->commandBlocks) <= piece));
        carrying += delta->commandBlocks.empty() ? 0 : 1;
    }
    CHECK(carrying > 1); // the remainder needed more than one piece-sized increment
    CHECK(deliveredBlockIds(received) == std::set<uint32_t>(ids.begin(), ids.end()));
    CHECK_FALSE(capture.contains("send queue overflow"));
}

TEST_CASE("command blocks wait for a client that stopped reading instead of costing the connection",
          "[vthost][native][blocks]")
{
    // Rows must flow; records need not, being state the next delta carries just as well. Forty
    // maximum-size records are ~320 KB, five times this bound. Were they to ride an increment every
    // debounce window regardless, a client that reads nothing for a while would have them fill the
    // pipe and then the backlog, until the queue refused one and the connection went. Gated on room,
    // they wait, and every one arrives once the client reads again.
    auto capture = core::log::ScopedCapture {};
    auto h = NativeHarness { smallQueueOptions() };
    auto const sessionId = newSession(h.host);
    auto const longest = std::string(vtbackend::MaxRecordedCommandLineBytes, 'd');
    auto const deepest = std::string(vtbackend::MaxRecordedWorkingDirectoryBytes - 1, 'p');
    auto const burst = std::format("\033]7;file://localhost/{}\033\\", deepest) + commandBurst(longest, 40);

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             stallThenCollectBlocks(&h, 800ms, 40, &received),
                                             writeOnce(&h, sessionId, burst, followsSession(&h, sessionId))));

    // Maximum-size as the host recorded them: neither string was cut, nor the directory dropped.
    auto const& newest = h.host.terminal(sessionId)->commandBlocks().records().back();
    REQUIRE(newest.commandLine.raw().size() == vtbackend::MaxRecordedCommandLineBytes);
    REQUIRE(newest.workingDirectory.path.size() == vtbackend::MaxRecordedWorkingDirectoryBytes);

    CHECK_FALSE(capture.contains("send queue overflow"));
    auto const ids = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(ids.size() == 40);
    CHECK(deliveredBlockIds(received) == std::set<uint32_t>(ids.begin(), ids.end()));
}

TEST_CASE("a head re-placement sends no command block", "[vthost][native][blocks]")
{
    // A head names a row in the HOST's grid and never travels, so an erase, a reflow or a scroll inside
    // margins that re-heads every block changes nothing a client holds: the increment carrying the row
    // written beside it carries no record, and no walk over the records even began.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness {};
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    auto* const terminal = h.host.terminal(sessionId);
    terminal->writeToScreen(commandCycle("ls", 0) + "\r\n" + commandCycle("make", 2) + "\r\n");
    std::ignore = Tester::snapshot(*h.session, sessionId);

    // Every block re-headed, the way Terminal::syncCommandBlockHeads() records it, and a row written.
    auto& store = terminal->commandBlocks();
    auto const revision = store.revision();
    for (auto const id: recordIds(h.host.terminal(sessionId)->commandBlocks()))
        store.updateHeadPosition(vtbackend::CommandBlockId { id }, -1, 7);
    REQUIRE(store.revision() != revision);
    terminal->writeToScreen("typed");

    auto const delta = Tester::increment(*h.session, sessionId);
    REQUIRE(delta.has_value());
    CHECK_FALSE(delta->lines.empty());
    CHECK(delta->commandBlocks.empty());
    CHECK(delta->retiredCommandBlocks.empty());
    CHECK_FALSE(Tester::walkFrom(*h.session, sessionId).has_value());
}

TEST_CASE("a budgeted record walk resumes where the last increment stopped", "[vthost][native][blocks]")
{
    // Forty 4 KB records are several increments' worth. Each increment converts records from where the
    // last one stopped, never from the oldest again, so the remainder drains in work proportional to
    // itself: every record converted once, plus the one per increment that did not fit — not the
    // quadratic walk a restart from the oldest each workspace would cost.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness {};
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    std::ignore = Tester::snapshot(*h.session, sessionId); // the attach, before any record
    h.host.terminal(sessionId)->writeToScreen(commandBurst(std::string(4000, 'a'), 40));

    auto delivered = std::vector<uint32_t> {};
    auto increments = 0;
    for ([[maybe_unused]] auto const workspace: std::views::iota(0, 40))
    {
        auto const from = Tester::walkFrom(*h.session, sessionId);
        auto const delta = Tester::increment(*h.session, sessionId);
        if (!delta)
            break;
        ++increments;
        REQUIRE_FALSE(delta->commandBlocks.empty());
        CHECK(recordBytes(delta->commandBlocks) <= NativeSession::CommandBlockDeltaBytes);
        if (from)
            CHECK(delta->commandBlocks.front().id == from->value); // exactly where the last one stopped
        for (auto const& block: delta->commandBlocks)
            delivered.push_back(block.id);
    }
    CHECK(delivered
          == recordIds(h.host.terminal(sessionId)->commandBlocks())); // every record once, oldest first
    CHECK(increments > 1);                                            // and it really took several increments
    CHECK_FALSE(Tester::walkFrom(*h.session, sessionId).has_value());
}

TEST_CASE("a capped resync snapshot retires nothing the client holds below its floor",
          "[vthost][native][blocks]")
{
    // A resize resyncs every pane, and a snapshot capped below the host's store used to replace the
    // client's whole table: every record older than the cap retired, re-sent and re-adopted, and the
    // gutter and the marks flickered. Stating its floor, it replaces only what it speaks for.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness { smallQueueOptions() };
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen(commandBurst(std::string(4000, 'c'), 20));
    auto const ids = recordIds(h.host.terminal(sessionId)->commandBlocks());

    auto client = vthost::client::RemoteScreen {};
    client.apply(Tester::snapshot(*h.session, sessionId)); // the attach
    std::ignore = drainIncrements(h, sessionId, client);
    REQUIRE(heldBlockIds(client) == ids);
    auto const retiredAt = client.commandBlocksRetiredAt;

    auto const resync = Tester::snapshot(*h.session, sessionId); // what a resize sends
    REQUIRE_FALSE(resync.commandBlocks.empty());
    REQUIRE(resync.commandBlocks.size() < ids.size()); // capped
    CHECK(resync.commandBlocksFloor == resync.commandBlocks.front().id);
    client.apply(resync);

    CHECK(client.commandBlocksRetiredAt == retiredAt);
    CHECK(heldBlockIds(client) == ids);
    CHECK(drainIncrements(h, sessionId, client).empty()); // and nothing below the floor trickles again
}

TEST_CASE("records owed to a client that stopped reading wait for room, not for the next window",
          "[vthost][native][blocks]")
{
    // While the send queue has no room, every debounce window would rebuild the session — the lock, the
    // row scan, the live state — only to find no room again, for as long as the client reads nothing.
    // The records wait on the queue's watermark instead, and the session is not queued again until there
    // is room. That they then arrive is what "command blocks wait for a client that stopped reading"
    // proves.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness { smallQueueOptions() };
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    std::ignore = Tester::snapshot(*h.session, sessionId);
    Tester::stallSendQueue(*h.session, SmallQueueBytes / 2);
    h.host.terminal(sessionId)->writeToScreen(commandCycle("ls", 0) + "out");

    auto const delta = Tester::increment(*h.session, sessionId);
    REQUIRE(delta.has_value());
    CHECK_FALSE(delta->lines.empty()); // the rows still travel
    CHECK(delta->commandBlocks.empty());
    CHECK(Tester::recordsAwaitRoom(*h.session, sessionId));
    CHECK_FALSE(Tester::incrementQueued(*h.session, sessionId));
}

TEST_CASE("a snapshot that drops a queued increment still leaves the client with the host's records",
          "[vthost][native][blocks]")
{
    // A resync supersedes whatever is still queued for its session. When that carried a record and a
    // retirement, the client never sees either, and this connection's view of what it holds below the
    // snapshot's cap is wrong: the snapshot has to speak for every id, and the remainder follow in full.
    using Tester = vthost::NativeSessionFollowTester;
    auto options = smallQueueOptions();
    options.blockLimits.maxRecords = 8;
    auto harness = ManualHarness { options };
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    auto* const terminal = h.host.terminal(sessionId);
    terminal->writeToScreen(commandBurst(std::string(4000, 'd'), 8)); // a full store

    auto client = vthost::client::RemoteScreen {};
    client.apply(Tester::snapshot(*h.session, sessionId));
    std::ignore = drainIncrements(h, sessionId, client);
    REQUIRE(heldBlockIds(client) == recordIds(h.host.terminal(sessionId)->commandBlocks()));
    auto const oldest = recordIds(h.host.terminal(sessionId)->commandBlocks()).front();

    // One more cycle evicts the oldest record: an increment retiring it and carrying the new one,
    // queued behind a client that has not read it yet.
    terminal->writeToScreen(commandCycle("evict", 0));
    auto queued = Tester::increment(*h.session, sessionId);
    REQUIRE(queued.has_value());
    REQUIRE(queued->retiredCommandBlocks == std::vector<uint32_t> { oldest });
    REQUIRE_FALSE(queued->commandBlocks.empty());
    Tester::enqueue(*h.session, *std::move(queued));

    auto const resync = Tester::snapshot(*h.session, sessionId); // supersedes the queued increment
    REQUIRE(resync.commandBlocks.size()
            < recordIds(h.host.terminal(sessionId)->commandBlocks()).size()); // capped ...
    CHECK(resync.commandBlocksFloor == 0);                                    // ... yet speaks for all
    client.apply(resync);
    std::ignore = drainIncrements(h, sessionId, client);
    CHECK(heldBlockIds(client)
          == recordIds(h.host.terminal(sessionId)->commandBlocks())); // the evicted record went too
}

TEST_CASE("a snapshot in the middle of a record flood resumes the walk the flood left open",
          "[vthost][native][blocks]")
{
    // A resize while a flood of records drains: the snapshot states the newest, and the walk resumes
    // where it stood below them. The client ends with the host's records, and none the walk already
    // delivered travels twice.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness { smallQueueOptions() };
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    auto client = vthost::client::RemoteScreen {};
    client.apply(Tester::snapshot(*h.session, sessionId)); // the attach, before any record
    h.host.terminal(sessionId)->writeToScreen(commandBurst(std::string(4000, 'f'), 20));

    auto const first = Tester::increment(*h.session, sessionId);
    REQUIRE(first.has_value());
    REQUIRE(Tester::walkFrom(*h.session, sessionId).has_value()); // mid-drain
    client.apply(*first);
    auto delivered = std::vector<uint32_t> {};
    for (auto const& block: first->commandBlocks)
        delivered.push_back(block.id);

    auto const resync = Tester::snapshot(*h.session, sessionId);
    REQUIRE_FALSE(resync.commandBlocks.empty());
    REQUIRE(resync.commandBlocks.front().id > delivered.back()); // the cap left a gap to fill
    CHECK(resync.commandBlocksFloor == resync.commandBlocks.front().id);
    client.apply(resync);
    auto const rest = drainIncrements(h, sessionId, client);

    CHECK(heldBlockIds(client) == recordIds(h.host.terminal(sessionId)->commandBlocks()));
    for (auto const id: delivered)
        CHECK_FALSE(std::ranges::contains(rest, id));
    CHECK(delivered.size() + rest.size() + resync.commandBlocks.size()
          == recordIds(h.host.terminal(sessionId)->commandBlocks()).size());
}

TEST_CASE("a host reset retires every record the client held", "[vthost][native][blocks]")
{
    // RIS clears the store and bumps the grid's generation in one batch. The increment cannot describe
    // that, and the resync that replaces it speaks for every id: nothing from before the reset is left
    // on the client, and what the shell did after it is there.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness {};
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    auto* const terminal = h.host.terminal(sessionId);
    terminal->writeToScreen(commandCycle("ls", 0) + "\r\n" + commandCycle("make", 2) + "\r\n");
    auto client = vthost::client::RemoteScreen {};
    client.apply(Tester::snapshot(*h.session, sessionId));
    auto const before = heldBlockIds(client);
    REQUIRE(before.size() == 2);

    terminal->writeToScreen("\033c" + commandCycle("after", 0));
    REQUIRE(Tester::incrementNeedsResync(*h.session, sessionId));
    auto const resync = Tester::snapshot(*h.session, sessionId);
    CHECK(resync.commandBlocksFloor == 0);
    client.apply(resync);

    auto const after = recordIds(h.host.terminal(sessionId)->commandBlocks());
    REQUIRE(after.size() == 1);
    CHECK(heldBlockIds(client) == after);
    CHECK_FALSE(std::ranges::contains(before, after.front()));
    CHECK(drainIncrements(h, sessionId, client).empty());
}

// ---------------------------------------------------------------------------
// Status-line extensions. They run where the session's terminal is -- here, in the daemon -- so the row
// they draw travels to the clients, which have no frame of theirs.

namespace
{

/// @param left The indicator status line's only segment.
/// @return What a harness's settings take to show the indicator status line.
[[nodiscard]] std::function<void(vtbackend::Settings&)> showingIndicator(std::string left)
{
    return [left = std::move(left)](vtbackend::Settings& settings) {
        settings.statusDisplayType = vtbackend::StatusDisplayType::Indicator;
        settings.indicatorStatusLine.left = left;
        settings.indicatorStatusLine.middle.clear();
        settings.indicatorStatusLine.right.clear();
    };
}

/// @return The indicator row @p frame carries, as text; none if it carries none.
[[nodiscard]] std::optional<std::string> indicatorRowOf(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    if (delta == nullptr || delta->statusLinesChanged == 0
        || delta->statusLinesDisplay != std::to_underlying(vtbackend::StatusDisplayType::Indicator)
        || delta->statusLines.empty())
        return std::nullopt;
    return textOf(delta->statusLines.front());
}

/// @return A collector's stop condition: a frame carrying an indicator row of @p text has arrived.
[[nodiscard]] CollectDone carriesIndicatorRow(std::string text)
{
    return [text = std::move(text)](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, [&](auto const& frame) { return indicatorRowOf(frame) == text; });
    };
}

/// Once @p gate holds: publishes @p vt as @p name's next frame, as the extension's worker does.
Task<void> publishOnce(NativeHarness* h,
                       vthost::testing::FakeSessionExtensions* extensions,
                       std::string name,
                       std::string vt,
                       StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    extensions->publish(name, vthost::testing::frameOf(vt, 2));
}

/// @return A host's extensions whose sessions' indicator row @p name takes over, its frame @p vt.
[[nodiscard]] std::shared_ptr<vthost::testing::FakeExtensionHosting> takingOver(std::string name,
                                                                                std::string vt)
{
    auto hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    hosting->prepare = [name = std::move(name),
                        vt = std::move(vt)](vtbackend::FakeStatusLineExtensions& fake) {
        fake.takeOver(name);
        fake.publish(name, vthost::testing::frameOf(vt));
    };
    return hosting;
}

/// Attaches a second client to @p h's host, as a client attaching after the first detached does, and
/// collects what it receives until @p done holds.
[[nodiscard]] std::vector<proto::DecodedFrame> attachAnother(NativeHarness& h, CollectDone done)
{
    auto pair = makeConnection(h.loop, std::nullopt);
    auto session = std::make_unique<NativeSession>(
        h.loop, h.host, vthost::ConnectionId { .endpoint = "test", .index = 2 }, std::move(pair.first));
    auto const bytes = encodeRequest({ vthost::testing::showingAll(h.host) });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(
        core::net::testing::allOf(session->run(),
                                  feedBytes(pair.second.get(), &bytes),
                                  collectUntil(&h.loop, pair.second.get(), std::move(done), &received)));
    return received;
}

} // namespace

TEST_CASE("NativeSession.extension.statusRowsMirrored", "[vthost][native][extensions]")
{
    SECTION("a take-over's frame is the indicator row the client is sent")
    {
        auto const hosting = takingOver("plasma", "PLASMA");
        auto h = NativeHarness { HarnessOptions { .extensions = hosting,
                                                  .adjustSettings = showingIndicator("template") } };
        auto const session = newSession(h.host);
        REQUIRE(hosting->sessions.size() == 1);

        auto const received = h.exchangeUntil({ proto::ClientHello {} },
                                              carriesIndicatorRow("NEXT"),
                                              publishOnce(&h,
                                                          hosting->sessions.front(),
                                                          "plasma",
                                                          "NEXT",
                                                          holdsFrames(&h.received, AttachSnapshotFrames)));

        // The attach snapshot carries the row the extension drew, not the template the client would draw.
        REQUIRE(received.size() > AttachSnapshotFrames);
        auto const* snapshot = std::get_if<proto::Delta>(&received[AttachSnapshotFrames - 1].pdu);
        REQUIRE(snapshot != nullptr);
        CHECK(snapshot->snapshot == 1);
        CHECK(snapshot->statusLinesChanged == 1);
        CHECK(indicatorRowOf(received[AttachSnapshotFrames - 1]) == "PLASMA");
        // Its next frame follows as an increment.
        auto const* increment = std::get_if<proto::Delta>(&received.back().pdu);
        REQUIRE(increment != nullptr);
        CHECK(increment->snapshot == 0);
        CHECK(increment->statusLinesChanged == 1);
        CHECK(indicatorRowOf(received.back()) == "NEXT");
        CHECK(session.value == increment->session);
    }

    SECTION("an {Extension} segment of the indicator template comes through its row's text")
    {
        auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
        hosting->prepare = [](vtbackend::FakeStatusLineExtensions& fake) {
            fake.publish("git", vthost::testing::frameOf("main"));
        };
        auto h = NativeHarness { HarnessOptions { .extensions = hosting,
                                                  .adjustSettings = showingIndicator("[{Extension:git}]") } };
        std::ignore = newSession(h.host);

        auto const received = h.exchangeUntil(
            { proto::ClientHello {} },
            carriesIndicatorRow("[dev]"),
            publishOnce(
                &h, hosting->sessions.front(), "git", "dev", holdsFrames(&h.received, AttachSnapshotFrames)));
        REQUIRE(received.size() > AttachSnapshotFrames);
        CHECK(indicatorRowOf(received[AttachSnapshotFrames - 1]) == "[main]");
        CHECK(indicatorRowOf(received.back()) == "[dev]");
    }

    SECTION("an indicator no extension draws is the client's own to draw")
    {
        auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
        auto h = NativeHarness { HarnessOptions { .extensions = hosting,
                                                  .adjustSettings = showingIndicator("{VTType}") } };
        std::ignore = newSession(h.host);

        auto const received = h.attach(AttachSnapshotFrames);
        REQUIRE(received.size() == AttachSnapshotFrames);
        auto const* snapshot = std::get_if<proto::Delta>(&received.back().pdu);
        REQUIRE(snapshot != nullptr);
        CHECK(snapshot->statusLinesChanged == 0);
    }
}

TEST_CASE("NativeSession.extension.reattachSnapshot", "[vthost][native][extensions]")
{
    // A client that detaches and attaches again is sent the row as it is now, by extensions that kept
    // running all along: the daemon's session -- and so its extensions -- outlives every client.
    auto const hosting = takingOver("plasma", "PLASMA");
    auto h = NativeHarness { HarnessOptions { .extensions = hosting,
                                              .adjustSettings = showingIndicator("template") } };
    std::ignore = newSession(h.host);
    REQUIRE(hosting->sessions.size() == 1);
    auto* const extensions = hosting->sessions.front();

    auto const first = h.exchangeUntil({ proto::ClientHello {} }, carriesIndicatorRow("PLASMA"));
    REQUIRE(std::ranges::any_of(first, [](auto const& frame) { return indicatorRowOf(frame) == "PLASMA"; }));

    // Drawn while no client is attached.
    extensions->publish("plasma", vthost::testing::frameOf("WHILE AWAY", 2));

    auto const second = attachAnother(h, carriesIndicatorRow("WHILE AWAY"));
    REQUIRE(second.size() >= AttachSnapshotFrames);
    auto const* snapshot = std::get_if<proto::Delta>(&second[AttachSnapshotFrames - 1].pdu);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot == 1);
    CHECK(indicatorRowOf(second[AttachSnapshotFrames - 1]) == "WHILE AWAY");

    // The same extensions, never made anew for a client, and started once.
    CHECK(hosting->sessions.size() == 1);
    CHECK(extensions->starts == 1);
}

namespace
{
/// @return Whether @p frame carries a PermissionRequest.
[[nodiscard]] bool isPermissionRequest(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::PermissionRequest>(frame.pdu);
}

/// @return The capabilities of a client that can answer an extension's permission question.
[[nodiscard]] proto::CapabilitySet answeringPermissions()
{
    return proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions });
}

} // namespace

TEST_CASE("NativeSession.extension.permission", "[vthost][native][extensions]")
{
    // Every client below can answer: one that cannot is never asked (see the [negotiation] cases).
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto h = NativeHarness { HarnessOptions { .extensions = hosting } };
    auto const session = newSession(h.host);
    auto const asked = proto::PermissionRequest {
        .session = 0,
        .hostId = "git@/usr/share/contour/extensions/git.wasm",
        .name = "git",
        .version = 1,
        .digest = "5f3c",
        .commands = { proto::WireDeclaredCommand { .name = "status", .argv = { "git", "status" } } },
    };

    // Asked while no client is attached: the question waits, and is asked of the client that attaches.
    hosting->sessions.front()->askPermission(asked);
    hosting->sessions.front()->askPermission(asked); // a second call into the same question asks nothing more
    // Bounded, as every wait here is: a question that never opens fails the case instead of hanging the run.
    auto const asking = std::chrono::steady_clock::now();
    h.loop.blockOn(core::net::pollUntil(&h.loop, [&] {
        return !h.host.pendingPermissionRequests().empty()
               || std::chrono::steady_clock::now() - asking > CollectBudget;
    }));
    REQUIRE(h.host.pendingPermissionRequests().size() == 1);

    SECTION("an attaching client is asked, and its answer is recorded for every session")
    {
        auto const received = h.exchangeUntil(
            { proto::ClientHello { .capabilities = answeringPermissions() },
              proto::PermissionReply { .hostId = asked.hostId,
                                       .digest = asked.digest,
                                       .grantState = std::to_underlying(vthost::GrantState::Granted) } },
            [&](auto const& got) {
                return std::ranges::any_of(got, isPermissionRequest) && hosting->answers.size() == 1;
            });
        auto const request = std::ranges::find_if(received, isPermissionRequest);
        REQUIRE(request != received.end());
        auto expected = asked;
        expected.session = session.value;
        CHECK(std::get<proto::PermissionRequest>(request->pdu) == expected);
        CHECK(hosting->lookup(asked.hostId, asked.digest) == vthost::GrantState::Granted);
        CHECK(h.host.pendingPermissionRequests().empty());
    }

    // A second question, which the sections below answer or ask after what they test: once its answer or its
    // question has landed, so has everything sent before it.
    auto sentinel = asked;
    sentinel.digest = "sentinel";
    auto const sentinelReply =
        proto::PermissionReply { .hostId = sentinel.hostId,
                                 .digest = sentinel.digest,
                                 .grantState = std::to_underlying(vthost::GrantState::Granted) };
    auto const sentinelAnswered = [&](auto const&) {
        return hosting->lookup(sentinel.hostId, sentinel.digest) == vthost::GrantState::Granted;
    };

    SECTION("an answer byte no GrantState has records nothing, and the question stays open")
    {
        hosting->sessions.front()->askPermission(sentinel);
        std::ignore = h.exchangeUntil(
            { proto::ClientHello { .capabilities = answeringPermissions() },
              proto::PermissionReply { .hostId = asked.hostId, .digest = asked.digest, .grantState = 7 },
              sentinelReply },
            sentinelAnswered);
        REQUIRE(sentinelAnswered(0));
        CHECK(hosting->lookup(asked.hostId, asked.digest) == vthost::GrantState::Pending);
        CHECK(hosting->answered.size() == 1); // the sentinel's alone
        REQUIRE(h.host.pendingPermissionRequests().size() == 1);
        CHECK(h.host.pendingPermissionRequests().front().digest == asked.digest);
    }

    SECTION("an answer to a question nobody asked records nothing")
    {
        // A client is a peer: it may name any extension and digest, and only an open question is its to
        // close.
        hosting->sessions.front()->askPermission(sentinel);
        std::ignore = h.exchangeUntil(
            { proto::ClientHello { .capabilities = answeringPermissions() },
              proto::PermissionReply { .hostId = "other@/x.wasm",
                                       .digest = "never-asked",
                                       .grantState = std::to_underlying(vthost::GrantState::Granted) },
              sentinelReply },
            sentinelAnswered);
        REQUIRE(sentinelAnswered(0));
        CHECK(hosting->lookup("other@/x.wasm", "never-asked") == vthost::GrantState::Pending);
        CHECK(hosting->answered.size() == 1);
    }

    SECTION("a dismissal (no answer) closes the question, and records nothing")
    {
        hosting->sessions.front()->askPermission(sentinel);
        std::ignore = h.exchangeUntil(
            { proto::ClientHello { .capabilities = answeringPermissions() },
              proto::PermissionReply { .hostId = asked.hostId,
                                       .digest = asked.digest,
                                       .grantState = std::to_underlying(vthost::GrantState::Pending) },
              sentinelReply },
            sentinelAnswered);
        REQUIRE(sentinelAnswered(0));
        CHECK(hosting->answered.size() == 1);
        CHECK(h.host.pendingPermissionRequests().empty());
    }

    SECTION("a question answered meanwhile is not asked")
    {
        // Answered through another client: this closes the question the setup opened, and records the answer.
        h.host.answerPermission(asked.hostId, asked.digest, vthost::GrantState::Denied);
        REQUIRE(hosting->lookup(asked.hostId, asked.digest) == vthost::GrantState::Denied);
        hosting->sessions.front()->askPermission(asked);
        // Asked after it, so it opens only once the one before it was turned away.
        hosting->sessions.front()->askPermission(sentinel);
        auto const asking = std::chrono::steady_clock::now();
        h.loop.blockOn(core::net::pollUntil(&h.loop, [&] {
            return !h.host.pendingPermissionRequests().empty()
                   || std::chrono::steady_clock::now() - asking > CollectBudget;
        }));
        REQUIRE(h.host.pendingPermissionRequests().size() == 1);
        CHECK(h.host.pendingPermissionRequests().front().digest == sentinel.digest);
    }
}

namespace
{
/// @return Whether @p frame carries an Unsupported.
[[nodiscard]] bool isUnsupported(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::Unsupported>(frame.pdu);
}
} // namespace

TEST_CASE("an unknown PDU mid-session is answered Unsupported and the session continues",
          "[vthost][negotiation]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    // A newer client's PDU with ident 200 at serial 9, between the hello and an Input.
    auto request = proto::Writer {};
    proto::encodePdu(request, 1, proto::DecodedPdu { proto::ClientHello {} });
    proto::writeFrame(request, 9, 200, {});
    proto::encodePdu(request, 10, inputRequest(sessionId.value, "ls\r"));
    auto const received = h.exchangeBytesUntil(request.take(), [](auto const& got) {
        return std::ranges::any_of(got, isUnsupported) && std::ranges::any_of(got, endsSnapshotRun);
    });

    auto const reply = std::ranges::find_if(received, isUnsupported);
    REQUIRE(reply != received.end());
    CHECK(reply->pdu == proto::DecodedPdu { proto::Unsupported { .refusedSerial = 9, .ident = 200 } });
    CHECK(std::ranges::count_if(received, isUnsupported) == 1);
    // The connection stayed up: the PDU behind the unknown one was served, and the snapshot followed.
    auto& tapped = dynamic_cast<vthost::TappingPty&>(h.host.terminal(sessionId)->device());
    CHECK(dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer() == "ls\r");
    CHECK(std::ranges::any_of(received, endsSnapshotRun));
}

TEST_CASE("Unsupported is never answered with Unsupported", "[vthost][negotiation]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    // The FetchImage behind it is answered, so anything the Unsupported drew would have arrived first.
    auto const received =
        h.exchangeUntil({ proto::ClientHello {},
                          proto::DecodedPdu { proto::Unsupported { .refusedSerial = 1, .ident = 200 } },
                          proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } },
                        imageGoneArrived);

    CHECK(imageGoneArrived(received));
    CHECK_FALSE(std::ranges::any_of(received, isUnsupported));
}

TEST_CASE("a PDU the client refused is not sent to it again", "[vthost][negotiation]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const isLayout = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::LayoutState>(frame.pdu);
    };

    // The hello's LayoutState goes out before the refusal; the CreateTab's, after it, must not.
    auto const received = h.exchangeUntil(
        { proto::ClientHello {},
          proto::DecodedPdu { proto::Unsupported {
              .refusedSerial = 2, .ident = std::to_underlying(proto::PduType::LayoutState) } },
          proto::DecodedPdu { proto::CreateTab {} },
          proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } },
        imageGoneArrived);

    REQUIRE(imageGoneArrived(received));
    CHECK(h.host.model().workspace(h.host.workspaceId())->tabCount() == 2);
    CHECK(std::ranges::count_if(received, isLayout) == 1);
    CHECK(h.session->peerRefused(proto::PduType::LayoutState));
    CHECK_FALSE(h.session->peerRefused(proto::PduType::Delta));
}

TEST_CASE("an unknown PDU before the handshake still ends the connection", "[vthost][negotiation]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    auto const received =
        h.exchange({ proto::DecodedPdu { proto::Invalid { .ident = 200 } }, proto::ClientHello {} }, 2);

    // The refusal every pre-negotiation peer reads, and nothing after it: neither an Unsupported nor the
    // hello behind it is answered.
    REQUIRE(received.size() == 1);
    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->layout == proto::HelloLayout::VersionAlone);
}

// ---------------------------------------------------------------------------
// Version and capability negotiation, daemon side.

namespace
{
/// @param versions The versions the session's handshake admits.
/// @param token The token it requires; empty requires none.
/// @return Harness options whose session admits @p versions and requires @p token.
[[nodiscard]] HarnessOptions admitting(proto::VersionRange versions, std::string token = {})
{
    auto acceptedTokens = std::vector<vthost::AcceptedToken> {};
    if (!token.empty())
        acceptedTokens.push_back(vthost::AcceptedToken { .token = std::move(token) });
    return HarnessOptions { .services = vthost::NativeSessionServices {
                                .handshake = { .acceptedTokens = std::move(acceptedTokens),
                                               .versions = versions } } };
}

/// @param hello The ClientHello to encode.
/// @return Its frame, as a client writes it.
[[nodiscard]] std::vector<std::byte> helloFrame(proto::ClientHello hello)
{
    return encodeRequest({ proto::DecodedPdu { std::move(hello) } });
}

/// The question the cases below have an extension ask.
[[nodiscard]] proto::PermissionRequest gitQuestion()
{
    return proto::PermissionRequest {
        .session = 0,
        .hostId = "git@/usr/share/contour/extensions/git.wasm",
        .name = "git",
        .version = 1,
        .digest = "5f3c",
        .commands = { proto::WireDeclaredCommand { .name = "status", .argv = { "git", "status" } } },
    };
}

/// Waits, bounded, until @p h's host holds an open question.
void awaitOpenQuestion(NativeHarness& h)
{
    auto const asking = std::chrono::steady_clock::now();
    h.loop.blockOn(core::net::pollUntil(&h.loop, [&] {
        return !h.host.pendingPermissionRequests().empty()
               || std::chrono::steady_clock::now() - asking > CollectBudget;
    }));
}

/// Once the attach snapshot has landed: has @p extensions ask @p question, then, once it is open, writes
/// @p sentinel, whose answer arrives after anything the question sent.
Task<void> askWhileAttached(NativeHarness* h,
                            vthost::testing::FakeSessionExtensions* extensions,
                            proto::PermissionRequest question,
                            std::vector<std::byte> const* sentinel)
{
    co_await awaitGate(&h->loop, holdsFrames(&h->received, AttachSnapshotFrames));
    extensions->askPermission(question);
    co_await awaitGate(&h->loop, [h] { return !h->host.pendingPermissionRequests().empty(); });
    std::ignore =
        co_await h->pair.second->write(std::span<std::byte const> { sentinel->data(), sentinel->size() });
}
} // namespace

TEST_CASE("the daemon agrees on the highest common version", "[vthost][negotiation]")
{
    auto h = NativeHarness { admitting({ .min = 4, .max = 6 }) };
    h.host.createTab();

    auto const received = h.exchange(
        { proto::ClientHello { .versions = { .min = 5, .max = 7 }, .capabilities = answeringPermissions() } },
        AttachSnapshotFrames);

    REQUIRE(received.size() == AttachSnapshotFrames);
    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->layout == proto::HelloLayout::Negotiating);
    CHECK(hello->agreedVersion == 6);
    CHECK(hello->versions == proto::VersionRange { .min = 4, .max = 6 });
    CHECK(hello->capabilities == vthost::HandshakePolicy {}.capabilities); // all it offers, not the shared
    REQUIRE(h.session->negotiated().has_value());
    CHECK(h.session->negotiated()->version == 6);
    CHECK(h.session->negotiated()->peerCapabilities == answeringPermissions());
    CHECK(h.session->negotiated()->shared == answeringPermissions());
}

TEST_CASE("the daemon refuses a disjoint range with its own range", "[vthost][negotiation]")
{
    auto capture = core::log::ScopedCapture {};
    auto const reply = answerTo(helloFrame(proto::ClientHello { .versions = { .min = 6, .max = 8 } }),
                                admitting({ .min = 4, .max = 4 }));

    auto const decoded = proto::decodePdu(reply);
    REQUIRE(decoded.has_value());
    CHECK(decoded->consumed == reply.size()); // and the connection closed behind it
    CHECK(decoded->pdu
          == proto::DecodedPdu { proto::ServerHello { .layout = proto::HelloLayout::Negotiating,
                                                      .agreedVersion = 0,
                                                      .versions = { .min = 4, .max = 4 },
                                                      .capabilities = {} } });
    CHECK(capture.contains("handshake rejected: the client speaks protocol 6-8, this daemon speaks 4-4"));
}

TEST_CASE("a codec 3 client is refused in a layout it reads", "[vthost][negotiation]")
{
    auto capture = core::log::ScopedCapture {};
    auto const body = refusalOf(vthost::testing::codec3ClientHello(1));

    CHECK(vthost::testing::codec3ServerHelloVersion(body) == proto::NegotiatingHelloMarker);
    CHECK(capture.contains("handshake rejected: the client speaks protocol 3, from a release before version "
                           "negotiation; this daemon speaks 4-4"));
}

TEST_CASE("a wrong token is refused with the bytes of a version refusal", "[vthost][negotiation]")
{
    auto capture = core::log::ScopedCapture {};

    SECTION("a negotiating hello")
    {
        // The same hello but for its range: once refused for the version alone, once for the token alone.
        auto const versionRefused =
            answerTo(helloFrame(proto::ClientHello { .versions = { .min = 6, .max = 8 }, .token = "right" }),
                     admitting(proto::SupportedVersions, "right"));
        auto const tokenRefused = answerTo(helloFrame(proto::ClientHello { .token = "wrong" }),
                                           admitting(proto::SupportedVersions, "right"));
        REQUIRE_FALSE(versionRefused.empty());
        CHECK(tokenRefused == versionRefused);
        auto const decoded = proto::decodePdu(tokenRefused);
        REQUIRE(decoded.has_value());
        auto const* hello = std::get_if<proto::ServerHello>(&decoded->pdu);
        REQUIRE(hello != nullptr);
        CHECK(hello->layout == proto::HelloLayout::Negotiating);
        CHECK(hello->agreedVersion == 0);
    }

    SECTION("a pre-negotiation hello")
    {
        auto const versionRefused = answerTo(vthost::testing::codec3ClientHello(1));
        auto const alsoTokenGuarded =
            answerTo(vthost::testing::codec3ClientHello(1), admitting(proto::SupportedVersions, "right"));
        REQUIRE_FALSE(versionRefused.empty());
        CHECK(alsoTokenGuarded == versionRefused);
    }

    CHECK(capture.contains("handshake rejected"));
}

TEST_CASE("permission questions wait for a client that can answer them", "[vthost][negotiation]")
{
    SECTION("asked before the client attached")
    {
        // One row per way the capability can fail to be shared: the client lacks it, or the daemon does.
        struct Row
        {
            std::string_view name;
            proto::CapabilitySet daemon;
            proto::CapabilitySet client;
        };
        auto const rows = std::array {
            Row { .name = "the client cannot answer", .daemon = answeringPermissions(), .client = {} },
            Row { .name = "the daemon does not offer it", .daemon = {}, .client = answeringPermissions() },
        };
        for (auto const& row: rows)
        {
            INFO(row.name);
            auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
            auto options = HarnessOptions { .extensions = hosting };
            options.services.handshake.capabilities = row.daemon;
            auto h = NativeHarness { std::move(options) };
            std::ignore = newSession(h.host);
            hosting->sessions.front()->askPermission(gitQuestion());
            awaitOpenQuestion(h);
            REQUIRE(h.host.pendingPermissionRequests().size() == 1);

            auto const received =
                h.exchangeUntil({ proto::ClientHello { .capabilities = row.client } },
                                [](auto const& got) { return std::ranges::any_of(got, endsSnapshotRun); });

            REQUIRE(std::ranges::any_of(received, endsSnapshotRun));
            CHECK_FALSE(std::ranges::any_of(received, isPermissionRequest));
            CHECK(h.host.pendingPermissionRequests().size() == 1); // still open, for another client
        }
    }

    SECTION("asked while it is attached")
    {
        auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
        auto h = NativeHarness { HarnessOptions { .extensions = hosting } };
        std::ignore = newSession(h.host);
        auto const fetch = encodeRequest({ proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } });

        auto const received =
            h.exchangeUntil({ proto::ClientHello {} },
                            imageGoneArrived,
                            askWhileAttached(&h, hosting->sessions.front(), gitQuestion(), &fetch));

        REQUIRE(imageGoneArrived(received));
        CHECK_FALSE(std::ranges::any_of(received, isPermissionRequest));
        CHECK(h.host.pendingPermissionRequests().size() == 1);
    }
}

TEST_CASE("a question for control permissions is asked only of a client that shows them",
          "[vthost][negotiation][control]")
{
    // A client without ControlPermissions would prompt with the commands alone, and its user would grant
    // what they were never shown: it is not asked, and the question waits for another.
    struct Row
    {
        std::string_view name;
        proto::CapabilitySet client;
        bool asked;
    };
    auto const rows = std::array {
        Row { .name = "a client that shows them",
              .client = proto::CapabilitySet::of(
                  { proto::Capability::ExtensionPermissions, proto::Capability::ControlPermissions }),
              .asked = true },
        Row { .name = "a client that would not", .client = answeringPermissions(), .asked = false },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
        auto h = NativeHarness { HarnessOptions { .extensions = hosting } };
        std::ignore = newSession(h.host);
        auto question = gitQuestion();
        question.control = { "input" };
        hosting->sessions.front()->askPermission(question);
        awaitOpenQuestion(h);
        REQUIRE(h.host.pendingPermissionRequests().size() == 1);

        auto const received =
            h.exchangeUntil({ proto::ClientHello { .capabilities = row.client } },
                            [](auto const& got) { return std::ranges::any_of(got, endsSnapshotRun); });

        REQUIRE(std::ranges::any_of(received, endsSnapshotRun));
        auto const request = std::ranges::find_if(received, isPermissionRequest);
        CHECK((request != received.end()) == row.asked);
        if (request != received.end())
            CHECK(std::get<proto::PermissionRequest>(request->pdu).control
                  == std::vector<std::string> { "input" });
        CHECK(h.host.pendingPermissionRequests().size() == 1); // asked, but not answered yet
    }
}

TEST_CASE("a permission question one client could not answer is asked of the next that can",
          "[vthost][negotiation]")
{
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto h = NativeHarness { HarnessOptions { .extensions = hosting } };
    std::ignore = newSession(h.host);
    hosting->sessions.front()->askPermission(gitQuestion());
    awaitOpenQuestion(h);
    REQUIRE(h.host.pendingPermissionRequests().size() == 1);

    // A client that cannot answer attaches, and is passed over.
    auto const passedOver = h.exchangeUntil(
        { proto::ClientHello {} }, [](auto const& got) { return std::ranges::any_of(got, endsSnapshotRun); });
    REQUIRE(std::ranges::any_of(passedOver, endsSnapshotRun));
    CHECK_FALSE(std::ranges::any_of(passedOver, isPermissionRequest));

    // One that can attaches after it, on a connection of its own, and is asked.
    auto pair = makeConnection(h.loop, std::nullopt);
    auto capable = std::make_unique<NativeSession>(h.loop,
                                                   h.host,
                                                   vthost::ConnectionId { .endpoint = "capable", .index = 2 },
                                                   std::move(pair.first),
                                                   NativeSession::DefaultWriteQueueBytes);
    auto const hello = encodeRequest({ vthost::testing::showingAll(
        h.host, proto::ClientHello { .capabilities = answeringPermissions() }) });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(
        capable->run(),
        feedBytes(pair.second.get(), &hello),
        collectUntil(
            &h.loop,
            pair.second.get(),
            [](auto const& got) { return std::ranges::any_of(got, isPermissionRequest); },
            &received)));

    auto const request = std::ranges::find_if(received, isPermissionRequest);
    REQUIRE(request != received.end());
    CHECK(std::get<proto::PermissionRequest>(request->pdu).digest == gitQuestion().digest);
}

namespace
{

/// @return Whether @p frame is a SessionBell.
[[nodiscard]] bool isBell(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::SessionBell>(frame.pdu);
}

/// @return Whether @p frame is a DaemonStatusReply.
[[nodiscard]] bool isStatusReply(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::DaemonStatusReply>(frame.pdu);
}

/// Once @p gate holds: writes @p bytes into @p id the way its application would, so that the host fans
/// the screen update and any bell out to every connection subscribed to it.
/// @param size Receives @p id's page size as it was just before the write.
Task<void> writeThroughHost(
    NativeHarness* h, vtworkspace::SessionId id, std::string bytes, StepGate gate, vtbackend::PageSize* size)
{
    co_await awaitGate(&h->loop, std::move(gate));
    *size = h->host.terminal(id)->pageSize();
    h->host.terminal(id)->writeToScreen(bytes);
}

} // namespace

TEST_CASE("a Control-role hello neither spawns a session nor counts as a client", "[vthost][native][control]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    auto const received = h.exchange(
        { proto::ClientHello { .role = proto::ConnectionRole::Control }, proto::DaemonStatusRequest {} }, 2);
    REQUIRE(received.size() == 2);
    REQUIRE(std::holds_alternative<proto::ServerHello>(received[0].pdu));
    CHECK(std::get<proto::ServerHello>(received[0].pdu).releaseVersion == "9.9.9"); // the fake's release
    REQUIRE(std::holds_alternative<proto::DaemonStatusReply>(received[1].pdu));
    CHECK(received[1].serial == 2); // answered under the request's serial
    CHECK(std::get<proto::DaemonStatusReply>(received[1].pdu).releaseVersion == "9.9.9");
    CHECK(h.host.sessionCount() == 0);
    CHECK(control.census.count() == 0);
    CHECK(control.peakClients == 0);
}

TEST_CASE("a Control connection hears nothing from the host's sessions", "[vthost][native][control]")
{
    // A second, Attach connection on the same host drives a resize, output and a bell into a session while
    // the Control connection is handshaken. The Control connection is sent nothing but the answers to its own
    // requests, and a client area it was never allowed to report leaves the session's size as it was.
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    auto const sessionId = newSession(h.host);

    auto attachPair = makeConnection(h.loop, std::nullopt);
    auto attach = std::make_unique<NativeSession>(h.loop,
                                                  h.host,
                                                  vthost::ConnectionId { .endpoint = "attach", .index = 2 },
                                                  std::move(attachPair.first));
    auto attachReceived = std::vector<proto::DecodedFrame> {};
    auto controlReceived = std::vector<proto::DecodedFrame> {};

    auto const controlHello =
        encodeRequest({ proto::ClientHello { .role = proto::ConnectionRole::Control } });
    auto const attachRequest =
        encodeRequest({ proto::ClientHello {}, proto::ResizeRequest { .columns = 100, .lines = 30 } });
    auto const controlRequest =
        encodeRequest({ proto::ResizeRequest { .columns = 60, .lines = 10 }, proto::DaemonStatusRequest {} });
    auto const attachHeard = [](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, isBell) && std::ranges::any_of(got, isIncrement);
    };
    auto sizeBeforeOutput = vtbackend::PageSize {};

    h.loop.blockOn(core::net::testing::allOf(
        h.session->run(),
        feedBytes(h.pair.second.get(), &controlHello),
        attach->run(),
        feedOnce(&h.loop,
                 attachPair.second.get(),
                 &attachRequest,
                 &controlReceived,
                 [](auto const& got) { return !got.empty(); }),
        writeThroughHost(
            &h,
            sessionId,
            "output\a",
            [&attachReceived] { return std::ranges::any_of(attachReceived, endsSnapshotRun); },
            &sizeBeforeOutput),
        collectUntil(&h.loop, attachPair.second.get(), attachHeard, &attachReceived),
        feedOnce(&h.loop, h.pair.second.get(), &controlRequest, &attachReceived, attachHeard),
        collectUntil(
            &h.loop,
            h.pair.second.get(),
            [](auto const& got) { return std::ranges::any_of(got, isStatusReply); },
            &controlReceived)));

    REQUIRE(attachHeard(attachReceived));
    REQUIRE(controlReceived.size() == 3);
    CHECK(std::holds_alternative<proto::ServerHello>(controlReceived[0].pdu));
    REQUIRE(std::holds_alternative<proto::Unsupported>(controlReceived[1].pdu));
    CHECK(std::get<proto::Unsupported>(controlReceived[1].pdu).ident
          == std::to_underlying(proto::PduType::ResizeRequest));
    CHECK(std::holds_alternative<proto::DaemonStatusReply>(controlReceived[2].pdu));
    CHECK(sizeBeforeOutput.columns == vtbackend::ColumnCount(100));
    CHECK(h.host.terminal(sessionId)->pageSize() == sizeBeforeOutput);
}

TEST_CASE("an Attach hello is admitted to the census for the connection's life", "[vthost][native][control]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    std::ignore = h.attach(4); // ServerHello, LayoutState, SessionState, Delta
    CHECK(control.peakClients == 1);
    CHECK(control.census.count() == 1);
    h.session.reset();
    CHECK(control.census.count() == 0);
}

TEST_CASE("StopDaemon reaches the daemon control with its reason", "[vthost][native][control]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    SECTION("a known reason is passed on, and answered with nothing")
    {
        auto const got = h.exchange({ proto::ClientHello { .role = proto::ConnectionRole::Control },
                                      proto::StopDaemon { .reason = 1 },
                                      proto::DaemonStatusRequest {} },
                                    2);
        CHECK(control.stops == std::vector { proto::StopReason::UpgradeRestart });
        REQUIRE(got.size() == 2);
        CHECK(std::holds_alternative<proto::DaemonStatusReply>(got[1].pdu));
    }
    SECTION("a reason no enumerator has is answered Unsupported")
    {
        auto const got = h.exchange({ proto::ClientHello { .role = proto::ConnectionRole::Control },
                                      proto::StopDaemon { .reason = 200 } },
                                    2);
        CHECK(control.stops.empty());
        REQUIRE(got.size() == 2);
        REQUIRE(std::holds_alternative<proto::Unsupported>(got[1].pdu));
        CHECK(std::get<proto::Unsupported>(got[1].pdu)
              == proto::Unsupported { .refusedSerial = 2,
                                      .ident = std::to_underlying(proto::PduType::StopDaemon) });
    }
}

TEST_CASE("an Attach connection may ask after the daemon too", "[vthost][native][control]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    auto const got =
        h.exchangeUntil({ proto::ClientHello {}, proto::DaemonStatusRequest {} },
                        [](auto const& frames) { return std::ranges::any_of(frames, isStatusReply); });
    auto const reply = std::ranges::find_if(got, isStatusReply);
    REQUIRE(reply != got.end());
    CHECK(reply->serial == 2);
    CHECK(h.host.sessionCount() == 1);
}

TEST_CASE("a Control connection is answered Unsupported for an attach-only request",
          "[vthost][native][control]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    auto const got =
        h.exchange({ proto::ClientHello { .role = proto::ConnectionRole::Control }, proto::CreateTab {} }, 2);
    REQUIRE(got.size() == 2);
    REQUIRE(std::holds_alternative<proto::Unsupported>(got.at(1).pdu));
    CHECK(
        std::get<proto::Unsupported>(got.at(1).pdu)
        == proto::Unsupported { .refusedSerial = 2, .ident = std::to_underlying(proto::PduType::CreateTab) });
    CHECK(h.host.sessionCount() == 0);
}

TEST_CASE("with the null daemon control, status and stop are answered Unsupported",
          "[vthost][native][control]")
{
    auto h = NativeHarness {}; // services.daemon defaults to nullDaemonControl()
    auto const got = h.exchange({ proto::ClientHello { .role = proto::ConnectionRole::Control },
                                  proto::DaemonStatusRequest {},
                                  proto::StopDaemon { .reason = 0 } },
                                3);
    REQUIRE(got.size() == 3);
    CHECK(std::get<proto::ServerHello>(got.at(0).pdu).releaseVersion.empty());
    CHECK(std::get<proto::Unsupported>(got.at(1).pdu)
          == proto::Unsupported { .refusedSerial = 2,
                                  .ident = std::to_underlying(proto::PduType::DaemonStatusRequest) });
    CHECK(std::get<proto::Unsupported>(got.at(2).pdu)
          == proto::Unsupported { .refusedSerial = 3,
                                  .ident = std::to_underlying(proto::PduType::StopDaemon) });
}

namespace
{

using vthost::NativeSessionFlushTester;

/// Turns @p loop until @p ready holds, without ever waiting on its clock: what a case on a ManualClock
/// drives the loop with, since every timed wait there lasts until the case advances the clock.
/// @param turns The most turns to take; each one is non-blocking.
/// @return Whether @p ready held.
[[nodiscard]] bool turnUntil(core::net::EventLoop& loop, std::function<bool()> const& ready, int turns = 1000)
{
    for ([[maybe_unused]] auto const turn: std::views::iota(0, turns))
    {
        if (ready())
            return true;
        std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    }
    return ready();
}

/// Turns @p loop @p turns times, for a case asserting that nothing more happens.
void turnIdle(core::net::EventLoop& loop, int turns = 50)
{
    std::ignore = turnUntil(loop, [] { return false; }, turns);
}

/// Decodes everything the server sends @p client into @p out until the stream ends, then says so.
Task<void> readAll(core::net::ISocket* client, std::vector<proto::DecodedFrame>* out, bool* ended)
{
    co_await decodeUntil(client, [](auto const&) { return false; }, out, {});
    *ended = true;
}

/// @return How many increments are among @p frames.
[[nodiscard]] std::size_t incrementCount(std::vector<proto::DecodedFrame> const& frames)
{
    return static_cast<std::size_t>(std::ranges::count_if(frames, isIncrement));
}

/// One client of a FlushHarness: its connection, handshaken by hand, and what it has read so far.
struct FlushClient
{
    std::unique_ptr<core::net::ISocket> end; ///< The client's end of the connection.
    std::unique_ptr<NativeSession> session;
    std::vector<proto::DecodedFrame> received; ///< What the client decoded so far.
    bool readerEnded = false;                  ///< Whether the client's reader saw the stream end.
};

/// Native connections on a ManualClock, handshaken by hand and turned one loop turn at a time — never
/// through run(), whose waits are on the frozen clock — so each flush deadline is an exact instant.
struct FlushHarness
{
    /// Where the host looks up its workspaces' git state; none tracks none.
    std::optional<vthost::WorkspaceGitServices> workspaceGit = std::nullopt;
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + 10s };
    NativeHarness h { HarnessOptions { .clock = &clock, .workspaceGit = workspaceGit } };
    std::deque<FlushClient> clients {};
    /// Last: every client is let go, and its reader run out, before anything it reaches is destroyed.
    core::Finally settle { [this] {
        for (auto& client: clients)
        {
            if (client.session)
                CHECK(NativeSessionFlushTester::letGo(*client.session, h.loop, clock));
            client.end->close();
        }
        CHECK(turnUntil(h.loop, [this] { return std::ranges::all_of(clients, &FlushClient::readerEnded); }));
    } };

    /// Connects a client, accepts its hello and lets its attach snapshots stream to it.
    /// @param id The session whose snapshot is waited for.
    /// @param hello What the client says of itself.
    /// @return The client, past its attach snapshots.
    FlushClient& attach(vtworkspace::SessionId id, proto::ClientHello hello = {})
    {
        // Every client shows the session's workspace by name: a plain launch of a second client is given a
        // workspace of its own, which would not carry the session.
        if (hello.attachIntent.kind == proto::AttachIntentKind::Default)
        {
            hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::AttachIntent));
            hello.attachIntent = { .kind = proto::AttachIntentKind::Select,
                                   .workspace = std::get<0>(h.host.model().findSessionLeaf(id))->id().value };
        }
        auto& client = connect();
        NativeSessionFlushTester::handshake(*client.session, std::move(hello));
        CHECK(turnUntil(h.loop, [&] {
            return !NativeSessionFlushTester::snapshotPending(*client.session, id)
                   && std::ranges::any_of(client.received, endsSnapshotRun);
        }));
        return client;
    }

    /// Attaches a client and lets its first increment go out, so the follow is past it and only input
    /// makes a flush immediate.
    /// @param id The session to follow.
    /// @return The client, holding exactly one increment.
    FlushClient& attachPastFirstIncrement(vtworkspace::SessionId id)
    {
        auto& client = attach(id);
        update(client, id, "first");
        CHECK(turnUntil(h.loop, [&] { return incrementCount(client.received) == 1; }));
        CHECK_FALSE(NativeSessionFlushTester::armedDeadline(*client.session).has_value());
        return client;
    }

    /// Writes @p text to @p id's screen and tells @p client its screen updated.
    void update(FlushClient& client, vtworkspace::SessionId id, std::string_view text)
    {
        h.host.terminal(id)->writeToScreen(text);
        client.session->sessionScreenUpdated(id);
    }

    /// @return A new connection to the host, reading what it is sent; not yet handshaken.
    FlushClient& connect()
    {
        auto pair = makeConnection(h.loop, std::nullopt);
        auto& client = clients.emplace_back(
            FlushClient { .end = std::move(pair.second),
                          .session = std::make_unique<NativeSession>(
                              h.loop,
                              h.host,
                              vthost::ConnectionId { .endpoint = "flush", .index = clients.size() + 2 },
                              std::move(pair.first),
                              NativeSession::DefaultWriteQueueBytes,
                              h.options.services),
                          .received = {},
                          .readerEnded = false });
        h.loop.spawn(readAll(client.end.get(), &client.received, &client.readerEnded));
        return client;
    }
};

} // namespace

TEST_CASE("a screen update right after input arms an immediate flush", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attachPastFirstIncrement(id);

    f.h.host.writeInput(id, "a");
    f.clock.advance(2ms);
    f.update(client, id, "a");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now());
}

TEST_CASE("a screen update with no recent input coalesces for 20 ms", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attachPastFirstIncrement(id);

    f.update(client, id, "bulk");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now() + 20ms);

    // An input older than the echo window is no reason to hurry either.
    f.h.host.writeInput(id, "a");
    f.clock.advance(51ms);
    turnIdle(f.h.loop); // the coalesced flush above goes out
    REQUIRE(incrementCount(client.received) == 2);
    f.update(client, id, "late");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now() + 20ms);
}

TEST_CASE("an input followed by a flood flushes once at once, then coalesces", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attachPastFirstIncrement(id);

    f.h.host.writeInput(id, "y\r");
    f.update(client, id, "y");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now());
    CHECK(turnUntil(f.h.loop, [&] { return incrementCount(client.received) == 2; }));

    // `yes` answering the Enter: a hundred updates within 5 ms, all inside the echo window, and none of
    // them earns an immediate flush, because the input already got its own.
    auto const floodStart = f.clock.now();
    for ([[maybe_unused]] auto const line: std::views::iota(0, 100))
    {
        f.update(client, id, "y\r\n");
        f.clock.advance(50us);
    }
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == floodStart + 20ms);
    CHECK(NativeSessionFlushTester::flushesInFlight(*client.session) == 1);
    turnIdle(f.h.loop);
    CHECK(incrementCount(client.received) == 2);

    // The whole flood then goes out as one increment, when its window closes.
    f.clock.advance(floodStart + 20ms - f.clock.now());
    CHECK(turnUntil(f.h.loop, [&] { return incrementCount(client.received) == 3; }));
    turnIdle(f.h.loop);
    CHECK(incrementCount(client.received) == 3);
    CHECK_FALSE(NativeSessionFlushTester::armedDeadline(*client.session).has_value());
}

TEST_CASE("the first increment after a snapshot flushes at once", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attach(id);
    REQUIRE(incrementCount(client.received) == 0);

    f.update(client, id, "prompt$ ");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now());
    CHECK(turnUntil(f.h.loop, [&] { return incrementCount(client.received) == 1; }));

    // Only the first: the next one coalesces.
    f.update(client, id, "more");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now() + 20ms);
}

TEST_CASE("a session opened after attaching flushes its first increment at once", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto& client = f.attachPastFirstIncrement(newSession(f.h.host));
    auto const snapshots = [&] {
        return std::ranges::count_if(client.received, endsSnapshotRun);
    };
    auto const before = snapshots();

    // The new tab's first update has no follow to push from yet: its snapshot is owed instead.
    auto const tab = newSession(f.h.host);
    f.update(client, tab, "prompt$ ");
    CHECK(turnUntil(f.h.loop, [&] {
        f.clock.advance(1ms);
        return snapshots() > before && !NativeSessionFlushTester::snapshotPending(*client.session, tab);
    }));

    // That snapshot was the tab's first, not a resync: the output after it flushes at once.
    f.update(client, tab, "output");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now());
}

TEST_CASE("a resync snapshot during a flood does not arm an immediate flush", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attachPastFirstIncrement(id);
    auto const snapshots = [&] {
        return std::ranges::count_if(client.received, endsSnapshotRun);
    };
    auto const before = snapshots();

    // A reset is a batch the increment cannot describe: the coalesced flush asks for a snapshot instead.
    f.update(client, id, "\033cafter the reset");
    CHECK(turnUntil(f.h.loop, [&] {
        f.clock.advance(1ms);
        return snapshots() > before && !NativeSessionFlushTester::snapshotPending(*client.session, id);
    }));

    // The snapshot was no attach: the next update coalesces, as the flood's updates do.
    f.update(client, id, "more");
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now() + 20ms);
}

TEST_CASE("updates during a streaming snapshot never arm an immediate flush", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    // What follows a snapshot put in flight, the same for either section's.
    auto const coalesce = [&](FlushClient& client) {
        REQUIRE(NativeSessionFlushTester::snapshotPending(*client.session, id));

        // A fresh input (and, before a follow's first push, no increment since its snapshot) would make a
        // flush immediate, and would do so for every update until the snapshot is off the wire, where the
        // increment is held back regardless. So they coalesce.
        f.h.host.writeInput(id, "a");
        auto const firstUpdate = f.clock.now();
        for ([[maybe_unused]] auto const update: std::views::iota(0, 10))
        {
            f.update(client, id, "a");
            f.clock.advance(100us);
        }
        CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == firstUpdate + 20ms);
        CHECK(NativeSessionFlushTester::flushesInFlight(*client.session) == 1);

        // Once the snapshot is out, the input's echo credit is still unspent.
        CHECK(turnUntil(f.h.loop,
                        [&] { return !NativeSessionFlushTester::snapshotPending(*client.session, id); }));
        f.update(client, id, "a");
        CHECK(NativeSessionFlushTester::armedDeadline(*client.session) == f.clock.now());
    };
    SECTION("the attach snapshot, before the session's first push")
    {
        auto& client = f.connect();
        NativeSessionFlushTester::handshake(*client.session);
        coalesce(client);
    }
    SECTION("a resync of a session already followed")
    {
        auto& client = f.attachPastFirstIncrement(id);
        client.session->sessionResized(id);
        coalesce(client);
    }
}

TEST_CASE("input from one client flushes another client's mirror at once too", "[vthost][native][flush]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& a = f.attachPastFirstIncrement(id);
    auto& b = f.attachPastFirstIncrement(id);

    // A types; the host fans the echo out to both connections.
    f.h.host.writeInput(id, "a");
    f.clock.advance(1ms);
    f.h.host.terminal(id)->writeToScreen("a");
    a.session->sessionScreenUpdated(id);
    b.session->sessionScreenUpdated(id);
    CHECK(NativeSessionFlushTester::armedDeadline(*a.session) == f.clock.now());
    CHECK(NativeSessionFlushTester::armedDeadline(*b.session) == f.clock.now());
    CHECK(turnUntil(f.h.loop,
                    [&] { return incrementCount(a.received) == 2 && incrementCount(b.received) == 2; }));

    // Each connection spends its own credit: B's next update, with no new input, coalesces.
    f.update(b, id, "b");
    CHECK(NativeSessionFlushTester::armedDeadline(*b.session) == f.clock.now() + 20ms);
}

namespace
{

/// Once @p h's attach snapshot of @p id is out and its first increment has followed: arms a coalescing
/// flush, then an echo flush ahead of it — two flush coroutines live — and closes the client end.
Task<void> armEchoAndCoalesceThenDisconnect(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await awaitGate(&h->loop, [h, id] {
        return std::ranges::any_of(h->received, endsSnapshotRun)
               && !NativeSessionFlushTester::snapshotPending(*h->session, id);
    });
    h->host.terminal(id)->writeToScreen("first");
    h->session->sessionScreenUpdated(id);
    co_await awaitGate(&h->loop, [h] { return incrementCount(h->received) == 1; });

    h->host.terminal(id)->writeToScreen("bulk");
    h->session->sessionScreenUpdated(id); // coalesces: a flusher sleeps 20 ms
    h->host.writeInput(id, "a");
    h->host.terminal(id)->writeToScreen("a");
    h->session->sessionScreenUpdated(id); // the echo: a second flusher, due at once
    CHECK(NativeSessionFlushTester::flushesInFlight(*h->session) == 2);
    h->pair.second->close(); // client gone: run() must settle both
}

} // namespace

TEST_CASE("an echo flush armed at disconnect resolves before run() returns", "[vthost][native][flush]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    std::ignore = h.exchangeUntil(
        { proto::ClientHello {} },
        [](auto const&) { return false; },
        armEchoAndCoalesceThenDisconnect(&h, sessionId));
    CHECK(NativeSessionFlushTester::flushesInFlight(*h.session) == 0);

    // The daemon frees the session the moment run() returns; the superseded coalescing flusher still
    // asleep would then resume on freed memory (ASan turns that into a hard failure right here).
    h.session.reset();
    h.loop.blockOn(core::net::testing::sleepFor(&h.loop, 30ms));
    SUCCEED("both flushes settled before the session was destroyed");
}

namespace
{

/// @param kind What the client asks to follow.
/// @param workspaces The workspaces named, for AttachIntentKind::Select and Resume.
/// @return A hello stating that intent, offering Capability::AttachIntent so that it is honoured.
[[nodiscard]] proto::ClientHello intentHello(proto::AttachIntentKind kind,
                                             std::vector<uint64_t> workspaces = {})
{
    return proto::ClientHello {
        .capabilities = proto::CapabilitySet::of({ proto::Capability::AttachIntent }),
        .attachIntent = { .kind = kind, .workspace = workspaces.empty() ? 0 : workspaces.front() }
    };
}

/// @return The ServerHello among @p frames, or nullptr when none arrived.
[[nodiscard]] proto::ServerHello const* serverHelloOf(std::vector<proto::DecodedFrame> const& frames)
{
    for (auto const& frame: frames)
        if (auto const* hello = std::get_if<proto::ServerHello>(&frame.pdu))
            return hello;
    return nullptr;
}

/// @return The workspaces the LayoutStates among @p frames describe.
[[nodiscard]] std::set<uint64_t> layoutWorkspaces(std::vector<proto::DecodedFrame> const& frames)
{
    auto workspaces = std::set<uint64_t> {};
    for (auto const& frame: frames)
        if (auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu))
            workspaces.insert(layout->workspace);
    return workspaces;
}

/// @return The last LayoutState among @p frames for @p workspace, or nullptr when none arrived.
[[nodiscard]] proto::LayoutState const* lastLayoutOf(std::vector<proto::DecodedFrame> const& frames,
                                                     uint64_t workspace)
{
    proto::LayoutState const* found = nullptr;
    for (auto const& frame: frames)
        if (auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu);
            layout && layout->workspace == workspace)
            found = layout;
    return found;
}

/// @return The sessions a SessionState among @p frames snapshots.
[[nodiscard]] std::set<uint64_t> snapshotSessions(std::vector<proto::DecodedFrame> const& frames)
{
    auto sessions = std::set<uint64_t> {};
    for (auto const& frame: frames)
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            sessions.insert(state->session);
    return sessions;
}

/// @return The sessions @p host hosts in @p workspace.
[[nodiscard]] std::set<uint64_t> sessionsOf(SessionHost& host, uint64_t workspace)
{
    auto sessions = std::set<uint64_t> {};
    host.model().forEachTab([&](vtworkspace::Workspace& owner, vtworkspace::Tab& tab) {
        if (owner.id().value != workspace)
            return;
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                sessions.insert(pane.session().value);
        });
    });
    return sessions;
}

/// @return How many snapshot runs @p frames completes.
[[nodiscard]] std::size_t snapshotRunCount(std::vector<proto::DecodedFrame> const& frames)
{
    return static_cast<std::size_t>(std::ranges::count_if(frames, endsSnapshotRun));
}

/// How long a probe waits after its gate before it asks, so that whatever the other connection's step
/// raised — flushes coalesce for 20 ms — has reached this connection if it was ever going to.
constexpr auto ProbeSettle = 100ms;

/// Once @p gate holds and ProbeSettle has passed, asks @p client's server for an image no session has: the
/// ImageGone that answers it comes after everything the server sent this connection before.
Task<void> probeOnce(core::net::EventLoop* loop, core::net::ISocket* client, StepGate gate)
{
    co_await awaitGate(loop, std::move(gate));
    co_await loop->delay(ProbeSettle);
    auto const bytes = encodeRequest({ proto::FetchImage { .imageId = 4242 } });
    std::ignore = co_await client->write(std::span<std::byte const> { bytes.data(), bytes.size() });
}

} // namespace

namespace
{

/// An attached client that only selects, so a test can make the host remember a selection.
class RememberedSelection final: public vthost::SessionStreamEvents
{
  public:
    [[nodiscard]] uint64_t selectedWorkspace() const noexcept override { return selected; }

    uint64_t selected = 0; ///< The workspace it selects.
};

/// @return The sessions the Deltas among @p frames describe.
[[nodiscard]] std::set<uint64_t> deltaSessions(std::vector<proto::DecodedFrame> const& frames)
{
    auto sessions = std::set<uint64_t> {};
    for (auto const& frame: frames)
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
            delta != nullptr && delta->snapshot == 0)
            sessions.insert(delta->session);
    return sessions;
}

} // namespace

TEST_CASE("a lone client selects the most recently selected workspace", "[vthost][native][attach-intent]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* second = h.host.createWorkspace();
    auto const* third = h.host.createWorkspace();
    REQUIRE((second != nullptr && third != nullptr));
    auto const workspaces = std::set<uint64_t> { first, second->id().value, third->id().value };

    auto const attachesTo = [&](uint64_t expected) {
        auto const received = h.exchangeUntil(
            { intentHello(proto::AttachIntentKind::Default), proto::FetchImage { .imageId = 4242 } },
            [](auto const& got) { return imageGoneArrived(got) && snapshotRunCount(got) >= 1; });

        auto const* hello = serverHelloOf(received);
        REQUIRE(hello != nullptr);
        CHECK(hello->selectedWorkspace == expected);
        CHECK(layoutWorkspaces(received) == workspaces);
        CHECK(snapshotSessions(received) == sessionsOf(h.host, expected));
    };

    SECTION("no window ever selected one: the first workspace")
    {
        attachesTo(first);
    }
    SECTION("a window selected the second: it is the default, though that window is gone")
    {
        auto earlier = RememberedSelection {};
        h.host.noteClientAttached(&earlier);
        h.host.noteSelected(earlier, second->id().value);
        h.host.noteClientDetached(&earlier);
        attachesTo(second->id().value);
    }
}

TEST_CASE("a second client sees every workspace's layout, and mirrors only the workspace it selected",
          "[vthost][native][attach-intent]")
{
    auto h = TwoClientHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* second = h.host.createWorkspace();
    auto const* third = h.host.createWorkspace();
    REQUIRE((second != nullptr && third != nullptr));
    auto const thirdWorkspace = third->id().value;
    auto const workspaces = std::set<uint64_t> { first, second->id().value, thirdWorkspace };
    auto const helloOne = encodeRequest({ intentHello(proto::AttachIntentKind::Default) });
    auto const helloTwo = encodeRequest({ intentHello(proto::AttachIntentKind::Select, { thirdWorkspace }) });

    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};
    auto const seesAll = [&](std::vector<proto::DecodedFrame> const& got) {
        return layoutWorkspaces(got) == workspaces && snapshotRunCount(got) >= 1;
    };
    h.loop.blockOn(core::net::testing::allOf(
        h.serverOne->run(),
        h.serverTwo->run(),
        feedBytes(h.firstPair.second.get(), &helloOne),
        feedBytes(h.secondPair.second.get(), &helloTwo),
        probeOnce(&h.loop, h.firstPair.second.get(), [&] { return seesAll(fromOne); }),
        probeOnce(&h.loop, h.secondPair.second.get(), [&] { return seesAll(fromTwo); }),
        collectUntil(&h.loop, h.firstPair.second.get(), imageGoneArrived, &fromOne),
        collectUntil(&h.loop, h.secondPair.second.get(), imageGoneArrived, &fromTwo)));

    auto const* oneHello = serverHelloOf(fromOne);
    auto const* twoHello = serverHelloOf(fromTwo);
    REQUIRE((oneHello != nullptr && twoHello != nullptr));
    CHECK(oneHello->selectedWorkspace == first);
    CHECK(twoHello->selectedWorkspace == thirdWorkspace);
    CHECK(layoutWorkspaces(fromOne) == workspaces);
    CHECK(layoutWorkspaces(fromTwo) == workspaces);
    CHECK(snapshotSessions(fromOne) == sessionsOf(h.host, first));
    CHECK(snapshotSessions(fromTwo) == sessionsOf(h.host, thirdWorkspace));
    CHECK(h.host.visibility(first) == vthost::WorkspaceVisibility::Shown);
    CHECK(h.host.visibility(thirdWorkspace) == vthost::WorkspaceVisibility::Shown);
    CHECK(h.host.visibility(second->id().value) == vthost::WorkspaceVisibility::Hidden);
}

TEST_CASE("a Select naming an unknown workspace selects what a launch would",
          "[vthost][native][attach-intent]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;

    auto const received = h.exchangeUntil(
        { intentHello(proto::AttachIntentKind::Select, { 999 }), proto::FetchImage { .imageId = 4242 } },
        [](auto const& got) { return imageGoneArrived(got) && snapshotRunCount(got) >= 1; });

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK(hello->selectedWorkspace == first);
    CHECK(snapshotSessions(received) == sessionsOf(h.host, first));
}

TEST_CASE("a client without the AttachIntent capability attaches as a launch",
          "[vthost][native][attach-intent]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* second = h.host.createWorkspace();
    REQUIRE(second != nullptr);
    // The intent is stated but not offered, so it is not honoured: this is how a client predating intents
    // attaches. It is a launch, which selects the first workspace, not the one named.
    auto const received =
        h.exchangeUntil({ proto::ClientHello { .attachIntent = { .kind = proto::AttachIntentKind::Select,
                                                                 .workspace = second->id().value } },
                          proto::FetchImage { .imageId = 4242 } },
                        imageGoneArrived);

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK(hello->selectedWorkspace == first);
}

TEST_CASE("a permission question is not asked of a client that does not mirror its pane",
          "[vthost][native][attach-intent][extensions]")
{
    // Asked in its pane, so a client selecting another workspace has nowhere to ask it.
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto h = NativeHarness { HarnessOptions { .extensions = hosting } };
    std::ignore = newSession(h.host);
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const shown = other->id().value;
    REQUIRE(hosting->sessions.size() == 2);
    hosting->sessions.front()->askPermission(proto::PermissionRequest { .session = 0,
                                                                        .hostId = "git@/x.wasm",
                                                                        .name = "git",
                                                                        .version = 1,
                                                                        .digest = "5f3c",
                                                                        .commands = {} });
    auto const asking = std::chrono::steady_clock::now();
    h.loop.blockOn(core::net::pollUntil(&h.loop, [&] {
        return !h.host.pendingPermissionRequests().empty()
               || std::chrono::steady_clock::now() - asking > CollectBudget;
    }));
    REQUIRE(h.host.pendingPermissionRequests().size() == 1);

    auto hello = intentHello(proto::AttachIntentKind::Select, { shown });
    hello.capabilities = proto::CapabilitySet::of(
        { proto::Capability::AttachIntent, proto::Capability::ExtensionPermissions });
    auto const received =
        h.exchangeUntil({ hello }, [](auto const& got) { return snapshotRunCount(got) >= 1; });

    REQUIRE(snapshotRunCount(received) >= 1);
    CHECK(layoutWorkspaces(received) == std::set<uint64_t> { h.host.workspaceId().value, shown });
    CHECK_FALSE(std::ranges::any_of(received, isPermissionRequest));
    CHECK(h.host.pendingPermissionRequests().size() == 1); // still waiting for a client that mirrors it
}

TEST_CASE("a workspace created elsewhere is laid out to every connection and mirrored by none",
          "[vthost][native][attach-intent]")
{
    // What the control API's tab.create does: the workspace belongs to no connection.
    auto h = TwoClientHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const hello = encodeRequest({ intentHello(proto::AttachIntentKind::Default) });

    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};
    auto laterWorkspace = uint64_t { 0 };
    auto const createElsewhere = [](TwoClientHarness* h,
                                    std::vector<proto::DecodedFrame> const* fromTwo,
                                    uint64_t* laterWorkspace) -> Task<void> {
        co_await awaitGate(&h->loop, [fromTwo] { return snapshotRunCount(*fromTwo) >= 1; });
        auto const* workspace = h->host.createWorkspace();
        REQUIRE(workspace != nullptr);
        *laterWorkspace = workspace->id().value;
    };
    auto const bothSeeIt = [&laterWorkspace](std::vector<proto::DecodedFrame> const& got) {
        return laterWorkspace != 0 && layoutWorkspaces(got).contains(laterWorkspace);
    };
    h.loop.blockOn(core::net::testing::allOf(
        h.serverOne->run(),
        h.serverTwo->run(),
        feedBytes(h.firstPair.second.get(), &hello),
        feedOnce(&h.loop,
                 h.secondPair.second.get(),
                 &hello,
                 &fromOne,
                 [](auto const& got) { return snapshotRunCount(got) >= 1; }),
        createElsewhere(&h, &fromTwo, &laterWorkspace),
        probeOnce(
            &h.loop, h.firstPair.second.get(), [&] { return bothSeeIt(fromOne) && bothSeeIt(fromTwo); }),
        collectUntil(&h.loop, h.firstPair.second.get(), imageGoneArrived, &fromOne),
        collectUntil(&h.loop, h.secondPair.second.get(), bothSeeIt, &fromTwo)));

    REQUIRE(bothSeeIt(fromOne));
    REQUIRE(bothSeeIt(fromTwo));
    CHECK(layoutWorkspaces(fromOne) == std::set<uint64_t> { first, laterWorkspace });
    CHECK(snapshotSessions(fromOne) == sessionsOf(h.host, first));
    CHECK(snapshotSessions(fromTwo) == sessionsOf(h.host, first));
}

TEST_CASE("the host counts an Attach connection as a client beside the census",
          "[vthost][native][attach-intent]")
{
    // Ruling C1: two counters answering different questions, kept in step on connect and close.
    auto control = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { HarnessOptions { .services = { .daemon = control } } };
    std::ignore = h.attach(AttachSnapshotFrames);
    CHECK(h.host.connectedClients() == 1);
    CHECK(control.census.count() == 1);
    h.session.reset();
    CHECK(h.host.connectedClients() == 0);
    CHECK(control.census.count() == 0);
}

TEST_CASE("a Control connection is not counted as the host's client", "[vthost][native][attach-intent]")
{
    auto h = NativeHarness {};
    std::ignore = h.exchange({ proto::ClientHello { .role = proto::ConnectionRole::Control } }, 1);
    CHECK(h.host.connectedClients() == 0);
}

namespace
{

/// @return A harness whose host resolves "main" (the default, 1000 lines) and "build" (5000 lines) and
///         "ops" (2000 lines), and whose handshake offers Capability::ProfileByName.
HarnessOptions profileOptions()
{
    using vthost::testing::fakeProfile;
    auto options = HarnessOptions {};
    options.profiles = std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> {
            { "main", fakeProfile("/bin/zsh", vtbackend::LineCount(1000)) },
            { "build", fakeProfile("/bin/build-shell", vtbackend::LineCount(5000)) },
            { "ops", fakeProfile("/bin/ops-shell", vtbackend::LineCount(2000)) },
        },
        "main");
    options.services.handshake.capabilities =
        proto::CapabilitySet::of({ proto::Capability::InitialClientArea, proto::Capability::ProfileByName });
    return options;
}

/// @return A hello naming @p profile, stating VT420 settings of 7000 lines beside it.
proto::ClientHello profileHello(std::string profile, proto::CapabilitySet capabilities)
{
    return proto::ClientHello {
        .capabilities = std::move(capabilities),
        .sessionSettings = proto::WireSessionSettings { .historyLineCount = 7000,
                                                        .terminalId = 41, // VT420
                                                        .graphemeClustering = 0,
                                                        .allowReflowOnResize = 0,
                                                        .maxImageRegisterCount = 512,
                                                        .sixelScrolling = 0 },
        .profileName = std::move(profile),
    };
}

/// @return The profile every session @p host hosts was spawned under, in session order.
std::vector<std::string> profilesOf(SessionHost& host)
{
    auto sessions = std::set<uint64_t> {};
    host.model().forEachTab([&](vtworkspace::Workspace&, vtworkspace::Tab& tab) {
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                sessions.insert(pane.session().value);
        });
    });
    auto names = std::vector<std::string> {};
    for (auto const session: sessions)
        names.push_back(host.hostedSession(vtworkspace::SessionId { session })->profileName());
    return names;
}

} // namespace

TEST_CASE("a client naming a profile gets it; its session settings are ignored", "[vthost][native][profiles]")
{
    auto h = NativeHarness { profileOptions() };
    std::ignore =
        h.exchange({ profileHello("build", proto::CapabilitySet::of({ proto::Capability::ProfileByName })) },
                   AttachSnapshotFrames);

    CHECK(profilesOf(h.host) == std::vector<std::string> { "build" });
    auto const session = h.host.model().workspace(h.host.workspaceId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(session);
    REQUIRE(terminal != nullptr);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount()) == 5000);
    CHECK(terminal->terminalId() != vtbackend::VTType::VT420);
}

TEST_CASE("a CreateTab's profile overrides the connection's", "[vthost][native][profiles]")
{
    auto h = NativeHarness { profileOptions() };
    std::ignore = h.exchangeUntil(
        { profileHello("build", proto::CapabilitySet::of({ proto::Capability::ProfileByName })),
          proto::CreateTab { .session = 0, .profileName = "ops" },
          proto::CreateTab { .session = 0, .profileName = {} } },
        [workspace = h.host.workspaceId().value](auto const& got) {
            auto const* layout = lastLayoutOf(got, workspace);
            return layout != nullptr && layout->tabs.size() == 3;
        });
    CHECK(profilesOf(h.host) == std::vector<std::string> { "build", "ops", "build" });
}

TEST_CASE("a client without ProfileByName keeps the session-settings path", "[vthost][native][profiles]")
{
    auto h = NativeHarness { profileOptions() };
    std::ignore = h.exchangeUntil(
        { profileHello("build", {}), proto::CreateTab { .session = 0, .profileName = "ops" } },
        [workspace = h.host.workspaceId().value](auto const& got) {
            auto const* layout = lastLayoutOf(got, workspace);
            return layout != nullptr && layout->tabs.size() == 2;
        });

    CHECK(profilesOf(h.host) == std::vector<std::string> { std::string {}, std::string {} });
    auto const session = h.host.model().workspace(h.host.workspaceId())->tabAt(0)->rootPane()->session();
    auto* terminal = h.host.terminal(session);
    REQUIRE(terminal != nullptr);
    CHECK(terminal->terminalId() == vtbackend::VTType::VT420);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount()) == 7000);
}

TEST_CASE("a title one client sets reaches the other client's layout", "[vthost][native][tab-edits]")
{
    auto h = TwoClientHarness {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session().value;
    auto const editing =
        proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::TabEdits }) };
    auto const watcherHello = encodeRequest({ editing });
    auto const editorRequest = encodeRequest(
        { editing,
          proto::EditTab { .session = session, .kind = proto::TabEditKind::SetTitle, .title = "build" } });

    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};
    auto const titled = [workspace =
                             h.host.workspaceId().value](std::vector<proto::DecodedFrame> const& got) {
        auto const* layout = lastLayoutOf(got, workspace);
        return layout != nullptr && !layout->tabs.empty() && layout->tabs.front().title == "build";
    };
    h.loop.blockOn(
        core::net::testing::allOf(h.serverOne->run(),
                                  h.serverTwo->run(),
                                  feedBytes(h.secondPair.second.get(), &watcherHello),
                                  feedOnce(&h.loop,
                                           h.firstPair.second.get(),
                                           &editorRequest,
                                           &fromTwo,
                                           [](auto const& got) { return snapshotRunCount(got) >= 1; }),
                                  collectUntil(&h.loop, h.firstPair.second.get(), titled, &fromOne),
                                  collectUntil(&h.loop, h.secondPair.second.get(), titled, &fromTwo)));

    CHECK(titled(fromTwo));
    CHECK(tab->runtimeTitle() == "build");
}

TEST_CASE("an EditTab from a client without TabEdits is answered Unsupported", "[vthost][native][tab-edits]")
{
    auto h = NativeHarness {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const received =
        h.exchangeUntil({ proto::ClientHello {},
                          proto::EditTab { .session = tab->rootPane()->session().value,
                                           .kind = proto::TabEditKind::SetTitle,
                                           .title = "x" } },
                        [](auto const& got) {
                            return std::ranges::any_of(got, [](auto const& frame) {
                                return std::holds_alternative<proto::Unsupported>(frame.pdu);
                            });
                        });
    auto const refused = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
    REQUIRE(refused != received.end());
    CHECK(std::get<proto::Unsupported>(refused->pdu).ident == std::to_underlying(proto::PduType::EditTab));
    CHECK_FALSE(tab->runtimeTitle());
}

TEST_CASE("a MergeTabIntoSplit grafts the tab, keeps its sessions running and pushes the merged layout",
          "[vthost][native][tab-merge][daemon]")
{
    auto h = NativeHarness {};
    auto const* target = h.host.createTab();
    auto const* dragged = h.host.createTab();
    REQUIRE((target != nullptr && dragged != nullptr));
    auto const targetSession = target->rootPane()->session();
    auto const draggedSession = dragged->rootPane()->session();
    auto const workspace = h.host.workspaceId().value;
    auto const tabCount = [&] {
        return h.host.model().workspace(h.host.workspaceId())->tabCount();
    };
    auto const tabsBefore = tabCount();
    REQUIRE(tabsBefore >= 2);

    auto const merging =
        proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                 { proto::Capability::TabMerge, proto::Capability::Heartbeat }) };
    // The verb is served in order, so the heartbeat's echo arrives after every push the merge made.
    auto const received = h.exchangeUntil(
        { merging,
          proto::MergeTabIntoSplit { .tabSessions = { draggedSession.value },
                                     .targetSession = targetSession.value,
                                     .side = std::to_underlying(vtworkspace::SplitSide::Right) },
          proto::Heartbeat { .nonce = 1 } },
        [&](auto const& got) {
            return std::ranges::any_of(
                got, [](auto const& frame) { return std::holds_alternative<proto::Heartbeat>(frame.pdu); });
        });
    auto const pushesOfMergedLayout = std::ranges::count_if(received, [&](auto const& frame) {
        auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu);
        return layout != nullptr && layout->workspace == workspace
               && std::cmp_equal(layout->tabs.size(), tabsBefore - 1);
    });
    INFO("pushes of the merged layout: " << pushesOfMergedLayout);
    // Not one: every model event the merge fires pushes the workspace's layout (the tab closed, the active
    // tab and pane changed, the tree restructured, ...), each of them already the merged one. A change to the
    // number is a decision about the clients' reconciliation, not an accident.
    constexpr auto MergePushCount = 5;
    CHECK(pushesOfMergedLayout == MergePushCount);

    CHECK(std::cmp_equal(tabCount(), tabsBefore - 1));
    auto const [mergedWorkspace, mergedTab, mergedLeaf] = h.host.model().findSessionLeaf(draggedSession);
    CHECK(mergedTab == target);
    CHECK(mergedLeaf != nullptr);
    CHECK(target->paneCount() == 2);
    // Moved, not re-created: both sessions are the ones that were already running.
    CHECK(h.host.terminal(targetSession) != nullptr);
    CHECK(h.host.terminal(draggedSession) != nullptr);
    CHECK_FALSE(std::ranges::any_of(
        received, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); }));
}

TEST_CASE("a MergeTabIntoSplit the daemon refuses is answered TabMergeRefused, under its serial",
          "[vthost][native][tab-merge][daemon]")
{
    auto h = NativeHarness {};
    auto const* target = h.host.createTab();
    auto const* dragged = h.host.createTab();
    REQUIRE((target != nullptr && dragged != nullptr));
    auto const tabsBefore = h.host.model().workspace(h.host.workspaceId())->tabCount();

    // The request names a tab as holding a session it does not: a stale view of it.
    auto const isRefusal = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::TabMergeRefused>(frame.pdu);
    };
    auto const received = h.exchangeUntil(
        { proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::TabMerge }) },
          proto::MergeTabIntoSplit { .tabSessions = { dragged->rootPane()->session().value, 9999 },
                                     .targetSession = target->rootPane()->session().value,
                                     .side = std::to_underlying(vtworkspace::SplitSide::Right) } },
        [&](auto const& got) { return std::ranges::any_of(got, isRefusal); });
    auto const refusal = std::ranges::find_if(received, isRefusal);
    REQUIRE(refusal != received.end());
    CHECK(refusal->serial == 2); // the merge's: the second frame the client sent
    CHECK(std::get<proto::TabMergeRefused>(refusal->pdu).reason
          == std::to_underlying(vthost::TabMergeError::StaleTab));
    CHECK(h.host.model().workspace(h.host.workspaceId())->tabCount() == tabsBefore);
    CHECK(target->paneCount() == 1);
}

TEST_CASE("a MergeTabIntoSplit from a client without TabMerge is answered Unsupported and changes nothing",
          "[vthost][native][tab-merge][daemon]")
{
    auto h = NativeHarness {};
    auto const* target = h.host.createTab();
    auto const* dragged = h.host.createTab();
    REQUIRE((target != nullptr && dragged != nullptr));
    auto const tabsBefore = h.host.model().workspace(h.host.workspaceId())->tabCount();

    // An older client's hello offers nothing, and a daemon of today still refuses what it did not negotiate.
    auto const received = h.exchangeUntil(
        { proto::ClientHello {},
          proto::MergeTabIntoSplit { .tabSessions = { dragged->rootPane()->session().value },
                                     .targetSession = target->rootPane()->session().value,
                                     .side = std::to_underlying(vtworkspace::SplitSide::Right) } },
        [](auto const& got) {
            return std::ranges::any_of(
                got, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
        });
    auto const refused = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
    REQUIRE(refused != received.end());
    CHECK(std::get<proto::Unsupported>(refused->pdu).ident
          == std::to_underlying(proto::PduType::MergeTabIntoSplit));
    CHECK(h.host.model().workspace(h.host.workspaceId())->tabCount() == tabsBefore);
    CHECK(target->paneCount() == 1);
}

TEST_CASE("the daemon's default handshake offers TabMerge", "[vthost][native][tab-merge][daemon]")
{
    CHECK(vthost::HandshakePolicy {}.capabilities.contains(proto::Capability::TabMerge));
}

TEST_CASE("the daemon echoes a heartbeat under its serial", "[vthost][native][liveness]")
{
    auto h = NativeHarness {};
    auto const isEcho = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::Heartbeat>(frame.pdu);
    };
    auto const received = h.exchangeUntil(
        { proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::Heartbeat }),
                               .role = proto::ConnectionRole::Control },
          proto::Heartbeat { .nonce = 77 } },
        [&](auto const& got) { return std::ranges::any_of(got, isEcho); });
    auto const echo = std::ranges::find_if(received, isEcho);
    REQUIRE(echo != received.end());
    CHECK(echo->serial == 2); // the request's
    CHECK(std::get<proto::Heartbeat>(echo->pdu).nonce == 77);
}

TEST_CASE("a heartbeat without the capability is answered Unsupported", "[vthost][native][liveness]")
{
    auto h = NativeHarness {};
    auto const isRefusal = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::Unsupported>(frame.pdu);
    };
    auto const received = h.exchangeUntil(
        { proto::ClientHello { .role = proto::ConnectionRole::Control }, proto::Heartbeat { .nonce = 1 } },
        [&](auto const& got) { return std::ranges::any_of(got, isRefusal); });
    auto const refusal = std::ranges::find_if(received, isRefusal);
    REQUIRE(refusal != received.end());
    CHECK(std::get<proto::Unsupported>(refusal->pdu).ident == std::to_underlying(proto::PduType::Heartbeat));
}

namespace
{

/// @return A harness whose connection calls a ControlService over its host, and whose handshake offers it.
HarnessOptions controlApiOptions(HarnessOptions options = {})
{
    options.control = ControlServing::Served;
    options.services.handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi });
    return options;
}

/// @return A Control hello that offers the control API.
proto::ClientHello controlApiHello()
{
    return proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }),
                                .role = proto::ConnectionRole::Control };
}

/// @return One control call.
proto::ControlRequest controlCall(uint64_t requestId, std::string method, std::string params = {})
{
    return proto::ControlRequest {
        .requestId = requestId, .method = std::move(method), .params = std::move(params), .context = {}
    };
}

[[nodiscard]] bool isControlReply(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::ControlReply>(frame.pdu);
}

[[nodiscard]] bool viewsAnything(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::LayoutState>(frame.pdu)
           || std::holds_alternative<proto::SessionState>(frame.pdu)
           || std::holds_alternative<proto::Delta>(frame.pdu);
}

/// @return The control events among @p frames, decoded, in order.
[[nodiscard]] std::vector<vtcontrol::Event> controlEvents(std::vector<proto::DecodedFrame> const& frames)
{
    auto events = std::vector<vtcontrol::Event> {};
    for (auto const& frame: frames)
        if (auto const* event = std::get_if<proto::ControlEvent>(&frame.pdu))
            if (auto decoded = vtcontrol::decodeEvent(event->event))
                events.push_back(std::move(*decoded));
    return events;
}

template <typename Payload>
[[nodiscard]] std::size_t countEvents(std::vector<proto::DecodedFrame> const& frames)
{
    return static_cast<std::size_t>(std::ranges::count_if(controlEvents(frames), [](auto const& event) {
        return std::holds_alternative<Payload>(event.payload);
    }));
}

/// Decodes server PDUs into @p out until @p done holds of them, keeping what it read past a frame in
/// @p buffer, so that it can be called again where it stopped.
Task<void> readFramesUntil(core::net::ISocket* client,
                           std::vector<std::byte>* buffer,
                           std::vector<proto::DecodedFrame>* out,
                           CollectDone done)
{
    auto scratch = std::vector<std::byte>(4096);
    while (!done(*out))
    {
        if (auto decoded = proto::decodePdu(*buffer))
        {
            buffer->erase(buffer->begin(), buffer->begin() + static_cast<long>(decoded->consumed));
            out->push_back(std::move(*decoded));
            continue;
        }
        auto const n = co_await client->read(scratch);
        if (!n.has_value() || *n == 0)
            co_return;
        buffer->insert(buffer->end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
    }
}

/// Runs @p session to its end, then destroys it, as the daemon's flow does.
Task<void> runAndRelease(std::unique_ptr<NativeSession>* session)
{
    co_await (*session)->run();
    session->reset();
}

/// A control client that subscribes to bells, stops reading while @p bells of them ring, then reads again,
/// ringing one more bell per frame it reads, until a bell arrived after an events.dropped.
Task<void> floodedSubscriber(NativeHarness* h,
                             vtworkspace::SessionId session,
                             std::size_t bells,
                             std::vector<proto::DecodedFrame>* out)
{
    auto* client = h->pair.second.get();
    auto buffer = std::vector<std::byte> {};
    co_await readFramesUntil(
        client, &buffer, out, [](auto const& got) { return std::ranges::any_of(got, isControlReply); });

    // Rung while nothing is read: the connection's backlog fills, and the rest is counted, not queued.
    h->host.terminal(session)->writeToScreen(std::string(bells, '\a'));
    co_await h->loop.delay(100ms);

    auto const recovered = [](std::vector<proto::DecodedFrame> const& got) {
        auto const events = controlEvents(got);
        auto const dropped = std::ranges::find_if(events, [](auto const& e) {
            return std::holds_alternative<vtcontrol::EventsDroppedEvent>(e.payload);
        });
        return dropped != events.end() && std::any_of(std::next(dropped), events.end(), [](auto const& e) {
                   return std::holds_alternative<vtcontrol::BellEvent>(e.payload);
               });
    };
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 5000))
    {
        if (recovered(*out))
            break;
        h->host.terminal(session)->writeToScreen("\a");
        auto const before = out->size();
        co_await readFramesUntil(
            client, &buffer, out, [before](auto const& got) { return got.size() > before; });
    }
    client->close();
}

} // namespace

TEST_CASE("a control connection never views", "[vthost][native][control]")
{
    auto h = NativeHarness { controlApiOptions() };
    auto const got = h.exchange({ controlApiHello(), controlCall(1, "workspace.list") }, 2);
    REQUIRE(got.size() == 2);
    REQUIRE(std::holds_alternative<proto::ServerHello>(got[0].pdu));
    CHECK(isControlReply(got[1]));
    CHECK_FALSE(std::ranges::any_of(got, viewsAnything));
    CHECK(h.host.sessionCount() == 0);
    CHECK(h.host.connectedClients() == 0);
    CHECK(h.session->phase() == NativeSession::ConnectionPhase::Controlling);
}

TEST_CASE("a ControlRequest is answered with its reply", "[vthost][native][control]")
{
    auto h = NativeHarness { controlApiOptions() };
    std::ignore = newSession(h.host);
    auto const got = h.exchange(
        { controlApiHello(), controlCall(41, "workspace.list"), controlCall(42, "pane.teleport") }, 3);
    REQUIRE(got.size() == 3);

    REQUIRE(isControlReply(got[1]));
    auto const& list = std::get<proto::ControlReply>(got[1].pdu);
    CHECK(got[1].serial == 2);
    CHECK(list.requestId == 41);
    CHECK(list.outcome == proto::ControlOutcome::Ok);
    auto const decoded = vtcontrol::decodeJson<vtcontrol::ListReply>(list.body);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->workspaces.size() == 1);
    CHECK(decoded->workspaces[0].tabs.size() == 1);

    REQUIRE(isControlReply(got[2]));
    auto const& unknown = std::get<proto::ControlReply>(got[2].pdu);
    CHECK(unknown.requestId == 42);
    CHECK(unknown.outcome == proto::ControlOutcome::Error);
    auto const error = vtcontrol::decodeJson<vtcontrol::ControlError>(unknown.body);
    REQUIRE(error.has_value());
    CHECK(error->code == vtcontrol::ControlErrorCode::MethodNotFound);
}

TEST_CASE("without the ControlApi capability a ControlRequest is Unsupported", "[vthost][native][control]")
{
    auto options = controlApiOptions();
    options.services.handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::Heartbeat });
    auto h = NativeHarness { options };
    auto const got =
        h.exchange({ controlApiHello(), controlCall(1, "daemon.info"), controlCall(2, "daemon.info") }, 3);
    REQUIRE(got.size() == 3);
    for (auto const index: { 1U, 2U })
    {
        REQUIRE(std::holds_alternative<proto::Unsupported>(got[index].pdu));
        auto const& refusal = std::get<proto::Unsupported>(got[index].pdu);
        CHECK(refusal.ident == std::to_underlying(proto::PduType::ControlRequest));
        CHECK(got[index].serial == index + 1);
    }

    SECTION("nor is it served to an Attach connection")
    {
        auto viewer = NativeHarness { controlApiOptions() };
        auto const answered = viewer.exchangeUntil(
            { proto::ClientHello { .capabilities =
                                       proto::CapabilitySet::of({ proto::Capability::ControlApi }) },
              controlCall(1, "daemon.info") },
            [](auto const& frames) {
                return std::ranges::any_of(
                    frames, [](auto const& f) { return std::holds_alternative<proto::Unsupported>(f.pdu); });
            });
        CHECK_FALSE(std::ranges::any_of(answered, isControlReply));
    }
}

TEST_CASE("the default services serve no control API", "[vthost][native][control]")
{
    // ControlApi advertised, but NativeSessionServices' null control API behind it.
    auto options = HarnessOptions {};
    options.services.handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi });
    auto h = NativeHarness { options };
    auto const got = h.exchange({ controlApiHello(), controlCall(7, "workspace.list") }, 2);
    REQUIRE(got.size() == 2);
    REQUIRE(isControlReply(got[1]));
    auto const& reply = std::get<proto::ControlReply>(got[1].pdu);
    CHECK(reply.outcome == proto::ControlOutcome::Error);
    auto const error = vtcontrol::decodeJson<vtcontrol::ControlError>(reply.body);
    REQUIRE(error.has_value());
    CHECK(error->code == vtcontrol::ControlErrorCode::Unsupported);
}

TEST_CASE("phase 2's status verb still works on a control connection", "[vthost][native][control]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    auto options = controlApiOptions();
    options.services.daemon = daemon;
    auto h = NativeHarness { options };
    auto const got =
        h.exchange({ controlApiHello(), proto::DaemonStatusRequest {}, controlCall(1, "daemon.info") }, 3);
    REQUIRE(got.size() == 3);
    CHECK(std::holds_alternative<proto::DaemonStatusReply>(got[1].pdu));
    REQUIRE(isControlReply(got[2]));
    CHECK(std::get<proto::ControlReply>(got[2].pdu).outcome == proto::ControlOutcome::Ok);
}

TEST_CASE("viewers attaching and detaching are events for control subscribers", "[vthost][native][control]")
{
    auto h = NativeHarness { controlApiOptions() };
    auto attachPair = makeConnection(h.loop, std::nullopt);
    auto attach = h.makeSession(attachPair, vthost::ConnectionId { .endpoint = "attach", .index = 2 });

    auto const controlBytes = encodeRequest(
        { controlApiHello(),
          controlCall(1, "events.subscribe", R"({"kinds":["client.attached","client.detached"]})") });
    auto const attachBytes = encodeRequest({ proto::ClientHello {} });
    auto controlReceived = std::vector<proto::DecodedFrame> {};
    auto attachReceived = std::vector<proto::DecodedFrame> {};

    h.loop.blockOn(core::net::testing::allOf(
        h.session->run(),
        feedBytes(h.pair.second.get(), &controlBytes),
        runAndRelease(&attach),
        feedOnce(&h.loop,
                 attachPair.second.get(),
                 &attachBytes,
                 &controlReceived,
                 [](auto const& got) { return std::ranges::any_of(got, isControlReply); }),
        collectUntil(
            &h.loop,
            attachPair.second.get(),
            [](auto const& got) { return std::ranges::any_of(got, endsSnapshotRun); },
            &attachReceived),
        collectUntil(
            &h.loop,
            h.pair.second.get(),
            [](auto const& got) { return countEvents<vtcontrol::ClientDetachedEvent>(got) >= 1; },
            &controlReceived)));

    auto const events = controlEvents(controlReceived);
    REQUIRE(events.size() == 2);
    REQUIRE(std::holds_alternative<vtcontrol::ClientAttachedEvent>(events[0].payload));
    REQUIRE(std::holds_alternative<vtcontrol::ClientDetachedEvent>(events[1].payload));
    CHECK(std::get<vtcontrol::ClientAttachedEvent>(events[0].payload).client == "attach#2");
    CHECK(std::get<vtcontrol::ClientDetachedEvent>(events[1].payload).client == "attach#2");
    CHECK(h.host.connectedClients() == 0);
}

TEST_CASE("an event flood drops events instead of the connection", "[vthost][native][control]")
{
    // Review focus 3: a loop ringing the bell while the subscriber does not read.
    auto options = controlApiOptions();
    options.writeQueueBytes = SmallQueueBytes;
    options.pipeBytes = std::size_t { 4096 };
    auto h = NativeHarness { options };
    auto const session = newSession(h.host);
    auto const bytes = encodeRequest(
        { controlApiHello(), controlCall(1, "events.subscribe", R"({"kinds":["pane.bell"]})") });
    auto received = std::vector<proto::DecodedFrame> {};

    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             floodedSubscriber(&h, session, 10'000, &received)));

    auto const events = controlEvents(received);
    auto const dropped = std::ranges::find_if(events, [](auto const& e) {
        return std::holds_alternative<vtcontrol::EventsDroppedEvent>(e.payload);
    });
    REQUIRE(dropped != events.end());
    CHECK(std::get<vtcontrol::EventsDroppedEvent>(dropped->payload).count > 0);
    CHECK(std::any_of(std::next(dropped), events.end(), [](auto const& e) {
        return std::holds_alternative<vtcontrol::BellEvent>(e.payload);
    }));
    // Far fewer than were rung reached the wire: the backlog stayed bounded.
    CHECK(countEvents<vtcontrol::BellEvent>(received) < 5'000);
    CHECK(h.host.sessionCount() == 1);
}

TEST_CASE("a capture bigger than the reply bound arrives truncated over the wire",
          "[vthost][native][control]")
{
    // Review focus 2.
    auto options = controlApiOptions();
    options.maxReplyBytes = 1024;
    options.history = vtbackend::LineCount(200);
    auto h = NativeHarness { options };
    auto const session = newSession(h.host);
    auto text = std::string {};
    while (text.size() < 5000)
    {
        // GCC's link-time inlining reports a -Wstringop-overflow for the natural form of this (a string
        // temporary joined with `+`) that cannot happen, so it is built in pieces.
        text.append(70, 'x');
        text += "\r\n";
    }
    h.host.terminal(session)->writeToScreen(text + "LAST");

    auto const got =
        h.exchange({ controlApiHello(), controlCall(1, "pane.capture", R"({"scrollback":-1})") }, 2);
    REQUIRE(got.size() == 2);
    REQUIRE(isControlReply(got[1]));
    auto const& reply = std::get<proto::ControlReply>(got[1].pdu);
    REQUIRE(reply.outcome == proto::ControlOutcome::Ok);
    auto const capture = vtcontrol::decodeJson<vtcontrol::CaptureReply>(reply.body);
    REQUIRE(capture.has_value());
    CHECK(capture->truncated == vtcontrol::Truncation::Head);
    CHECK(capture->text.size() <= 1024);
    CHECK(capture->text.ends_with("LAST"));
}

namespace
{

/// @return A control harness whose handshake accepts "ro" for reading and the screen, "arranger" for that and
///         changing the workspace, and "admin" for all.
HarnessOptions narrowTokenOptions()
{
    auto options = controlApiOptions();
    options.services.handshake.acceptedTokens = {
        { .token = "ro",
          .grants =
              vtcontrol::ControlGrants::of({ vtcontrol::Permission::Read, vtcontrol::Permission::Screen }) },
        { .token = "arranger",
          .grants = vtcontrol::ControlGrants::of(
              { vtcontrol::Permission::Read, vtcontrol::Permission::Screen, vtcontrol::Permission::Write }) },
        { .token = "admin", .grants = vtcontrol::ControlGrants::all() },
    };
    return options;
}

/// @return The control error of a ControlReply frame.
[[nodiscard]] vtcontrol::ControlError controlErrorOf(proto::DecodedFrame const& frame)
{
    auto const& reply = std::get<proto::ControlReply>(frame.pdu);
    REQUIRE(reply.outcome == proto::ControlOutcome::Error);
    auto error = vtcontrol::decodeJson<vtcontrol::ControlError>(reply.body);
    REQUIRE(error.has_value());
    return *error;
}

} // namespace

TEST_CASE("a narrow token can read but not type", "[vthost][native][grants]")
{
    // Review focus 6.
    auto daemon = vthost::testing::FakeDaemonControl {};
    auto options = narrowTokenOptions();
    options.services.daemon = daemon;
    auto h = NativeHarness { options };
    std::ignore = newSession(h.host);
    auto hello = controlApiHello();
    hello.token = "ro";
    auto const got = h.exchange({ hello,
                                  controlCall(1, "pane.capture"),
                                  controlCall(2, "pane.send", R"({"text":"rm -rf ~\n"})"),
                                  controlCall(3, "workspace.list"),
                                  proto::StopDaemon {},
                                  proto::DaemonStatusRequest {} },
                                6);
    REQUIRE(got.size() == 6);
    REQUIRE(std::holds_alternative<proto::ServerHello>(got[0].pdu));
    CHECK(h.session->grants()
          == vtcontrol::ControlGrants::of({ vtcontrol::Permission::Read, vtcontrol::Permission::Screen }));

    REQUIRE(isControlReply(got[1]));
    CHECK(std::get<proto::ControlReply>(got[1].pdu).outcome == proto::ControlOutcome::Ok);

    REQUIRE(isControlReply(got[2]));
    auto const forbidden = controlErrorOf(got[2]);
    CHECK(forbidden.code == vtcontrol::ControlErrorCode::Forbidden);
    CHECK(forbidden.message.contains("'input'"));

    // The connection stays open, and a stop needs `write`.
    REQUIRE(isControlReply(got[3]));
    CHECK(std::get<proto::ControlReply>(got[3].pdu).outcome == proto::ControlOutcome::Ok);
    REQUIRE(std::holds_alternative<proto::Unsupported>(got[4].pdu));
    CHECK(std::get<proto::Unsupported>(got[4].pdu).ident == std::to_underlying(proto::PduType::StopDaemon));
    CHECK(daemon.stops.empty());
    CHECK(std::holds_alternative<proto::DaemonStatusReply>(got[5].pdu));
}

TEST_CASE("a write token can rearrange but not run a command", "[vthost][native][grants]")
{
    auto h = NativeHarness { narrowTokenOptions() };
    std::ignore = newSession(h.host);
    auto hello = controlApiHello();
    hello.token = "arranger";
    auto const got = h.exchange({ hello,
                                  controlCall(1, "tab.create"),
                                  controlCall(2, "tab.create", R"({"command":["sh","-c","id"]})"),
                                  controlCall(3, "tab.create", R"({"placement":"new","command":["sh"]})"),
                                  controlCall(4, "pane.split", R"({"command":["sh","-c","id"]})"),
                                  controlCall(5, "pane.split", R"({"cwd":"/tmp"})") },
                                6);
    REQUIRE(got.size() == 6);
    REQUIRE(isControlReply(got[1]));
    CHECK(std::get<proto::ControlReply>(got[1].pdu).outcome == proto::ControlOutcome::Ok);
    for (auto const index: { 2, 3, 4 })
    {
        INFO("reply " << index);
        REQUIRE(isControlReply(got[static_cast<std::size_t>(index)]));
        auto const forbidden = controlErrorOf(got[static_cast<std::size_t>(index)]);
        CHECK(forbidden.code == vtcontrol::ControlErrorCode::Forbidden);
        CHECK(forbidden.message.contains("running a command needs grant 'input'"));
    }
    REQUIRE(isControlReply(got[5]));
    CHECK(std::get<proto::ControlReply>(got[5].pdu).outcome == proto::ControlOutcome::Ok);
    // The first session, the shell tab.create opened and the split's: nothing the refused calls named.
    CHECK(h.host.sessionCount() == 3);
}

TEST_CASE("a connection past its subscription share is refused and stays open",
          "[vthost][native][control][limits]")
{
    auto h = NativeHarness { controlApiOptions() };
    auto const share = vthost::control::DefaultMaxSubscriptionsPerConnection;
    auto pdus = std::vector<proto::DecodedPdu> { controlApiHello() };
    for (auto const id: std::views::iota(uint64_t { 1 }, uint64_t { share + 2 }))
        pdus.emplace_back(controlCall(id, "events.subscribe"));
    pdus.emplace_back(controlCall(share + 2, "events.unsubscribe", R"({"subscription":1})"));
    pdus.emplace_back(controlCall(share + 3, "events.subscribe"));
    auto const got = h.exchange(pdus, pdus.size());
    REQUIRE(got.size() == pdus.size());
    for (auto const index: std::views::iota(std::size_t { 1 }, share + 1))
    {
        REQUIRE(isControlReply(got[index]));
        CHECK(std::get<proto::ControlReply>(got[index].pdu).outcome == proto::ControlOutcome::Ok);
    }
    REQUIRE(isControlReply(got[share + 1]));
    CHECK(controlErrorOf(got[share + 1]).code == vtcontrol::ControlErrorCode::LimitReached);
    // Ending one frees a place.
    REQUIRE(isControlReply(got[share + 2]));
    CHECK(std::get<proto::ControlReply>(got[share + 2].pdu).outcome == proto::ControlOutcome::Ok);
    REQUIRE(isControlReply(got[share + 3]));
    CHECK(std::get<proto::ControlReply>(got[share + 3].pdu).outcome == proto::ControlOutcome::Ok);
}

TEST_CASE("an attach with a narrow token is refused like a wrong token", "[vthost][native][grants]")
{
    auto const answerTo = [](std::string token) {
        auto h = NativeHarness { narrowTokenOptions() };
        auto const got = h.exchange({ proto::ClientHello { .token = std::move(token) } }, 1);
        CHECK(h.host.sessionCount() == 0);
        CHECK(h.host.connectedClients() == 0);
        CHECK(h.session->phase() == NativeSession::ConnectionPhase::AwaitingHello);
        REQUIRE(got.size() == 1);
        auto bytes = proto::Writer {};
        proto::encodePdu(bytes, got[0].serial, got[0].pdu);
        return std::vector<std::byte> { bytes.view().begin(), bytes.view().end() };
    };
    auto const narrow = answerTo("ro");
    CHECK(narrow == answerTo("wrong"));

    // The full token attaches.
    auto h = NativeHarness { narrowTokenOptions() };
    std::ignore = h.exchange({ proto::ClientHello { .token = "admin" } }, 1);
    CHECK(h.session->phase() == NativeSession::ConnectionPhase::Viewing);
    CHECK(h.session->grants() == vtcontrol::ControlGrants::all());
}

TEST_CASE("an AF_UNIX connection holds every grant", "[vthost][native][grants]")
{
    auto h = NativeHarness { controlApiOptions() };
    REQUIRE(h.session->grants() == vtcontrol::ControlGrants::none());
    std::ignore = h.exchange({ controlApiHello(), controlCall(1, "workspace.list") }, 2);
    CHECK(h.session->grants() == vtcontrol::ControlGrants::all());
}

TEST_CASE("an echo change with no output after it reaches a viewer by the idle check",
          "[vthost][native][flush][inputecho]")
{
    // sudo may turn echo off after it printed its prompt: nothing prints, nothing is typed, and only the
    // idle check (FlushPolicy::inputEchoCheck) can tell the viewer. On a ManualClock, so no wait is real.
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto const interval = f.h.options.services.flush.inputEchoCheck;
    auto& client = f.attach(id, inputEchoHello());
    REQUIRE(inputEchoFrames(client.received).empty());

    mockPtyOf(f.h, id).setInputEcho(vtpty::InputEcho::Hidden);
    // Not before the interval: the check is coarse, not a poll of every flush.
    f.clock.advance(interval - 1ms);
    turnIdle(f.h.loop);
    CHECK(inputEchoFrames(client.received).empty());

    f.clock.advance(1ms);
    REQUIRE(turnUntil(f.h.loop, [&] { return inputEchoFrames(client.received).size() == 1; }));
    CHECK(inputEchoFrames(client.received)[0].second.session == id.value);
    CHECK(inputEchoFrames(client.received)[0].second.echo == std::to_underlying(vtpty::InputEcho::Hidden));

    // Unchanged across many intervals: nothing more is sent, and the check keeps itself armed.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        f.clock.advance(interval);
        turnIdle(f.h.loop);
    }
    CHECK(inputEchoFrames(client.received).size() == 1);
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session).has_value());

    // Back on, again with no output: told at the next check.
    mockPtyOf(f.h, id).setInputEcho(vtpty::InputEcho::Echoed);
    f.clock.advance(interval);
    REQUIRE(turnUntil(f.h.loop, [&] { return inputEchoFrames(client.received).size() == 2; }));
    CHECK(inputEchoFrames(client.received)[1].second.echo == std::to_underlying(vtpty::InputEcho::Echoed));
}

TEST_CASE("the idle check of the echo state runs only within the activity window",
          "[vthost][native][flush][inputecho]")
{
    // An idle connection wakes nothing: the check keeps itself armed for FlushPolicy::inputEchoWindow after
    // the attach, an output or an input, then stays off until the next of them. On a ManualClock.
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto const& flush = f.h.options.services.flush;
    auto& client = f.attach(id, inputEchoHello());
    REQUIRE(NativeSessionFlushTester::armedDeadline(*client.session).has_value());

    // Quiet past the window: the check that finds it over arms nothing.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 12))
    {
        f.clock.advance(flush.inputEchoCheck);
        turnIdle(f.h.loop);
    }
    CHECK_FALSE(NativeSessionFlushTester::armedDeadline(*client.session).has_value());

    // An echo change with no activity is not told ...
    mockPtyOf(f.h, id).setInputEcho(vtpty::InputEcho::Hidden);
    f.clock.advance(flush.inputEchoCheck * 4);
    turnIdle(f.h.loop);
    CHECK(inputEchoFrames(client.received).empty());
    CHECK_FALSE(NativeSessionFlushTester::armedDeadline(*client.session).has_value());

    // ... until output opens the workspace: the echo rides the output's flush, and the check is armed again.
    f.update(client, id, "Password:");
    REQUIRE(turnUntil(f.h.loop, [&] { return inputEchoFrames(client.received).size() == 1; }));
    CHECK(NativeSessionFlushTester::armedDeadline(*client.session).has_value());
}

TEST_CASE("a closed session's echo bookkeeping is dropped with the rest of its per-session state",
          "[vthost][native][flush][inputecho]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attach(id, inputEchoHello());
    mockPtyOf(f.h, id).setInputEcho(vtpty::InputEcho::Hidden);
    f.update(client, id, "Password:");
    REQUIRE(turnUntil(f.h.loop, [&] { return inputEchoFrames(client.received).size() == 1; }));
    REQUIRE(NativeSessionFlushTester::tracksInputEcho(*client.session, id));

    client.session->sessionClosed(id);
    CHECK_FALSE(NativeSessionFlushTester::tracksInputEcho(*client.session, id));
}

TEST_CASE("a viewer that does not mirror the echo state arms no idle check",
          "[vthost][native][flush][inputecho]")
{
    auto f = FlushHarness {};
    auto const id = newSession(f.h.host);
    auto& client = f.attach(id);
    mockPtyOf(f.h, id).setInputEcho(vtpty::InputEcho::Hidden);
    f.clock.advance(f.h.options.services.flush.inputEchoCheck * 4);
    turnIdle(f.h.loop);
    CHECK_FALSE(NativeSessionFlushTester::armedDeadline(*client.session).has_value());
    CHECK(inputEchoFrames(client.received).empty());
}

namespace
{

/// A hello offering MoveNode, as a client that can drag panes between workspaces does.
[[nodiscard]] proto::ClientHello movingHello()
{
    return proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::MoveNode }) };
}

/// @param workspace The workspace the window selects.
/// @return A hello offering MoveNode that selects @p workspace.
[[nodiscard]] proto::ClientHello movingHelloSelecting(uint64_t workspace)
{
    return proto::ClientHello {
        .capabilities =
            proto::CapabilitySet::of({ proto::Capability::MoveNode, proto::Capability::AttachIntent }),
        .attachIntent = { .kind = proto::AttachIntentKind::Select, .workspace = workspace }
    };
}

/// @return A MoveNode taking the one-pane tab of @p dragged to the tab well of the workspace hosting @p onto.
[[nodiscard]] proto::MoveNode intoTabWell(vtworkspace::SessionId dragged, vtworkspace::SessionId onto)
{
    return proto::MoveNode { .sourceKind = std::to_underlying(vtworkspace::MoveSourceKind::Pane),
                             .sourceSessions = { dragged.value },
                             .targetKind = std::to_underlying(vtworkspace::MoveTargetKind::TabWell),
                             .targetSession = onto.value,
                             .index = 9 };
}

/// @return Whether @p frames holds a LayoutState of @p workspace naming @p session.
[[nodiscard]] bool layoutShows(std::vector<proto::DecodedFrame> const& frames,
                               uint64_t workspace,
                               vtworkspace::SessionId session)
{
    auto const* layout = lastLayoutOf(frames, workspace);
    if (layout == nullptr)
        return false;
    auto const names = [&](this auto const& self, proto::WirePane const& node) -> bool {
        return node.session == session.value || std::ranges::any_of(node.children, self);
    };
    return std::ranges::any_of(layout->tabs, [&](auto const& tab) { return names(tab.root); });
}

} // namespace

TEST_CASE("a MoveNode moves a pane into another workspace, its session keeps running and its output keeps "
          "arriving",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* dragged = h.host.createTab();
    auto const* other = h.host.createWorkspace();
    REQUIRE((dragged != nullptr && other != nullptr));
    auto const draggedSession = dragged->rootPane()->session();
    auto const otherWorkspace = other->id();
    auto const otherSession = other->tabAt(0)->rootPane()->session();

    auto const arrived = [&](auto const& got) {
        return layoutShows(got, otherWorkspace.value, draggedSession)
               && std::ranges::any_of(increments(got), [&](auto const* delta) {
                      return delta->session == draggedSession.value;
                  });
    };
    // Written once the move's layout has reached the client: the session is the same one, in its new
    // workspace.
    auto const received = h.exchangeUntil(
        { movingHelloSelecting(otherWorkspace.value), intoTabWell(draggedSession, otherSession) },
        arrived,
        writeOnce(&h, draggedSession, "after the move", [&] {
            return layoutShows(h.received, otherWorkspace.value, draggedSession);
        }));

    CHECK(arrived(received));
    auto const [workspace, tab, leaf] = h.host.model().findSessionLeaf(draggedSession);
    REQUIRE(workspace != nullptr);
    CHECK(workspace->id() == otherWorkspace);
    // Moved, not re-created.
    CHECK(h.host.terminal(draggedSession) != nullptr);
    CHECK_FALSE(std::ranges::any_of(
        received, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); }));
}

TEST_CASE("two clients: a MoveNode from one pushes the new layout to both", "[vthost][native][move][daemon]")
{
    auto h = TwoClientHarness {};
    auto const* dragged = h.host.createTab();
    auto const* other = h.host.createWorkspace();
    REQUIRE((dragged != nullptr && other != nullptr));
    auto const draggedSession = dragged->rootPane()->session();
    auto const otherWorkspace = other->id().value;
    auto const otherSession = other->tabAt(0)->rootPane()->session();

    auto const hello = encodeRequest({ vthost::testing::showingAll(h.host) });
    auto const moving = encodeRequest(
        { vthost::testing::showingAll(h.host, movingHello()), intoTabWell(draggedSession, otherSession) });
    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};
    auto const moved = [&](std::vector<proto::DecodedFrame> const& got) {
        return layoutShows(got, otherWorkspace, draggedSession);
    };
    // The mover speaks once the watcher holds its attach snapshot, so the watcher sees the change as a push.
    h.loop.blockOn(
        core::net::testing::allOf(h.serverOne->run(),
                                  h.serverTwo->run(),
                                  feedBytes(h.secondPair.second.get(), &hello),
                                  feedOnce(&h.loop,
                                           h.firstPair.second.get(),
                                           &moving,
                                           &fromTwo,
                                           [](auto const& got) { return snapshotRunCount(got) >= 1; }),
                                  collectUntil(&h.loop, h.firstPair.second.get(), moved, &fromOne),
                                  collectUntil(&h.loop, h.secondPair.second.get(), moved, &fromTwo)));

    CHECK(moved(fromOne));
    CHECK(moved(fromTwo));
}

TEST_CASE("two clients: a pane moved into the other client's workspace is mirrored there without new output",
          "[vthost][native][move][daemon]")
{
    auto h = TwoClientHarness {};
    auto const* dragged = h.host.createTab();
    REQUIRE(dragged != nullptr);
    auto const draggedSession = dragged->rootPane()->session();
    auto const draggedWorkspace = h.host.model().workspaceOfTab(dragged->id()).value;
    std::ignore = h.host.createTab({}, draggedSession); // the dragged pane's workspace outlives the move
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const otherWorkspace = other->id().value;
    auto const otherSession = other->tabAt(0)->rootPane()->session();

    // Each client shows one workspace by name, as each window's own link does.
    auto const showing = [](uint64_t workspace, std::initializer_list<proto::Capability> capabilities) {
        return proto::ClientHello {
            .capabilities = proto::CapabilitySet::of(capabilities),
            .attachIntent =
                proto::AttachIntent { .kind = proto::AttachIntentKind::Select, .workspace = workspace },
        };
    };
    auto const helloOne = encodeRequest({ showing(
        draggedWorkspace,
        { proto::Capability::MoveNode, proto::Capability::LayoutBatch, proto::Capability::AttachIntent }) });
    auto const helloTwo = encodeRequest(
        { showing(otherWorkspace, { proto::Capability::LayoutBatch, proto::Capability::AttachIntent }) });
    auto const moving = encodeRequest({ intoTabWell(draggedSession, otherSession) });
    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};
    auto const landed = [](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(got, [](proto::DecodedFrame const& frame) {
            auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu);
            return end != nullptr && end->causeSerial != 0;
        });
    };
    // Nothing is written on the moved session: what reaches the second client is owed by the move alone.
    auto const mirrored = [&](std::vector<proto::DecodedFrame> const& got) {
        return layoutShows(got, otherWorkspace, draggedSession)
               && std::ranges::any_of(got, [&](proto::DecodedFrame const& frame) {
                      auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
                      return delta != nullptr && delta->session == draggedSession.value
                             && endsSnapshotRun(frame);
                  });
    };
    auto mirroredBefore = false;
    // The mover asks once both clients hold their attach snapshots.
    h.loop.blockOn(core::net::testing::allOf(
        h.serverOne->run(),
        h.serverTwo->run(),
        feedBytes(h.firstPair.second.get(), &helloOne),
        feedBytes(h.secondPair.second.get(), &helloTwo),
        feedOnce(&h.loop,
                 h.firstPair.second.get(),
                 &moving,
                 &fromOne,
                 [&](auto const& got) {
                     auto const ready = snapshotRunCount(got) >= 2 && snapshotRunCount(fromTwo) >= 1;
                     if (ready)
                         mirroredBefore = NativeSessionFlushTester::mirrors(*h.serverOne, draggedSession);
                     return ready;
                 }),
        collectUntil(&h.loop, h.firstPair.second.get(), landed, &fromOne),
        collectUntil(&h.loop, h.secondPair.second.get(), mirrored, &fromTwo)));

    CHECK(mirrored(fromTwo));
    CHECK(landed(fromOne));
    // The mover's connection shows it no longer: one that comes back is mirrored anew.
    CHECK(mirroredBefore);
    CHECK_FALSE(NativeSessionFlushTester::mirrors(*h.serverOne, draggedSession));
    CHECK(h.host.terminal(draggedSession) != nullptr);
}

namespace
{
/// @return Whether a layout among @p frames for @p workspace shows a tab whose root split lies along @p axis.
[[nodiscard]] bool layoutSplitsAlong(std::vector<proto::DecodedFrame> const& frames,
                                     uint64_t workspace,
                                     vtworkspace::SplitState axis)
{
    return std::ranges::any_of(frames, [&](proto::DecodedFrame const& frame) {
        auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu);
        return layout != nullptr && layout->workspace == workspace
               && std::ranges::any_of(layout->tabs, [&](proto::WireTab const& tab) {
                      return tab.root.split == std::to_underlying(axis);
                  });
    });
}
} // namespace

TEST_CASE("two clients: a SetSplitOrientation from one pushes the new axis to both",
          "[vthost][native][move][daemon]")
{
    auto h = TwoClientHarness {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
    auto const* root = h.host.model().findTab(tab->id())->rootPane();
    auto const anchor = root->first()->session();
    auto const workspace = h.host.model().workspaceOfTab(tab->id()).value;

    // Both show the tab's workspace by name: a plain launch of a second client is given one of its own.
    auto const showing =
        proto::AttachIntent { .kind = proto::AttachIntentKind::Select, .workspace = workspace };
    auto const capabilities =
        proto::CapabilitySet::of({ proto::Capability::MoveNode, proto::Capability::AttachIntent });
    auto movingTheWorkspace = movingHello();
    movingTheWorkspace.capabilities = capabilities;
    movingTheWorkspace.attachIntent = showing;
    auto const hello =
        encodeRequest({ proto::ClientHello { .capabilities = capabilities, .attachIntent = showing } });
    auto const setting = encodeRequest(
        { movingTheWorkspace,
          proto::SetSplitOrientation { .anchorSession = anchor.value,
                                       .state = std::to_underlying(vtworkspace::SplitState::Horizontal) } });
    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};
    auto const flipped = [&](std::vector<proto::DecodedFrame> const& got) {
        return layoutSplitsAlong(got, workspace, vtworkspace::SplitState::Horizontal);
    };
    h.loop.blockOn(
        core::net::testing::allOf(h.serverOne->run(),
                                  h.serverTwo->run(),
                                  feedBytes(h.secondPair.second.get(), &hello),
                                  feedOnce(&h.loop,
                                           h.firstPair.second.get(),
                                           &setting,
                                           &fromTwo,
                                           [](auto const& got) { return snapshotRunCount(got) >= 1; }),
                                  collectUntil(&h.loop, h.firstPair.second.get(), flipped, &fromOne),
                                  collectUntil(&h.loop, h.secondPair.second.get(), flipped, &fromTwo)));

    CHECK(flipped(fromOne));
    CHECK(flipped(fromTwo));
    CHECK(h.host.model().findTab(tab->id())->rootPane()->splitState() == vtworkspace::SplitState::Horizontal);
}

TEST_CASE("a SetSplitOrientation ends a layout batch, and a stale session or a state no axis names changes "
          "nothing",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
    auto const anchor = h.host.model().findTab(tab->id())->rootPane()->first()->session();
    auto const workspace = h.host.model().workspaceOfTab(tab->id()).value;
    auto const hello =
        proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                 { proto::Capability::MoveNode, proto::Capability::LayoutBatch }) };

    // The two no-ops come first; the valid one after them proves they were read.
    auto const received = h.exchangeUntil(
        { hello,
          proto::SetSplitOrientation { .anchorSession = 987654,
                                       .state = std::to_underlying(vtworkspace::SplitState::Horizontal) },
          proto::SetSplitOrientation { .anchorSession = anchor.value, .state = 0xEE },
          proto::SetSplitOrientation { .anchorSession = anchor.value,
                                       .state = std::to_underlying(vtworkspace::SplitState::Horizontal) } },
        [&](auto const& got) {
            return layoutSplitsAlong(got, workspace, vtworkspace::SplitState::Horizontal)
                   && std::ranges::any_of(got, [](auto const& frame) {
                          return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu);
                      });
        });

    CHECK(layoutSplitsAlong(received, workspace, vtworkspace::SplitState::Horizontal));
    CHECK(std::ranges::any_of(received, [](auto const& frame) {
        return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu);
    }));
    CHECK(h.host.model().findTab(tab->id())->rootPane()->splitState() == vtworkspace::SplitState::Horizontal);
}

TEST_CASE("a SetSplitOrientation without the move-node capability is refused",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
    auto const anchor = h.host.model().findTab(tab->id())->rootPane()->first()->session();

    auto const received = h.exchangeUntil(
        { proto::ClientHello {},
          proto::SetSplitOrientation { .anchorSession = anchor.value,
                                       .state = std::to_underlying(vtworkspace::SplitState::Horizontal) } },
        [](auto const& got) {
            return std::ranges::any_of(
                got, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
        });

    auto const refused = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
    REQUIRE(refused != received.end());
    CHECK(std::get<proto::Unsupported>(refused->pdu).ident
          == std::to_underlying(proto::PduType::SetSplitOrientation));
    CHECK(h.host.model().findTab(tab->id())->rootPane()->splitState() == vtworkspace::SplitState::Vertical);
}

TEST_CASE("a MoveNode onto the only sibling's edge pushes the tab re-split along that edge",
          "[vthost][native][move][daemon]")
{
    // A | B, B dropped on A's bottom edge: A above B. The leaf order is the same; only the axis changed.
    auto h = NativeHarness {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const tabId = tab->id();
    h.host.splitActivePane(tabId, vtworkspace::SplitState::Vertical, 0.5);
    auto const* root = h.host.model().findTab(tabId)->rootPane();
    auto const left = root->first()->session();
    auto const right = root->second()->session();
    auto const workspace = h.host.model().workspaceOfTab(tabId).value;

    auto const stacked = [&](auto const& got) {
        auto const* layout = lastLayoutOf(got, workspace);
        return layout != nullptr && std::ranges::any_of(layout->tabs, [&](proto::WireTab const& wireTab) {
                   auto const& node = wireTab.root;
                   return node.split == std::to_underlying(vtworkspace::SplitState::Horizontal)
                          && node.children.size() == 2 && node.children[0].session == left.value
                          && node.children[1].session == right.value;
               });
    };
    auto const received = h.exchangeUntil(
        { movingHello(),
          proto::MoveNode { .sourceKind = std::to_underlying(vtworkspace::MoveSourceKind::Pane),
                            .sourceSessions = { right.value },
                            .targetKind = std::to_underlying(vtworkspace::MoveTargetKind::Edge),
                            .targetSession = left.value,
                            .side = std::to_underlying(vtworkspace::SplitSide::Bottom) } },
        stacked);

    CHECK(stacked(received));
    auto const* after = h.host.model().findTab(tabId);
    REQUIRE(after != nullptr);
    CHECK(after->rootPane()->splitState() == vtworkspace::SplitState::Horizontal);
    CHECK(after->activePane()->session() == right);
}

TEST_CASE("a MoveNode without the move-node capability is refused", "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* dragged = h.host.createTab();
    auto const* other = h.host.createWorkspace();
    REQUIRE((dragged != nullptr && other != nullptr));
    auto const draggedSession = dragged->rootPane()->session();
    auto const before = h.host.model().findSessionLeaf(draggedSession);

    // An older client's hello offers nothing, and a daemon of today still refuses what it did not negotiate.
    auto const received = h.exchangeUntil(
        { proto::ClientHello {}, intoTabWell(draggedSession, other->tabAt(0)->rootPane()->session()) },
        [](auto const& got) {
            return std::ranges::any_of(
                got, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
        });

    auto const refused = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::Unsupported>(frame.pdu); });
    REQUIRE(refused != received.end());
    CHECK(std::get<proto::Unsupported>(refused->pdu).ident == std::to_underlying(proto::PduType::MoveNode));
    CHECK(h.host.model().findSessionLeaf(draggedSession) == before);
}

TEST_CASE("a refused MoveNode answers MoveNodeRefused under its serial and changes nothing",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* dragged = h.host.createTab();
    auto const* other = h.host.createWorkspace();
    REQUIRE((dragged != nullptr && other != nullptr));
    auto const draggedSession = dragged->rootPane()->session();
    auto const before = h.host.model().findSessionLeaf(draggedSession);

    auto const isRefusal = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::MoveNodeRefused>(frame.pdu);
    };
    // The request names the source as a tab holding a session it does not: a stale view of it.
    auto move = intoTabWell(draggedSession, other->tabAt(0)->rootPane()->session());
    move.sourceKind = std::to_underlying(vtworkspace::MoveSourceKind::Tab);
    move.sourceSessions.push_back(9999);
    auto const received = h.exchangeUntil(
        { movingHello(), move }, [&](auto const& got) { return std::ranges::any_of(got, isRefusal); });

    auto const refusal = std::ranges::find_if(received, isRefusal);
    REQUIRE(refusal != received.end());
    CHECK(refusal->serial == 2); // the move's: the second frame the client sent
    CHECK(std::get<proto::MoveNodeRefused>(refusal->pdu).reason
          == std::to_underlying(vthost::NodeMoveError::StaleTab));
    CHECK(h.host.model().findSessionLeaf(draggedSession) == before);
}

TEST_CASE("an old daemon: the client sees no move-node capability", "[vthost][native][move][daemon]")
{
    auto options = HarnessOptions {};
    // A daemon of the release before MoveNode: it offers everything but.
    options.services.handshake.capabilities = proto::CapabilitySet::of(
        { proto::Capability::TabEdits, proto::Capability::TabMerge, proto::Capability::Heartbeat });
    auto h = NativeHarness { std::move(options) };

    auto const received =
        h.exchangeUntil({ movingHello() }, [](auto const& got) { return serverHelloOf(got) != nullptr; });

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK_FALSE(hello->capabilities.contains(proto::Capability::MoveNode));
}

TEST_CASE("a pane moved to a new workspace is laid out to every client", "[vthost][native][move][daemon]")
{
    auto h = TwoClientHarness {};
    auto const* dragged = h.host.createTab();
    // Beside another tab: a workspace of its own for a workspace's whole content is refused.
    auto const* staying = h.host.createTab();
    REQUIRE((dragged != nullptr && staying != nullptr));
    auto const first = h.host.workspaceId().value;
    auto const draggedSession = dragged->rootPane()->session();

    // The one that moves also offers MoveNode; the daemon lays the new workspace out to both.
    auto const watcherHello = encodeRequest({ intentHello(proto::AttachIntentKind::Select) });
    auto moverHello = intentHello(proto::AttachIntentKind::Select);
    moverHello.capabilities =
        proto::CapabilitySet::of({ proto::Capability::AttachIntent, proto::Capability::MoveNode });
    auto const moving = encodeRequest(
        { moverHello,
          proto::MoveNode { .sourceKind = std::to_underlying(vtworkspace::MoveSourceKind::Pane),
                            .sourceSessions = { draggedSession.value },
                            .targetKind = std::to_underlying(vtworkspace::MoveTargetKind::NewWorkspace) } });

    auto fromWatcher = std::vector<proto::DecodedFrame> {};
    auto fromMover = std::vector<proto::DecodedFrame> {};
    auto const opensNewWorkspace = [first](std::vector<proto::DecodedFrame> const& got) {
        return std::ranges::any_of(layoutWorkspaces(got),
                                   [first](uint64_t workspace) { return workspace != first; });
    };
    h.loop.blockOn(core::net::testing::allOf(
        h.serverOne->run(),
        h.serverTwo->run(),
        feedBytes(h.firstPair.second.get(), &watcherHello),
        feedOnce(&h.loop,
                 h.secondPair.second.get(),
                 &moving,
                 &fromWatcher,
                 [](auto const& got) { return serverHelloOf(got) != nullptr; }),
        probeOnce(&h.loop, h.firstPair.second.get(), [&] { return opensNewWorkspace(fromMover); }),
        collectUntil(&h.loop, h.firstPair.second.get(), imageGoneArrived, &fromWatcher),
        collectUntil(&h.loop, h.secondPair.second.get(), opensNewWorkspace, &fromMover)));

    CHECK(opensNewWorkspace(fromMover));
    CHECK(opensNewWorkspace(fromWatcher));
}

namespace
{
/// @return Whether @p frames last told of @p workspace as holding no tab.
[[nodiscard]] bool pushedEmpty(std::vector<proto::DecodedFrame> const& frames, uint64_t workspace)
{
    auto const* layout = lastLayoutOf(frames, workspace);
    return layout != nullptr && layout->tabs.empty();
}
} // namespace

// A workspace a move takes the last tab out of is not left behind: every client is pushed it empty, which is
// what closes the client's own workspace for it, and then it is gone -- from the model, and with its name and
// claim from what the control API lists.
TEST_CASE("a workspace a MoveNode empties is pushed empty, then removed", "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* staying = h.host.createTab();
    auto const* emptied = h.host.createWorkspace();
    REQUIRE((staying != nullptr && emptied != nullptr));
    auto const primary = h.host.workspaceId();
    auto const emptiedWorkspace = emptied->id();
    auto const draggedSession = emptied->tabAt(0)->rootPane()->session();
    auto const stayingSession = staying->rootPane()->session();
    REQUIRE(h.host.objects().uuidOf(emptiedWorkspace).has_value());

    auto const moved = [&](auto const& got) {
        return layoutShows(got, primary.value, draggedSession) && pushedEmpty(got, emptiedWorkspace.value);
    };
    auto const received =
        h.exchangeUntil({ movingHello(), intoTabWell(draggedSession, stayingSession) }, moved);

    CHECK(moved(received));
    CHECK(h.host.model().workspace(emptiedWorkspace) == nullptr);
    CHECK(h.host.model().workspaceCount() == 1);
    CHECK_FALSE(h.host.objects().uuidOf(emptiedWorkspace).has_value());
    CHECK(h.host.terminal(draggedSession) != nullptr);
    CHECK(h.host.workspaceId() == primary);
}

// The host's own workspace is where a creation naming none lands: emptied by a move and removed, another
// takes that role, so the next tab still has a workspace to open in.
TEST_CASE("the host's own workspace a MoveNode empties is removed, and another workspace takes its role",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* dragged = h.host.createTab();
    auto const* other = h.host.createWorkspace();
    REQUIRE((dragged != nullptr && other != nullptr));
    auto const primary = h.host.workspaceId();
    auto const otherWorkspace = other->id();
    auto const draggedSession = dragged->rootPane()->session();

    auto const moved = [&](auto const& got) {
        return layoutShows(got, otherWorkspace.value, draggedSession) && pushedEmpty(got, primary.value);
    };
    auto const received = h.exchangeUntil(
        { movingHello(), intoTabWell(draggedSession, other->tabAt(0)->rootPane()->session()) }, moved);

    CHECK(moved(received));
    CHECK(h.host.model().workspace(primary) == nullptr);
    CHECK(h.host.workspaceId() == otherWorkspace);
    auto const* const created = h.host.createTab();
    REQUIRE(created != nullptr);
    CHECK(h.host.model().workspaceOfTab(created->id()) == otherWorkspace);
}

// A client of the release before MoveNode never moves anything, but sees the moves another client makes: it
// is pushed the emptied workspace empty, as it was pushed any workspace a closed pane emptied, and closes it.
TEST_CASE("a client that cannot move is pushed the workspace another client's move empties as empty",
          "[vthost][native][move][daemon][compat]")
{
    auto h = TwoClientHarness {};
    auto const* staying = h.host.createTab();
    auto const* emptied = h.host.createWorkspace();
    REQUIRE((staying != nullptr && emptied != nullptr));
    auto const primary = h.host.workspaceId().value;
    auto const emptiedWorkspace = emptied->id();
    auto const draggedSession = emptied->tabAt(0)->rootPane()->session();

    auto const hello = encodeRequest({ vthost::testing::showingAll(h.host) });
    auto const moving = encodeRequest({ vthost::testing::showingAll(h.host, movingHello()),
                                        intoTabWell(draggedSession, staying->rootPane()->session()) });
    auto fromMover = std::vector<proto::DecodedFrame> {};
    auto fromWatcher = std::vector<proto::DecodedFrame> {};
    auto const moved = [&](std::vector<proto::DecodedFrame> const& got) {
        return layoutShows(got, primary, draggedSession) && pushedEmpty(got, emptiedWorkspace.value);
    };
    h.loop.blockOn(
        core::net::testing::allOf(h.serverOne->run(),
                                  h.serverTwo->run(),
                                  feedBytes(h.secondPair.second.get(), &hello),
                                  feedOnce(&h.loop,
                                           h.firstPair.second.get(),
                                           &moving,
                                           &fromWatcher,
                                           [](auto const& got) { return snapshotRunCount(got) >= 1; }),
                                  collectUntil(&h.loop, h.firstPair.second.get(), moved, &fromMover),
                                  collectUntil(&h.loop, h.secondPair.second.get(), moved, &fromWatcher)));

    CHECK(moved(fromMover));
    CHECK(moved(fromWatcher));
    CHECK(h.host.model().workspace(emptiedWorkspace) == nullptr);
}

namespace
{

/// The layouts a client holds as each LayoutBatchEnd among @p frames arrives, by workspace, one entry per
/// batch.
[[nodiscard]] std::vector<std::map<uint64_t, proto::LayoutState>> layoutsAtBatchEnds(
    std::vector<proto::DecodedFrame> const& frames)
{
    auto held = std::map<uint64_t, proto::LayoutState> {};
    auto batches = std::vector<std::map<uint64_t, proto::LayoutState>> {};
    for (auto const& frame: frames)
    {
        if (auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu))
            held.insert_or_assign(layout->workspace, *layout);
        else if (std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu))
            batches.push_back(held);
    }
    return batches;
}

/// @return How many of the workspaces in @p layouts name @p session.
[[nodiscard]] std::size_t workspacesNaming(std::map<uint64_t, proto::LayoutState> const& layouts,
                                           vtworkspace::SessionId session)
{
    auto const names = [&](this auto const& self, proto::WirePane const& node) -> bool {
        return node.session == session.value || std::ranges::any_of(node.children, self);
    };
    return static_cast<std::size_t>(
        std::ranges::count_if(layouts | std::views::values, [&](auto const& layout) {
            return std::ranges::any_of(layout.tabs, [&](auto const& tab) { return names(tab.root); });
        }));
}

/// The move a layout batch exists for: one pane of a two-pane tab into another workspace's tab well, which
/// the model announces half done -- the pane detached, in no workspace -- before it announces it inserted.
struct PaneIntoOtherWorkspace
{
    NativeHarness h;
    vtworkspace::SessionId dragged;
    uint64_t otherWorkspace = 0;
    vtworkspace::SessionId otherSession;

    explicit PaneIntoOtherWorkspace(HarnessOptions options = {}): h(std::move(options))
    {
        auto const* tab = h.host.createTab();
        REQUIRE(tab != nullptr);
        h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        dragged = h.host.model().findTab(tab->id())->activePane()->session();
        auto const* other = h.host.createWorkspace();
        REQUIRE(other != nullptr);
        otherWorkspace = other->id().value;
        otherSession = other->tabAt(0)->rootPane()->session();
    }

    /// Attaches with @p hello, moves the pane, and collects until output written after the move arrived.
    [[nodiscard]] std::vector<proto::DecodedFrame> run(proto::ClientHello offered)
    {
        // The window selects the workspace the pane moves into, so the pane arrives in what it mirrors.
        auto hello = std::move(offered);
        hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::AttachIntent));
        hello.attachIntent = { .kind = proto::AttachIntentKind::Select, .workspace = otherWorkspace };
        auto const arrived = [&](auto const& got) {
            return layoutShows(got, otherWorkspace, dragged)
                   && std::ranges::any_of(increments(got),
                                          [&](auto const* delta) { return delta->session == dragged.value; });
        };
        // Written a turn after the move's layouts reached the client, so whatever the move ends with is in.
        return h.exchangeUntil({ hello, intoTabWell(dragged, otherSession) },
                               arrived,
                               writeOnce(&h, dragged, "after the move", [&] {
                                   return layoutShows(h.received, otherWorkspace, dragged);
                               }));
    }
};

} // namespace

TEST_CASE("a client sharing layout-batch is told each push's end, and a moved pane is in one workspace at "
          "every end",
          "[vthost][native][move][daemon]")
{
    auto f = PaneIntoOtherWorkspace {};
    auto const received =
        f.run(proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                       { proto::Capability::MoveNode, proto::Capability::LayoutBatch }) });

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK(hello->capabilities.contains(proto::Capability::LayoutBatch));
    auto const batches = layoutsAtBatchEnds(received);
    // However many model events the move fired, and however many pushes they made, no batch ends between
    // them: the attach's and the move's may share one, read in one turn.
    REQUIRE(!batches.empty());
    for (auto const& layouts: batches)
        CHECK(workspacesNaming(layouts, f.dragged) == 1);
    auto const& last = batches.back();
    REQUIRE(last.contains(f.otherWorkspace));
    CHECK(workspacesNaming({ { f.otherWorkspace, last.at(f.otherWorkspace) } }, f.dragged) == 1);
    // Nothing of a push trails its end.
    auto const lastLayout = std::ranges::find_last_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutState>(frame.pdu); });
    auto const lastEnd = std::ranges::find_last_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu); });
    REQUIRE(!lastLayout.empty());
    REQUIRE(!lastEnd.empty());
    CHECK(lastLayout.begin() < lastEnd.begin());
}

TEST_CASE("the batch a MoveNode's change ends names that request's serial", "[vthost][native][move][daemon]")
{
    auto f = PaneIntoOtherWorkspace {};
    auto const received =
        f.run(proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                       { proto::Capability::MoveNode, proto::Capability::LayoutBatch }) });

    // The MoveNode is the request's second PDU, so it went out under serial 2.
    auto const lastEnd = std::ranges::find_last_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu); });
    REQUIRE(!lastEnd.empty());
    CHECK(std::get<proto::LayoutBatchEnd>(lastEnd.front().pdu).causeSerial == 2);
}

TEST_CASE("a push no request caused ends with cause 0", "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    std::ignore = h.host.createTab();
    auto const ended = [](auto const& got) {
        return std::ranges::any_of(
            got, [](auto const& frame) { return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu); });
    };
    auto const received = h.exchangeUntil({ proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                                                     { proto::Capability::LayoutBatch }) } },
                                          ended);

    auto const end = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu); });
    REQUIRE(end != received.end());
    CHECK(std::get<proto::LayoutBatchEnd>(end->pdu).causeSerial == 0);
}

TEST_CASE("a client without layout-batch is sent no LayoutBatchEnd", "[vthost][native][move][daemon][compat]")
{
    auto f = PaneIntoOtherWorkspace {};
    auto const received = f.run(
        proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::MoveNode }) });

    CHECK(layoutShows(received, f.otherWorkspace, f.dragged));
    CHECK_FALSE(std::ranges::any_of(received, [](auto const& frame) {
        return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu);
    }));
}

namespace
{

/// @param more A capability offered beside them, if any.
/// @return A hello offering MoveNode and LayoutBatch, as a client that drags panes between workspaces does.
[[nodiscard]] proto::ClientHello batchingHello(std::optional<proto::Capability> more = std::nullopt)
{
    auto capabilities =
        proto::CapabilitySet::of({ proto::Capability::MoveNode, proto::Capability::LayoutBatch });
    if (more)
        capabilities.idents.push_back(std::to_underlying(*more));
    return proto::ClientHello { .capabilities = capabilities };
}

/// @return @p pdu encoded as a frame under @p serial: a request sent after the attach's.
[[nodiscard]] std::vector<std::byte> encodedAt(uint64_t serial, proto::DecodedPdu const& pdu)
{
    auto request = proto::Writer {};
    proto::encodePdu(request, serial, pdu);
    return { request.view().begin(), request.view().end() };
}

/// @return Whether @p frames holds a LayoutBatchEnd naming @p cause.
[[nodiscard]] bool batchEndedBy(std::vector<proto::DecodedFrame> const& frames, uint64_t cause)
{
    return std::ranges::any_of(frames, [cause](auto const& frame) {
        auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu);
        return end != nullptr && end->causeSerial == cause;
    });
}

} // namespace

// One move fires several model events, and a client that ends its pushes reads only the state at the end:
// each workspace the move changed is pushed once, as the move left it, and the batch is ended once.
TEST_CASE(
    "a move pushes each workspace it changed once, then ends its batch, to a client sharing layout-batch",
    "[vthost][native][move][daemon]")
{
    auto f = PaneIntoOtherWorkspace {};
    auto const sourceWorkspace =
        f.h.host.model().workspaceOfTab(std::get<1>(f.h.host.model().findSessionLeaf(f.dragged))->id()).value;
    // The window selects the workspace the pane moves into, so the pane arrives in what it mirrors.
    auto selecting = batchingHello(proto::Capability::AttachIntent);
    selecting.attachIntent = { .kind = proto::AttachIntentKind::Select, .workspace = f.otherWorkspace };
    auto const hello = encodeRequest({ selecting });
    auto const move = encodedAt(2, intoTabWell(f.dragged, f.otherSession));
    auto const attachEnded = [](std::vector<proto::DecodedFrame> const& got) {
        return batchEndedBy(got, 0);
    };
    auto const arrived = [&](auto const& got) {
        return batchEndedBy(got, 2) && std::ranges::any_of(increments(got), [&](auto const* delta) {
                   return delta->session == f.dragged.value;
               });
    };
    // The move is sent once the attach's batch has ended, and output written once the move's has: whatever
    // the move pushed is in by the time that output arrives.
    auto const received = f.h.exchangeBytesUntil(
        hello,
        arrived,
        feedOnce(&f.h.loop, f.h.pair.second.get(), &move, &f.h.received, attachEnded),
        writeOnce(&f.h, f.dragged, "after the move", [&] { return batchEndedBy(f.h.received, 2); }));

    REQUIRE(arrived(received));
    auto const attachEnd = std::ranges::find_if(
        received, [](auto const& frame) { return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu); });
    REQUIRE(attachEnd != received.end());
    auto const afterAttach = std::ranges::subrange(std::next(attachEnd), received.end());
    auto const layoutsOf = [&](uint64_t workspace) {
        return std::ranges::count_if(afterAttach, [workspace](auto const& frame) {
            auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu);
            return layout != nullptr && layout->workspace == workspace;
        });
    };
    auto const ends = std::ranges::count_if(afterAttach, [](auto const& frame) {
        return std::holds_alternative<proto::LayoutBatchEnd>(frame.pdu);
    });
    CHECK(layoutsOf(sourceWorkspace) == 1);
    CHECK(layoutsOf(f.otherWorkspace) == 1);
    CHECK(ends == 1);
    CHECK(layoutShows(received, f.otherWorkspace, f.dragged));
}

// The client that asked for a move is told it landed: a batch naming the request ends, though the workspace
// the window selects is none of those the move changed.
TEST_CASE("a move ends its batch for the client that asked, whatever workspace it selects",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    std::ignore = h.host.createTab();
    auto const* from = h.host.createWorkspace();
    auto const* into = h.host.createWorkspace();
    REQUIRE((from != nullptr && into != nullptr));
    auto const intoWorkspace = into->id().value;
    auto const dragged = from->tabAt(0)->rootPane()->session();
    auto const onto = into->tabAt(0)->rootPane()->session();

    auto const received =
        h.exchangeUntil({ batchingHello(proto::Capability::AttachIntent), intoTabWell(dragged, onto) },
                        [](auto const& got) { return batchEndedBy(got, 2); });

    CHECK(batchEndedBy(received, 2));
    CHECK(h.host.model().workspaceOfTab(std::get<1>(h.host.model().findSessionLeaf(dragged))->id()).value
          == intoWorkspace);
}

TEST_CASE("a MoveNode the model refuses answers with the model's reason too",
          "[vthost][native][move][daemon]")
{
    auto h = NativeHarness {};
    auto const* dragged = h.host.createTab();
    REQUIRE(dragged != nullptr);
    auto const session = dragged->rootPane()->session();
    auto const isRefusal = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::MoveNodeRefused>(frame.pdu);
    };
    // A pane swapped with itself.
    auto const received = h.exchangeUntil(
        { movingHello(),
          proto::MoveNode { .sourceKind = std::to_underlying(vtworkspace::MoveSourceKind::Pane),
                            .sourceSessions = { session.value },
                            .targetKind = std::to_underlying(vtworkspace::MoveTargetKind::Swap),
                            .targetSession = session.value } },
        [&](auto const& got) { return std::ranges::any_of(got, isRefusal); });

    auto const refusal = std::ranges::find_if(received, isRefusal);
    REQUIRE(refusal != received.end());
    auto const& refused = std::get<proto::MoveNodeRefused>(refusal->pdu);
    CHECK(refused.reason == std::to_underlying(vthost::NodeMoveError::Model));
    CHECK(refused.modelReason == std::to_underlying(vtworkspace::MoveError::SelfTarget));
}

namespace
{

/// @return Options whose daemon offers Capability::LiveUpgrade, on the local "control" endpoint.
[[nodiscard]] HarnessOptions upgradeOptions(vthost::testing::FakeDaemonControl& daemon)
{
    auto options = HarnessOptions { .services = { .daemon = daemon }, .endpoint = "control" };
    options.services.handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::LiveUpgrade });
    return options;
}

/// @param role What the connection is for.
/// @return A hello offering Capability::LiveUpgrade.
[[nodiscard]] proto::ClientHello upgradeHello(proto::ConnectionRole role = proto::ConnectionRole::Control)
{
    return proto::ClientHello { .capabilities = proto::CapabilitySet::of({ proto::Capability::LiveUpgrade }),
                                .role = role };
}

/// @return Whether @p frame is an UpgradeDaemonReply.
[[nodiscard]] bool isUpgradeReply(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<proto::UpgradeDaemonReply>(frame.pdu);
}

/// @return Whether @p got holds a DaemonUpgrading.
[[nodiscard]] bool upgradeAnnounced(std::vector<proto::DecodedFrame> const& got)
{
    return std::ranges::any_of(
        got, [](auto const& frame) { return std::holds_alternative<proto::DaemonUpgrading>(frame.pdu); });
}

} // namespace

TEST_CASE("UpgradeDaemon over TCP is refused", "[vthost][native][upgrade]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    daemon.upgradeOutcome = {};
    auto options = upgradeOptions(daemon);
    options.endpoint = "native-tcp";
    auto h = NativeHarness { options };
    auto const got = h.exchange({ upgradeHello(), proto::UpgradeDaemon { .imageId = "x" } }, 2);
    REQUIRE(got.size() == 2);
    REQUIRE(isUpgradeReply(got[1]));
    CHECK(std::get<proto::UpgradeDaemonReply>(got[1].pdu)
          == proto::UpgradeDaemonReply {
              .result =
                  static_cast<uint8_t>(std::to_underlying(vthost::handover::UpgradeRefusal::LocalOnly) + 1),
              .detail = "live upgrade is local only" });
    CHECK(daemon.upgrades.empty());
}

TEST_CASE("daemon.upgrade answers started ahead of the announcement, then the connection closes",
          "[vthost][native][upgrade][control]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    daemon.upgradeOutcome = {};
    auto options = controlApiOptions();
    options.services.daemon = daemon;
    options.endpoint = "control";
    // Announced to the requester too, which negotiated it.
    options.services.handshake.capabilities =
        proto::CapabilitySet::of({ proto::Capability::ControlApi, proto::Capability::LiveUpgrade });
    auto h = NativeHarness { options };
    auto hello = controlApiHello();
    hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::LiveUpgrade));
    auto const got = h.exchangeUntil({ hello, controlCall(7, "daemon.upgrade", R"({"image":"0.9.0-a"})") },
                                     upgradeAnnounced);
    auto const reply = std::ranges::find_if(got, isControlReply);
    auto const announced = std::ranges::find_if(
        got, [](auto const& frame) { return std::holds_alternative<proto::DaemonUpgrading>(frame.pdu); });
    REQUIRE(reply != got.end());
    REQUIRE(announced != got.end());
    CHECK(reply < announced);
    auto const& body = std::get<proto::ControlReply>(reply->pdu);
    CHECK(body.requestId == 7);
    CHECK(body.outcome == proto::ControlOutcome::Ok);
    auto const decoded = vtcontrol::decodeJson<vtcontrol::DaemonUpgradeReply>(body.body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->started);
    CHECK(decoded->toRelease == "9.9.10");
    CHECK(daemon.upgrades == std::vector<std::string> { "0.9.0-a" });
}

TEST_CASE("daemon.upgrade reports a refusal as a coded error", "[vthost][native][upgrade][control]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    daemon.upgradeOutcome =
        std::unexpected(vthost::handover::UpgradeError { vthost::handover::UpgradeRefusal::Sandboxed });
    auto options = controlApiOptions();
    options.services.daemon = daemon;
    options.endpoint = "control";
    auto h = NativeHarness { options };
    auto const got = h.exchange({ controlApiHello(), controlCall(8, "daemon.upgrade") }, 2);
    REQUIRE(got.size() == 2);
    auto const error = controlErrorOf(got[1]);
    CHECK(error.code == vtcontrol::ControlErrorCode::Failed);
    CHECK(error.message.starts_with("sandboxed: "));
    CHECK(error.message.contains(vthost::handover::describe(vthost::handover::UpgradeRefusal::Sandboxed)));
}

TEST_CASE("daemon.upgrade over TCP is refused local only, and needs the write grant",
          "[vthost][native][upgrade][control]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    daemon.upgradeOutcome = {};
    SECTION("over TCP")
    {
        auto options = controlApiOptions();
        options.services.daemon = daemon;
        options.endpoint = "native-tcp";
        auto h = NativeHarness { options };
        auto const got = h.exchange({ controlApiHello(), controlCall(9, "daemon.upgrade") }, 2);
        REQUIRE(got.size() == 2);
        auto const error = controlErrorOf(got[1]);
        CHECK(error.message.starts_with("localOnly: "));
        CHECK(daemon.upgrades.empty());
    }
    SECTION("without write")
    {
        auto options = narrowTokenOptions();
        options.services.daemon = daemon;
        options.endpoint = "control";
        auto h = NativeHarness { options };
        auto hello = controlApiHello();
        hello.token = "ro";
        auto const got = h.exchange({ hello, controlCall(9, "daemon.upgrade") }, 2);
        REQUIRE(got.size() == 2);
        CHECK(controlErrorOf(got[1]).code == vtcontrol::ControlErrorCode::Forbidden);
        CHECK(daemon.upgrades.empty());
    }
}

TEST_CASE("UpgradeDaemon needs the capability and the write grant", "[vthost][native][upgrade]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    daemon.upgradeOutcome = {};
    SECTION("a peer that did not negotiate it is answered Unsupported")
    {
        auto h = NativeHarness { upgradeOptions(daemon) };
        auto const got = h.exchange(
            { proto::ClientHello { .role = proto::ConnectionRole::Control }, proto::UpgradeDaemon {} }, 2);
        REQUIRE(got.size() == 2);
        CHECK(std::get<proto::Unsupported>(got[1].pdu).ident
              == std::to_underlying(proto::PduType::UpgradeDaemon));
        CHECK(daemon.upgrades.empty());
    }
    SECTION("a token without write is answered Unsupported")
    {
        auto options = narrowTokenOptions();
        options.services.daemon = daemon;
        options.services.handshake.capabilities =
            proto::CapabilitySet::of({ proto::Capability::LiveUpgrade, proto::Capability::ControlApi });
        options.endpoint = "control";
        auto h = NativeHarness { options };
        auto hello = upgradeHello();
        hello.token = "ro";
        hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::ControlApi));
        auto const got = h.exchange({ hello, proto::UpgradeDaemon {} }, 2);
        REQUIRE(got.size() == 2);
        CHECK(std::get<proto::Unsupported>(got[1].pdu).ident
              == std::to_underlying(proto::PduType::UpgradeDaemon));
        CHECK(daemon.upgrades.empty());
    }
}

TEST_CASE("a refused UpgradeDaemon is answered with the refusal", "[vthost][native][upgrade]")
{
    using vthost::handover::ExecFailure;
    using vthost::handover::UpgradeError;
    using vthost::handover::UpgradeRefusal;
    auto daemon = vthost::testing::FakeDaemonControl {};
    SECTION("a second request during an upgrade is refused with InProgress")
    {
        daemon.upgradeOutcome = std::unexpected(UpgradeError { UpgradeRefusal::InProgress });
        auto h = NativeHarness { upgradeOptions(daemon) };
        auto const got = h.exchange({ upgradeHello(), proto::UpgradeDaemon { .imageId = "0.9.0-a" } }, 2);
        REQUIRE(got.size() == 2);
        REQUIRE(isUpgradeReply(got[1]));
        CHECK(std::get<proto::UpgradeDaemonReply>(got[1].pdu).result == 1);
        CHECK(vthost::upgradeRefusalFrom(std::get<proto::UpgradeDaemonReply>(got[1].pdu).result)
              == UpgradeRefusal::InProgress);
        CHECK(daemon.upgrades == std::vector<std::string> { "0.9.0-a" });
    }
    SECTION("an exec failure is told apart from a refusal")
    {
        daemon.upgradeOutcome = std::unexpected(UpgradeError {
            ExecFailure { .cause = std::make_error_code(std::errc::no_such_file_or_directory) } });
        auto h = NativeHarness { upgradeOptions(daemon) };
        auto const got = h.exchange({ upgradeHello(), proto::UpgradeDaemon {} }, 2);
        REQUIRE(got.size() == 2);
        REQUIRE(isUpgradeReply(got[1]));
        CHECK(std::get<proto::UpgradeDaemonReply>(got[1].pdu).result
              == proto::UpgradeDaemonReply::ExecFailedResult);
        CHECK_FALSE(std::get<proto::UpgradeDaemonReply>(got[1].pdu).detail.empty());
    }
    SECTION("an upgrade that failed by an exception is answered as an exec failure")
    {
        daemon.upgradeThrows = std::make_exception_ptr(std::bad_alloc {});
        auto h = NativeHarness { upgradeOptions(daemon) };
        auto const got = h.exchange({ upgradeHello(), proto::UpgradeDaemon {} }, 2);
        REQUIRE(got.size() == 2);
        REQUIRE(isUpgradeReply(got[1]));
        CHECK(std::get<proto::UpgradeDaemonReply>(got[1].pdu)
              == proto::UpgradeDaemonReply {
                  .result = proto::UpgradeDaemonReply::ExecFailedResult,
                  .detail = std::make_error_code(std::errc::not_enough_memory).message() });
    }
    SECTION("an upgrade that failed by a non-std exception is answered as an exec failure too")
    {
        daemon.upgradeThrows = std::make_exception_ptr(42);
        auto h = NativeHarness { upgradeOptions(daemon) };
        auto const got = h.exchange({ upgradeHello(), proto::UpgradeDaemon {} }, 2);
        REQUIRE(got.size() == 2);
        REQUIRE(isUpgradeReply(got[1]));
        CHECK(std::get<proto::UpgradeDaemonReply>(got[1].pdu).result
              == proto::UpgradeDaemonReply::ExecFailedResult);
    }
}

namespace
{
/// Once the hello is answered: closes every upgrade observer's connection at once.
Task<void> abortOnceHello(NativeHarness* h, vthost::testing::FakeDaemonControl* daemon)
{
    co_await awaitGate(&h->loop, holdsFrames(&h->received, 1));
    daemon->abortObservers();
}
} // namespace

TEST_CASE("an upgrade's abort ends a connection at once", "[vthost][native][upgrade]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    auto h = NativeHarness { upgradeOptions(daemon) };
    // Collects until the connection ends; run() ends with it, or this never returns.
    auto const got = h.exchangeUntil(
        { upgradeHello(proto::ConnectionRole::Attach) },
        [](auto const&) { return false; },
        abortOnceHello(&h, &daemon));
    REQUIRE_FALSE(got.empty());
    CHECK(std::holds_alternative<proto::ServerHello>(got.front().pdu));
    // run() ended: it no longer observes the daemon's upgrades.
    CHECK(daemon.observers.empty());
}

TEST_CASE("every client receives DaemonUpgrading before its connection closes", "[vthost][native][upgrade]")
{
    auto daemon = vthost::testing::FakeDaemonControl {};
    daemon.upgradeOutcome = {};
    SECTION("the requester is answered first, then told, then closed")
    {
        auto h = NativeHarness { upgradeOptions(daemon) };
        // run() ends with the connection: the exchange returning is the close.
        auto const got = h.exchangeUntil({ upgradeHello(), proto::UpgradeDaemon { .imageId = "0.9.0-a" } },
                                         upgradeAnnounced);
        REQUIRE(got.size() == 3);
        CHECK(std::holds_alternative<proto::ServerHello>(got[0].pdu));
        REQUIRE(isUpgradeReply(got[1]));
        CHECK(std::get<proto::UpgradeDaemonReply>(got[1].pdu).result == 0);
        CHECK(got[1].serial == 2);
        REQUIRE(std::holds_alternative<proto::DaemonUpgrading>(got[2].pdu));
        CHECK(std::get<proto::DaemonUpgrading>(got[2].pdu).toRelease == "9.9.10");
        CHECK(h.session->phase() == NativeSession::ConnectionPhase::Controlling);
    }
    SECTION("an attached client is told as well, and its connection ends")
    {
        auto h = NativeHarness { upgradeOptions(daemon) };
        auto const got = h.exchangeUntil(
            { upgradeHello(proto::ConnectionRole::Attach), proto::UpgradeDaemon {} }, upgradeAnnounced);
        CHECK(upgradeAnnounced(got));
        // run() ended with the connection: the session is the host's client no longer once it is let go.
        h.session.reset();
        CHECK(h.host.connectedClients() == 0);
    }
}

TEST_CASE("a daemon no longer offers panes-reprojected", "[vthost][native][size][compat]")
{
    // It restores the survivors' pane sizes itself when a client departs, so it never asks a client for them.
    auto h = NativeHarness {};
    auto const received = h.exchange({ proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                                                { proto::Capability::PanesReprojected }) } },
                                     AttachSnapshotFrames);

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK_FALSE(hello->capabilities.contains(proto::Capability::PanesReprojected));
}

TEST_CASE("a clipboard write reaches a client only where it can land it", "[vthost][native][clipboard]")
{
    using vtbackend::ClipboardTarget;
    using vtbackend::ClipboardTargets;
    using vthost::forwardedClipboardTargets;
    auto const clipboard = ClipboardTargets { ClipboardTarget::Clipboard };
    auto const primary = ClipboardTargets { ClipboardTarget::PrimarySelection };
    auto const both = ClipboardTargets { ClipboardTarget::Clipboard, ClipboardTarget::PrimarySelection };

    SECTION("a client sharing the capability gets every target")
    {
        auto const shared = proto::CapabilitySet::of({ proto::Capability::ClipboardTargets });
        CHECK(forwardedClipboardTargets(both, shared) == both);
        CHECK(forwardedClipboardTargets(primary, shared) == primary);
    }

    SECTION("a client predating it, which puts everything on the clipboard")
    {
        auto const shared = proto::CapabilitySet {};
        CHECK(forwardedClipboardTargets(clipboard, shared) == clipboard);
        CHECK(forwardedClipboardTargets(both, shared) == clipboard); // narrowed, so its `selection` reads "c"
        CHECK_FALSE(forwardedClipboardTargets(primary, shared));     // never moved onto its user's clipboard
    }
}

namespace
{

/// The directory each session a harness spawns starts in, in spawn order: empty for one spawned without
/// a directory of its own.
using SpawnedDirectories = std::shared_ptr<std::vector<std::filesystem::path>>;

/// @param spawned Receives the directory of every session the factory spawns.
/// @return Harness options whose PTY factory records the directory each spawn asks for.
[[nodiscard]] HarnessOptions recordingSpawnDirectories(SpawnedDirectories const& spawned)
{
    auto options = HarnessOptions {};
    options.ptyFactory =
        [spawned](
            vtbackend::PageSize size,
            std::optional<vtpty::Process::ExecInfo> const& commandOverride) -> std::unique_ptr<vtpty::Pty> {
        spawned->push_back(commandOverride ? commandOverride->workingDirectory : std::filesystem::path {});
        return std::make_unique<vtpty::MockPty>(size);
    };
    return options;
}

/// @return An absolute directory on every platform; a test makes the harness's file system hold it, as the
///         host starts no shell in a directory it does not find.
[[nodiscard]] std::filesystem::path statedDirectory()
{
    return std::filesystem::temp_directory_path() / "contour-attach-cwd";
}

/// @param kind What the client asks to follow.
/// @param directory The working directory the hello states, as it travels.
/// @param workspaces The workspaces named, for AttachIntentKind::Select and Resume.
/// @return A hello stating that intent and that directory.
[[nodiscard]] proto::ClientHello directoryHello(proto::AttachIntentKind kind,
                                                std::string directory,
                                                std::vector<uint64_t> workspaces = {})
{
    auto hello = intentHello(kind, std::move(workspaces));
    hello.workingDirectory = std::move(directory);
    return hello;
}

} // namespace

TEST_CASE("the first session of an attach starts in the directory its hello states",
          "[vthost][native][attach-intent][cwd]")
{
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };
    h.files.addDirectory(statedDirectory());

    std::ignore = h.exchangeUntil(
        { directoryHello(proto::AttachIntentKind::Default, core::pathToUtf8(statedDirectory())) },
        [](auto const& got) { return snapshotRunCount(got) >= 1; });

    CHECK(*spawned == std::vector { statedDirectory() });
}

TEST_CASE("a hello's relative working directory is ignored", "[vthost][native][attach-intent][cwd]")
{
    // A relative path names a place under the client's own directory, which the daemon cannot see.
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };

    std::ignore = h.exchangeUntil({ directoryHello(proto::AttachIntentKind::Default, "relative/dir") },
                                  [](auto const& got) { return snapshotRunCount(got) >= 1; });

    CHECK(*spawned == std::vector { std::filesystem::path {} });
}

TEST_CASE("a launch stating a directory opens a tab in it beside the workspaces the daemon kept",
          "[vthost][native][attach-intent][cwd]")
{
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };
    h.files.addDirectory(statedDirectory());
    REQUIRE(h.host.createTab() != nullptr);
    auto const workspace = h.host.workspaceId().value;
    spawned->clear();

    auto const received = h.exchangeUntil(
        { directoryHello(proto::AttachIntentKind::Default, core::pathToUtf8(statedDirectory())) },
        [](auto const& got) { return snapshotRunCount(got) >= 2; });

    CHECK(*spawned == std::vector { statedDirectory() });
    CHECK(sessionsOf(h.host, workspace).size() == 2);
    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK(hello->selectedWorkspace == workspace);
}

TEST_CASE("a launch stating no directory adds nothing to the workspaces the daemon kept",
          "[vthost][native][attach-intent][cwd]")
{
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };
    REQUIRE(h.host.createTab() != nullptr);
    auto const workspace = h.host.workspaceId().value;
    spawned->clear();

    std::ignore = h.exchangeUntil({ intentHello(proto::AttachIntentKind::Default) },
                                  [](auto const& got) { return snapshotRunCount(got) >= 1; });

    CHECK(spawned->empty());
    CHECK(sessionsOf(h.host, workspace).size() == 1);
}

TEST_CASE("an attach to named workspaces stating a directory adds nothing to them",
          "[vthost][native][attach-intent][cwd]")
{
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };
    REQUIRE(h.host.createTab() != nullptr);
    auto const workspace = h.host.workspaceId().value;
    spawned->clear();

    std::ignore = h.exchangeUntil({ directoryHello(proto::AttachIntentKind::Select,
                                                   core::pathToUtf8(statedDirectory()),
                                                   { workspace }) },
                                  [](auto const& got) { return snapshotRunCount(got) >= 1; });

    CHECK(spawned->empty());
    CHECK(sessionsOf(h.host, workspace).size() == 1);
}

// {{{ A tab or split opens in the directory of the pane it was made from
namespace
{

/// Where the shell the tests start reports itself to be; the harness's file system holds it.
constexpr auto WorkDirectory = std::string_view { "/tmp/work" };

/// A harness whose first tab's shell reports a directory the host finds on disk, and the recording of the
/// directories the sessions spawned after it ask for.
struct ReportingHarness
{
    /// @param directory The directory the first tab's shell is in.
    explicit ReportingHarness(std::filesystem::path const& directory):
        harness { recordingSpawnDirectories(spawned) }
    {
        harness.files.addDirectory(directory);
        auto* tab = harness.host.createTab();
        REQUIRE(tab != nullptr);
        session = tab->rootPane()->session();
        vthost::testing::reportDirectory(*harness.host.terminal(session), directory);
        spawned->clear();
    }

    std::shared_ptr<std::vector<std::filesystem::path>> spawned =
        std::make_shared<std::vector<std::filesystem::path>>();
    NativeHarness harness;
    vtworkspace::SessionId session {};
};

} // namespace

TEST_CASE("a CreateTab beside a session starts in that session's working directory", "[vthost][native][cwd]")
{
    auto h = ReportingHarness { WorkDirectory };

    std::ignore =
        h.harness.exchange({ proto::DecodedPdu { proto::ClientHello {} },
                             proto::DecodedPdu { proto::CreateTab { .session = h.session.value } } },
                           4);

    REQUIRE_FALSE(h.spawned->empty());
    CHECK(h.spawned->back() == std::filesystem::path { WorkDirectory });
}

TEST_CASE("a SplitPane starts in the split pane's working directory", "[vthost][native][cwd]")
{
    auto h = ReportingHarness { WorkDirectory };
    auto const sessions = h.harness.host.sessionCount();

    std::ignore =
        h.harness.exchange({ proto::DecodedPdu { proto::ClientHello {} },
                             proto::DecodedPdu { proto::SplitPane { .session = h.session.value } } },
                           4);

    REQUIRE(h.harness.host.sessionCount() == sessions + 1);
    CHECK(h.spawned->back() == std::filesystem::path { WorkDirectory });
}

TEST_CASE("a tab or split beside a pane with no known directory keeps the profile's", "[vthost][native][cwd]")
{
    auto const spawned = std::make_shared<std::vector<std::filesystem::path>>();
    auto h = NativeHarness { recordingSpawnDirectories(spawned) };
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();
    spawned->clear();

    std::ignore = h.exchange({ proto::DecodedPdu { proto::ClientHello {} },
                               proto::DecodedPdu { proto::CreateTab { .session = session.value } },
                               proto::DecodedPdu { proto::SplitPane { .session = session.value } } },
                             6);

    REQUIRE(spawned->size() == 2);
    CHECK(*spawned == std::vector { std::filesystem::path {}, std::filesystem::path {} });
}

TEST_CASE("a tab beside a pane whose directory is gone keeps the profile's, and still opens",
          "[vthost][native][cwd]")
{
    auto h = ReportingHarness { WorkDirectory };
    REQUIRE(h.harness.files.remove(std::filesystem::path { WorkDirectory }).value_or(false));
    auto const sessions = h.harness.host.sessionCount();

    std::ignore =
        h.harness.exchange({ proto::DecodedPdu { proto::ClientHello {} },
                             proto::DecodedPdu { proto::CreateTab { .session = h.session.value } } },
                           4);

    REQUIRE(h.harness.host.sessionCount() == sessions + 1);
    CHECK(h.spawned->back().empty());
}
// }}}

namespace
{
/// @param kind What the client asks to follow.
/// @param workspaces The workspaces named, for AttachIntentKind::Select and Resume.
/// @return A hello that offers Capability::DaemonFollow beside the attach intent.
[[nodiscard]] proto::ClientHello followingHello(proto::AttachIntentKind kind,
                                                std::vector<uint64_t> workspaces = {})
{
    auto hello = intentHello(kind, std::move(workspaces));
    hello.capabilities =
        proto::CapabilitySet::of({ proto::Capability::AttachIntent, proto::Capability::DaemonFollow });
    return hello;
}
} // namespace

namespace
{

/// Once @p gate holds: sends @p bytes on @p client, a request ordered after what @p gate saw arrive.
Task<void> sendOnce(NativeHarness* h, std::vector<std::byte> const* bytes, StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    std::ignore = co_await h->pair.second->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// Once @p gate holds: removes @p workspace from the daemon's model, as closing everything in it does.
Task<void> removeOnce(NativeHarness* h, uint64_t workspace, StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    h->host.model().removeWorkspace(vtworkspace::WorkspaceId { workspace });
}

/// @return Whether a LayoutBatchEnd naming @p workspace as the connection's selection is among @p frames.
[[nodiscard]] bool batchEndSelects(std::vector<proto::DecodedFrame> const& frames, uint64_t workspace)
{
    return std::ranges::any_of(frames, [workspace](proto::DecodedFrame const& frame) {
        auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu);
        return end != nullptr && end->selected == workspace;
    });
}

/// @param kind What the client asks for.
/// @param workspace The workspace it names.
/// @param instance The daemon run the id is of.
/// @return A hello stating that intent, offering the batching a window shares.
[[nodiscard]] proto::ClientHello daemonFollowHello(proto::AttachIntentKind kind,
                                                   uint64_t workspace = 0,
                                                   uint64_t instance = 0)
{
    auto hello = followingHello(kind, { workspace });
    hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::LayoutBatch));
    hello.attachIntent.daemonInstance = instance;
    return hello;
}

} // namespace

TEST_CASE("SelectWorkspace moves the full mirror to another workspace", "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const second = other->id().value;
    auto const oldSession = *sessionsOf(h.host, first).begin();
    auto const newSession = *sessionsOf(h.host, second).begin();
    auto const select = encodeRequest({ proto::SelectWorkspace { .workspace = second } });

    // After the snapshot of the first workspace: select the second; after its snapshot, both sessions
    // produce output, and one flush later the second's Delta is there and the first's never comes.
    auto const received = h.exchangeUntil(
        { followingHello(proto::AttachIntentKind::Default) },
        [&](auto const& got) { return deltaSessions(got).contains(newSession); },
        sendOnce(&h, &select, [&] { return snapshotRunCount(h.received) >= 1; }),
        writeOnce(&h,
                  vtworkspace::SessionId { oldSession },
                  "old",
                  [&] { return snapshotSessions(h.received).contains(newSession); }),
        writeOnce(&h, vtworkspace::SessionId { newSession }, "new", [&] {
            return snapshotSessions(h.received).contains(newSession);
        }));

    CHECK(snapshotSessions(received) == std::set<uint64_t> { oldSession, newSession });
    CHECK(deltaSessions(received) == std::set<uint64_t> { newSession });
    CHECK(h.session->selectedWorkspace() == second);
    CHECK(h.host.lastSelectedWorkspace() == std::optional<uint64_t> { second });
    CHECK(h.host.visibility(first) == vthost::WorkspaceVisibility::Hidden);
    CHECK(h.host.visibility(second) == vthost::WorkspaceVisibility::Shown);
}

TEST_CASE("SelectWorkspace 0 creates a workspace and selects it", "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;

    auto const received = h.exchangeUntil(
        { followingHello(proto::AttachIntentKind::Default), proto::SelectWorkspace { .workspace = 0 } },
        [](auto const& got) { return layoutWorkspaces(got).size() == 2 && snapshotRunCount(got) >= 2; });

    auto const workspaces = layoutWorkspaces(received);
    REQUIRE(workspaces.size() == 2);
    auto const created =
        *std::ranges::find_if(workspaces, [first](uint64_t workspace) { return workspace != first; });
    CHECK(h.session->selectedWorkspace() == created);
    CHECK(snapshotSessions(received).contains(*sessionsOf(h.host, created).begin()));
    CHECK(h.host.lastSelectedWorkspace() == std::optional<uint64_t> { created });
}

TEST_CASE("SelectWorkspace naming no workspace keeps the selection, and one from a peer without "
          "daemon-follow is refused",
          "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;

    SECTION("an unknown workspace is answered with the selection it kept, not as an unknown PDU")
    {
        // Sent under serial 1, which the batch answering it names as its cause.
        auto const select = encodeRequest({ proto::SelectWorkspace { .workspace = 999 } });
        auto const answer = [](std::vector<proto::DecodedFrame> const& got) -> proto::LayoutBatchEnd const* {
            for (auto const& frame: got)
                if (auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu);
                    end && end->causeSerial == 1)
                    return end;
            return nullptr;
        };
        auto const received = h.exchangeUntil(
            { daemonFollowHello(proto::AttachIntentKind::Default) },
            [&](auto const& got) { return answer(got) != nullptr; },
            sendOnce(&h, &select, [&] { return snapshotRunCount(h.received) >= 1; }));
        CHECK(std::ranges::count_if(received, isUnsupported) == 0);
        REQUIRE(answer(received) != nullptr);
        CHECK(answer(received)->selected == first);
        CHECK(h.session->selectedWorkspace() == first);
    }
    SECTION("a workspace whose last session ended is no workspace to select")
    {
        auto const ended = h.host.createWorkspace()->id();
        h.host.closeSession(h.host.model().workspace(ended)->tabAt(0)->rootPane()->session());
        REQUIRE(h.host.model().workspace(ended) != nullptr); // kept, emptied
        auto const select = encodeRequest({ proto::SelectWorkspace { .workspace = ended.value } });
        auto const answered = [](std::vector<proto::DecodedFrame> const& got) {
            return std::ranges::any_of(got, [](proto::DecodedFrame const& frame) {
                auto const* end = std::get_if<proto::LayoutBatchEnd>(&frame.pdu);
                return end != nullptr && end->causeSerial == 1;
            });
        };
        std::ignore =
            h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Default) },
                            answered,
                            sendOnce(&h, &select, [&] { return snapshotRunCount(h.received) >= 1; }));
        CHECK(h.session->selectedWorkspace() == first);
    }
    SECTION("a peer without the capability")
    {
        auto const received =
            h.exchangeUntil({ intentHello(proto::AttachIntentKind::Default),
                              proto::DecodedPdu { proto::SelectWorkspace { .workspace = first } } },
                            [](auto const& got) { return std::ranges::count_if(got, isUnsupported) >= 1; });
        CHECK(std::ranges::count_if(received, isUnsupported) == 1);
    }
    CHECK(h.session->selectedWorkspace() == first);
}

namespace
{

/// Once @p gate holds: closes the last tab of @p workspace, as the shell of its only pane exiting does.
Task<void> closeLastTabOnce(NativeHarness* h, uint64_t workspace, StepGate gate)
{
    co_await awaitGate(&h->loop, std::move(gate));
    auto const id = vtworkspace::WorkspaceId { workspace };
    h->host.model().closeTab(id, h->host.model().workspace(id)->tabAt(0)->id());
}

} // namespace

TEST_CASE("a connection whose selected workspace ends selects another", "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const second = other->id().value;
    auto const secondSession = *sessionsOf(h.host, second).begin();
    auto const attached = [&] {
        return snapshotRunCount(h.received) >= 1;
    };
    auto const moved = [&](auto const& got) {
        return batchEndSelects(got, second) && snapshotSessions(got).contains(secondSession);
    };

    SECTION("its last tab closes, the workspace staying in the model")
    {
        std::ignore = h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Select, first) },
                                      moved,
                                      closeLastTabOnce(&h, first, attached));
        REQUIRE(h.host.model().workspace(vtworkspace::WorkspaceId { first }) != nullptr);
    }
    SECTION("it leaves the model")
    {
        std::ignore = h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Select, first) },
                                      moved,
                                      removeOnce(&h, first, attached));
    }

    CHECK(h.session->selectedWorkspace() == second);
    CHECK(h.host.lastSelectedWorkspace() == std::optional<uint64_t> { second });
}

TEST_CASE("a connection whose only workspace ends selects none and creates none",
          "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const attached = [&] {
        return snapshotRunCount(h.received) >= 1;
    };
    auto const selectsNone = [](auto const& got) {
        return batchEndSelects(got, 0);
    };

    SECTION("its last tab closes")
    {
        std::ignore = h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Select, first) },
                                      selectsNone,
                                      closeLastTabOnce(&h, first, attached));
        CHECK(h.host.model().workspaceCount() == 1);
    }
    SECTION("it leaves the model")
    {
        std::ignore = h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Select, first) },
                                      selectsNone,
                                      removeOnce(&h, first, attached));
        CHECK(h.host.model().workspaceCount() == 0);
    }

    CHECK(h.session->selectedWorkspace() == 0);
}

TEST_CASE("a workspace whose last session ended is selectable again once it holds a tab",
          "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const own = h.host.workspaceId();
    auto const other = h.host.createWorkspace()->id();
    auto const endLastSession = [&h](vtworkspace::WorkspaceId workspace) {
        h.host.closeSession(h.host.model().workspace(workspace)->tabAt(0)->rootPane()->session());
        REQUIRE(h.host.hasEnded(workspace));
    };

    auto refilled = vtworkspace::WorkspaceId {};
    SECTION("by a tab created naming no workspace, which lands in the host's own")
    {
        endLastSession(own);
        REQUIRE(h.host.createTab() != nullptr);
        refilled = own;
    }
    SECTION("by a tab moved there")
    {
        REQUIRE(h.host.createTab() != nullptr); // a second tab in the host's own, to move
        endLastSession(other);
        auto& model = h.host.model();
        model.moveTabToWorkspace(own, model.workspace(own)->tabAt(1)->id(), other, 0);
        refilled = other;
    }
    CHECK_FALSE(h.host.hasEnded(refilled));

    // Selected: the batch answering the request states it.
    auto const select = encodeRequest({ proto::SelectWorkspace { .workspace = refilled.value } });
    std::ignore = h.exchangeUntil(
        { daemonFollowHello(proto::AttachIntentKind::Select, refilled == own ? other.value : own.value) },
        [&](auto const& got) { return batchEndSelects(got, refilled.value); },
        sendOnce(&h, &select, [&] { return snapshotRunCount(h.received) >= 1; }));
    CHECK(h.session->selectedWorkspace() == refilled.value);
}

TEST_CASE("an attach selecting a workspace whose last session ended selects as a launch does",
          "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const ended = h.host.createWorkspace()->id();
    h.host.closeSession(h.host.model().workspace(ended)->tabAt(0)->rootPane()->session());
    REQUIRE(h.host.model().workspace(ended) != nullptr); // kept, emptied

    std::ignore = h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Select, ended.value) },
                                  [](auto const& got) { return snapshotRunCount(got) >= 1; });
    CHECK(h.session->selectedWorkspace() == first);
}

TEST_CASE("a workspace made empty for its first tab has not ended", "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const& empty = h.host.createEmptyWorkspace();
    auto const emptyWorkspace = empty.id().value;
    auto const select = encodeRequest({ proto::SelectWorkspace { .workspace = emptyWorkspace } });

    // Selected while it holds nothing, then another workspace changes (a batch is pushed): the connection
    // stays on it.
    std::ignore = h.exchangeUntil(
        { daemonFollowHello(proto::AttachIntentKind::Select, first) },
        [&](auto const& got) {
            auto const* layout = lastLayoutOf(got, first);
            return layout != nullptr && layout->tabs.size() == 2 && batchEndSelects(got, emptyWorkspace);
        },
        sendOnce(&h, &select, [&] { return snapshotRunCount(h.received) >= 1; }),
        [](NativeHarness* h, uint64_t workspace) -> Task<void> {
            co_await awaitGate(&h->loop,
                               [h, workspace] { return h->session->selectedWorkspace() == workspace; });
            std::ignore = h->host.createTab();
        }(&h, emptyWorkspace));
    CHECK(h.session->selectedWorkspace() == emptyWorkspace);
}

namespace
{

/// Attaches to @p oldWorkspace, then parks a snapshot of @p oldSession behind a send backlog the client
/// does not read, selects @p newSession's workspace meanwhile, and reads on.
/// @param frames Receives what the client decodes.
/// @param switchedAt Receives how many frames had arrived when the selection was made.
Task<void> switchWhileSnapshotParked(NativeHarness* h,
                                     vtworkspace::SessionId oldSession,
                                     vtworkspace::WorkspaceId newWorkspace,
                                     vtworkspace::SessionId newSession,
                                     std::vector<proto::DecodedFrame>* frames,
                                     std::size_t* switchedAt)
{
    auto* client = h->pair.second.get();
    co_await decodeUntil(client, [](auto const& got) { return snapshotRunCount(got) >= 1; }, frames, {});
    // The client reads nothing now: one frame fills its pipe, the next waits in the writer's hands, and
    // the frames behind it are the backlog the streamer must wait out.
    using Tester = vthost::NativeSessionFollowTester;
    Tester::stallSendQueue(*h->session, SmallQueueBytes - 4096);
    co_await h->loop.delay(50ms);
    Tester::stallSendQueue(*h->session, SmallQueueBytes - 4096);
    co_await h->loop.delay(50ms);
    for (auto const frame: std::views::iota(0, 4))
    {
        (void) frame;
        Tester::stallSendQueue(*h->session, std::size_t { 12 } * 1024);
    }
    Tester::requestSnapshotOf(*h->session, oldSession);
    auto const parked = co_await core::net::testing::waitUntil(
        &h->loop,
        [h, oldSession] {
            return vthost::NativeSessionFollowTester::streamingOffQueue(*h->session, oldSession);
        },
        StepTicks);
    CHECK(parked);
    vthost::NativeSessionFollowTester::select(*h->session, newWorkspace);
    *switchedAt = frames->size();
    // Reads on: the backlog drains, the parked streamer wakes, and the new workspace's snapshot follows.
    co_await core::net::testing::anyOf(decodeUntil(client,
                                                   [newSession](auto const& got) {
                                                       return snapshotSessions(got).contains(newSession.value)
                                                              && snapshotRunCount(got) >= 2;
                                                   },
                                                   frames,
                                                   {}),
                                       core::net::testing::sleepFor(&h->loop, 5s));
    client->close();
}

} // namespace

TEST_CASE("a snapshot parked behind a send backlog when the selection moves is dropped, not sent",
          "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness { smallQueueOptions() };
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const oldSession = vtworkspace::SessionId { *sessionsOf(h.host, first).begin() };
    auto const newSession = vtworkspace::SessionId { *sessionsOf(h.host, other->id().value).begin() };
    auto const bytes = encodeRequest({ followingHello(proto::AttachIntentKind::Select, { first }) });

    auto frames = std::vector<proto::DecodedFrame> {};
    auto switchedAt = std::size_t { 0 };
    h.loop.blockOn(core::net::testing::allOf(
        h.session->run(),
        feedBytes(h.pair.second.get(), &bytes),
        switchWhileSnapshotParked(&h, oldSession, other->id(), newSession, &frames, &switchedAt)));

    REQUIRE(switchedAt > 0);
    auto const after = std::span { frames }.subspan(switchedAt);
    auto const touchesOld = [oldSession](proto::DecodedFrame const& frame) {
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            return state->session == oldSession.value;
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
            return delta->session == oldSession.value;
        if (auto const* echo = std::get_if<proto::SessionInputEcho>(&frame.pdu))
            return echo->session == oldSession.value;
        return false;
    };
    CHECK_FALSE(std::ranges::any_of(after, touchesOld));
    CHECK_FALSE(vthost::NativeSessionFollowTester::follows(*h.session, oldSession));
    CHECK(snapshotSessions(frames).contains(newSession.value));
}

TEST_CASE("a resumed connection selects the workspace it had", "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness { HarnessOptions { .instance = 9 } };
    REQUIRE(h.host.createTab() != nullptr);
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const second = other->id().value;

    auto const received =
        h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Resume, second, 9),
                          proto::FetchImage { .imageId = 4242 } },
                        [](auto const& got) { return imageGoneArrived(got) && snapshotRunCount(got) >= 1; });

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK(hello->selectedWorkspace == second);
    CHECK(snapshotSessions(received) == sessionsOf(h.host, second));
}

TEST_CASE("a resume naming a workspace of another daemon instance selects the default",
          "[vthost][native][daemon-follow]")
{
    auto h = NativeHarness { HarnessOptions { .instance = 9 } };
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);

    // The ids of a restarted daemon count from 1 again: the second workspace of another run is no workspace
    // of this one.
    auto const received =
        h.exchangeUntil({ daemonFollowHello(proto::AttachIntentKind::Resume, other->id().value, 8),
                          proto::FetchImage { .imageId = 4242 } },
                        imageGoneArrived);

    auto const* hello = serverHelloOf(received);
    REQUIRE(hello != nullptr);
    CHECK(hello->selectedWorkspace == first);
}

TEST_CASE("visibility is Shown while some connection selects it",
          "[vthost][native][daemon-follow][visibility]")
{
    auto h = NativeHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const first = h.host.workspaceId().value;
    auto const* other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const second = other->id().value;
    REQUIRE(h.host.visibility(first) == vthost::WorkspaceVisibility::Hidden);

    std::ignore = h.exchangeUntil(
        { followingHello(proto::AttachIntentKind::Select, { first }), proto::FetchImage { .imageId = 4242 } },
        imageGoneArrived);
    CHECK(h.host.visibility(first) == vthost::WorkspaceVisibility::Shown);
    CHECK(h.host.visibility(second) == vthost::WorkspaceVisibility::Hidden);

    h.session.reset();
    CHECK(h.host.visibility(first) == vthost::WorkspaceVisibility::Hidden);
    CHECK(h.host.lastSelectedWorkspace() == std::optional<uint64_t> { first });
}

// {{{ status stream and raise role
namespace
{

/// Two workspaces of one session each, and windows on a ManualClock handshaken by hand, each selecting the
/// workspace it is asked to.
struct SummaryFixture
{
    FlushHarness flush {};
    vtworkspace::SessionId inFirst = newSession(flush.h.host);
    uint64_t first = flush.h.host.workspaceId().value;
    uint64_t second = flush.h.host.createWorkspace()->id().value;
    vtworkspace::SessionId inSecond { *sessionsOf(flush.h.host, second).begin() };

    /// @return A hello that selects @p workspace and shares the capabilities the status stream rides on.
    [[nodiscard]] static proto::ClientHello helloSelecting(uint64_t workspace)
    {
        return proto::ClientHello {
            .capabilities = proto::CapabilitySet::of({ proto::Capability::AttachIntent,
                                                       proto::Capability::DaemonFollow,
                                                       proto::Capability::ProgramStatus }),
            .attachIntent = { .kind = proto::AttachIntentKind::Select, .workspace = workspace }
        };
    }

    /// Attaches a window selecting @p workspace.
    FlushClient& attachSelecting(uint64_t workspace)
    {
        auto const shown = workspace == first ? inFirst : inSecond;
        return flush.attach(shown, helloSelecting(workspace));
    }

    /// Delivers what the sessions raised, then lets the flush that is due go out, and the window read what
    /// it sent.
    void settle()
    {
        turnIdle(flush.h.loop);
        flush.clock.advance(30ms);
        turnIdle(flush.h.loop);
    }

    /// @return The PaneSummary PDUs @p client received for @p session after its first @p skip frames.
    [[nodiscard]] static std::vector<proto::PaneSummary> summariesOf(FlushClient const& client,
                                                                     vtworkspace::SessionId session,
                                                                     std::size_t skip = 0)
    {
        auto found = std::vector<proto::PaneSummary> {};
        for (auto const& frame: client.received | std::views::drop(skip))
            if (auto const* summary = std::get_if<proto::PaneSummary>(&frame.pdu);
                summary != nullptr && summary->session == session.value)
                found.push_back(*summary);
        return found;
    }

    /// @return The raise roles @p client was told, in order.
    [[nodiscard]] static std::vector<proto::RaiseRole> rolesOf(FlushClient const& client)
    {
        auto roles = std::vector<proto::RaiseRole> {};
        for (auto const& frame: client.received)
            if (auto const* role = std::get_if<proto::NotifyRole>(&frame.pdu))
                roles.push_back(role->role);
        return roles;
    }

    /// @return How many PDUs of type @p Pdu naming @p session @p client received.
    template <typename Pdu>
    [[nodiscard]] static std::size_t countOf(FlushClient const& client, vtworkspace::SessionId session)
    {
        return static_cast<std::size_t>(std::ranges::count_if(client.received, [session](auto const& frame) {
            auto const* pdu = std::get_if<Pdu>(&frame.pdu);
            return pdu != nullptr && pdu->session == session.value;
        }));
    }

    /// Reports @p focus from @p client as its window would.
    static void reportFocus(FlushClient& client, proto::WindowFocus focus)
    {
        NativeSessionFlushTester::serveWorkspaceVerb(*client.session, proto::ReportFocus { .focus = focus });
    }
};

} // namespace

TEST_CASE("an unselected pane's title change reaches the client as a PaneSummary",
          "[vthost][native][daemon-follow][status]")
{
    auto f = SummaryFixture {};
    auto& client = f.attachSelecting(f.first);
    f.settle();
    auto const announced = SummaryFixture::summariesOf(client, f.inSecond);
    REQUIRE(announced.size() == 1); // the pane's row starts from what it is now
    auto const skip = client.received.size();

    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\033]2;building\033\\");
    f.settle();

    auto const summaries = SummaryFixture::summariesOf(client, f.inSecond, skip);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries.front().title == std::optional<std::string> { "building" });
    CHECK(SummaryFixture::countOf<proto::PaneSummary>(client, f.inFirst) == 0); // a mirrored pane has none
}

TEST_CASE("activity marks are throttled to one per second per session",
          "[vthost][native][daemon-follow][status]")
{
    auto f = SummaryFixture {};
    auto& client = f.attachSelecting(f.first);
    auto const skip = client.received.size();
    auto const marks = [&] {
        return std::ranges::count_if(SummaryFixture::summariesOf(client, f.inSecond, skip),
                                     [](proto::PaneSummary const& summary) {
                                         return summary.activity == proto::OutputActivity::Output;
                                     });
    };

    // Each output gets its own flush inside the second, so only the pacing can hold the marks back.
    for ([[maybe_unused]] auto const output: std::views::iota(0, 10))
    {
        client.session->sessionScreenUpdated(f.inSecond);
        f.settle();
    }
    CHECK(marks() == 1);

    f.flush.clock.advance(1s);
    client.session->sessionScreenUpdated(f.inSecond);
    f.settle();
    CHECK(marks() == 2);
}

TEST_CASE("a finished command in an unselected workspace is summarized",
          "[vthost][native][daemon-follow][status]")
{
    auto f = SummaryFixture {};
    auto& client = f.attachSelecting(f.first);
    auto const skip = client.received.size();

    f.flush.h.host.terminal(f.inSecond)->writeToScreen(commandCycle("make all", 3));
    f.settle();

    auto const summaries = SummaryFixture::summariesOf(client, f.inSecond, skip);
    auto const finished = std::ranges::find_if(
        summaries, [](proto::PaneSummary const& summary) { return summary.finished.has_value(); });
    REQUIRE(finished != summaries.end());
    CHECK(finished->finished->exitCode == std::optional<int32_t> { 3 });
    CHECK(finished->finished->commandLine == "make all");
}

TEST_CASE("a pending finished command survives another workspace being selected",
          "[vthost][native][daemon-follow][status]")
{
    auto f = SummaryFixture {};
    auto& client = f.attachSelecting(f.first);
    f.settle();
    auto const skip = client.received.size();

    // The finished command waits in the coalescing window when the window selects another workspace, which
    // announces the unmirrored panes once more.
    auto const third = f.flush.h.host.createWorkspace()->id().value;
    f.flush.h.host.terminal(f.inSecond)->writeToScreen(commandCycle("make all", 3));
    turnIdle(f.flush.h.loop);
    NativeSessionFlushTester::serveWorkspaceVerb(*client.session,
                                                 proto::SelectWorkspace { .workspace = third });
    f.settle();

    auto const summaries = SummaryFixture::summariesOf(client, f.inSecond, skip);
    auto const finished = std::ranges::find_if(
        summaries, [](proto::PaneSummary const& summary) { return summary.finished.has_value(); });
    REQUIRE(finished != summaries.end());
    CHECK(finished->finished->commandLine == "make all");
}

TEST_CASE("program status of an unselected pane reaches every client",
          "[vthost][native][daemon-follow][status]")
{
    auto f = SummaryFixture {};
    auto& one = f.attachSelecting(f.first);
    auto& two = f.attachSelecting(f.first);

    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\033]7501;state=working:id=build:app=make\033\\");
    f.settle();

    CHECK(SummaryFixture::countOf<proto::ProgramStatus>(one, f.inSecond) == 1);
    CHECK(SummaryFixture::countOf<proto::ProgramStatus>(two, f.inSecond) == 1);
}

TEST_CASE("the most recently focused client holds the raise role", "[vthost][native][daemon-follow][raise]")
{
    using proto::RaiseRole;
    auto f = SummaryFixture {};
    auto& one = f.attachSelecting(f.first);
    f.settle();
    CHECK(SummaryFixture::rolesOf(one) == std::vector { RaiseRole::Raise }); // alone, it holds it

    auto& two = f.attachSelecting(f.first);
    f.settle();
    // Neither was ever focused: the one attached last holds the role.
    CHECK(SummaryFixture::rolesOf(one) == std::vector { RaiseRole::Raise, RaiseRole::Quiet });
    CHECK(SummaryFixture::rolesOf(two) == std::vector { RaiseRole::Raise });

    SummaryFixture::reportFocus(one, proto::WindowFocus::Focused);
    f.settle();
    CHECK(SummaryFixture::rolesOf(one)
          == std::vector { RaiseRole::Raise, RaiseRole::Quiet, RaiseRole::Raise });
    CHECK(SummaryFixture::rolesOf(two) == std::vector { RaiseRole::Raise, RaiseRole::Quiet });

    // A report that changes nobody's role sends nothing.
    SummaryFixture::reportFocus(one, proto::WindowFocus::Focused);
    SummaryFixture::reportFocus(two, proto::WindowFocus::Unfocused);
    f.settle();
    CHECK(SummaryFixture::rolesOf(one).size() == 3);
    CHECK(SummaryFixture::rolesOf(two).size() == 2);
}

TEST_CASE("the raise role passes on when its holder detaches", "[vthost][native][daemon-follow][raise]")
{
    using proto::RaiseRole;
    auto f = SummaryFixture {};
    auto& one = f.attachSelecting(f.first);
    auto& two = f.attachSelecting(f.first);
    SummaryFixture::reportFocus(two, proto::WindowFocus::Focused);
    f.settle();
    REQUIRE(f.flush.h.host.raiseHolderNow() == std::optional { two.session->connectionId() });

    REQUIRE(NativeSessionFlushTester::letGo(*two.session, f.flush.h.loop, f.flush.clock));
    two.session.reset();
    f.settle();

    CHECK(f.flush.h.host.raiseHolderNow() == std::optional { one.session->connectionId() });
    CHECK(SummaryFixture::rolesOf(one).back() == RaiseRole::Raise);
}

TEST_CASE("a clipboard write reaches only the focused client that selects the pane's workspace",
          "[vthost][native][daemon-follow][clipboard]")
{
    auto f = SummaryFixture {};
    auto& earlier = f.attachSelecting(f.second);
    auto& later = f.attachSelecting(f.second);
    auto& elsewhere = f.attachSelecting(f.first);
    SummaryFixture::reportFocus(later, proto::WindowFocus::Focused);
    SummaryFixture::reportFocus(earlier, proto::WindowFocus::Focused);
    SummaryFixture::reportFocus(elsewhere,
                                proto::WindowFocus::Focused); // focused last, but shows no such pane

    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\033]52;c;aGk=\033\\");
    f.settle();

    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(earlier, f.inSecond) == 1);
    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(later, f.inSecond) == 0);
    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(elsewhere, f.inSecond) == 0);
}

TEST_CASE("a clipboard write no client displays is dropped", "[vthost][native][daemon-follow][clipboard]")
{
    auto f = SummaryFixture {};
    auto& one = f.attachSelecting(f.first);
    auto& two = f.attachSelecting(f.first);

    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\033]52;c;aGk=\033\\");
    f.settle();

    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(one, f.inSecond) == 0);
    CHECK(SummaryFixture::countOf<proto::SessionClipboard>(two, f.inSecond) == 0);
}

TEST_CASE("a bell reaches every client: as the pane's event where mirrored, in its summary elsewhere",
          "[vthost][native][daemon-follow][bell]")
{
    auto f = SummaryFixture {};
    auto& elsewhere = f.attachSelecting(f.first);
    auto& mirroring = f.attachSelecting(f.second);
    f.settle();
    auto const skip = elsewhere.received.size();

    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\a");
    f.settle();

    CHECK(SummaryFixture::countOf<proto::SessionBell>(mirroring, f.inSecond) == 1);
    CHECK(SummaryFixture::countOf<proto::SessionBell>(elsewhere, f.inSecond) == 0);
    auto const summaries = SummaryFixture::summariesOf(elsewhere, f.inSecond, skip);
    CHECK(std::ranges::count(summaries, proto::PaneBell::Rang, &proto::PaneSummary::bell) == 1);
}

TEST_CASE("a desktop notification reaches every client: as the pane's event where mirrored, in its summary "
          "elsewhere",
          "[vthost][native][daemon-follow][notify]")
{
    auto f = SummaryFixture {};
    auto& elsewhere = f.attachSelecting(f.first);
    auto& mirroring = f.attachSelecting(f.second);
    f.settle();
    auto const skip = elsewhere.received.size();

    // Two within one coalescing window, told to each connection as the host tells every observer: each is
    // kept, in order.
    for (auto* const client: { &elsewhere, &mirroring })
    {
        client->session->sessionNotify(f.inSecond, "agent", "needs input", std::nullopt);
        client->session->sessionNotify(f.inSecond, "agent", "done", vtbackend::DisplayOccasion::Always);
    }
    f.settle();

    CHECK(SummaryFixture::countOf<proto::SessionNotify>(mirroring, f.inSecond) == 2);
    CHECK(SummaryFixture::countOf<proto::SessionNotify>(elsewhere, f.inSecond) == 0);
    auto notified = std::vector<proto::PaneNotification> {};
    for (auto const& summary: SummaryFixture::summariesOf(elsewhere, f.inSecond, skip))
        notified.insert(notified.end(), summary.notifications.begin(), summary.notifications.end());
    CHECK(notified
          == std::vector {
              proto::PaneNotification {
                  .title = "agent", .body = "needs input", .occasion = proto::NotifyOccasion::Unstated },
              proto::PaneNotification {
                  .title = "agent", .body = "done", .occasion = proto::NotifyOccasion::Always },
          });
}

TEST_CASE("a raise role change sends what was summarized under the old role first",
          "[vthost][native][daemon-follow][raise]")
{
    auto f = SummaryFixture {};
    auto& one = f.attachSelecting(f.first);
    auto& two = f.attachSelecting(f.first);
    SummaryFixture::reportFocus(one, proto::WindowFocus::Focused);
    f.settle();
    REQUIRE(SummaryFixture::rolesOf(one).back() == proto::RaiseRole::Raise);
    auto const skipOne = one.received.size();
    auto const skipTwo = two.received.size();

    // A bell waits in the coalescing window when the other window takes the role.
    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\a");
    turnIdle(f.flush.h.loop);
    SummaryFixture::reportFocus(two, proto::WindowFocus::Focused);
    f.settle();

    // On each connection the bell's summary comes before the role that replaces the one it was raised under:
    // the window that held the role rings, and the one that takes it does not.
    auto const bellThenRole = [&](FlushClient const& client, std::size_t skip) {
        auto const frames = client.received | std::views::drop(skip);
        auto const bell = std::ranges::find_if(frames, [&](proto::DecodedFrame const& frame) {
            auto const* summary = std::get_if<proto::PaneSummary>(&frame.pdu);
            return summary != nullptr && summary->session == f.inSecond.value
                   && summary->bell == proto::PaneBell::Rang;
        });
        auto const role = std::ranges::find_if(frames, [](proto::DecodedFrame const& frame) {
            return std::holds_alternative<proto::NotifyRole>(frame.pdu);
        });
        return bell != frames.end() && role != frames.end()
               && std::ranges::distance(frames.begin(), bell) < std::ranges::distance(frames.begin(), role);
    };
    CHECK(bellThenRole(one, skipOne));
    CHECK(bellThenRole(two, skipTwo));
}

TEST_CASE("a bell summarized before its workspace was selected still reaches the client",
          "[vthost][native][daemon-follow][bell]")
{
    auto f = SummaryFixture {};
    auto& client = f.attachSelecting(f.first);
    f.settle();
    auto const skip = client.received.size();

    // The bell waits in the coalescing window when the window selects the pane's workspace.
    f.flush.h.host.terminal(f.inSecond)->writeToScreen("\a");
    turnIdle(f.flush.h.loop);
    NativeSessionFlushTester::serveWorkspaceVerb(*client.session,
                                                 proto::SelectWorkspace { .workspace = f.second });
    f.settle();

    auto const summaries = SummaryFixture::summariesOf(client, f.inSecond, skip);
    CHECK(std::ranges::count(summaries, proto::PaneBell::Rang, &proto::PaneSummary::bell) == 1);
    // Nothing but what it raised: the pane's own stream says what it is now.
    for (auto const& summary: summaries)
        if (summary.bell == proto::PaneBell::Rang)
            CHECK_FALSE(summary.title.has_value());
}
// }}}

// ---- key authentication (AuthChallenge and the proof that answers it) ----------------------------------

namespace
{
/// A deterministic nonce source: the n-th nonce is 32 bytes of the value n, so a test knows what to sign.
class CountingRandom final: public core::IRandom
{
  public:
    void fill(std::span<std::byte> out) override
    {
        ++_calls;
        std::ranges::fill(out, static_cast<std::byte>(_calls));
    }

    /// @return The nonce the @p ordinal-th challenge carries (one-based).
    [[nodiscard]] static std::array<std::byte, 32> nonce(std::size_t ordinal)
    {
        auto result = std::array<std::byte, 32> {};
        result.fill(static_cast<std::byte>(ordinal));
        return result;
    }

  private:
    std::size_t _calls = 0;
};

/// A security key standing in for the user's, and the authorized_keys entry listing it.
struct TestKey
{
    std::unique_ptr<core::net::testing::SoftwareSecurityKey> device;
    core::net::SshPublicKey pub;

    [[nodiscard]] static TestKey make()
    {
        auto device = core::net::testing::SoftwareSecurityKey::generate(core::net::SshKeyType::SkEd25519);
        REQUIRE(device.has_value());
        auto const parsed = vthost::parseAuthorizedKeys((*device)->publicKeyLine("t"));
        REQUIRE(parsed.keys.size() == 1);
        return TestKey { .device = std::move(*device), .pub = parsed.keys.front().key };
    }

    [[nodiscard]] vthost::AuthorizedKey entry(vtcontrol::ControlGrants grants) const
    {
        return vthost::AuthorizedKey { .key = pub, .grants = grants };
    }

    /// @return A proof of this key over @p nonce, bound to @p binding.
    [[nodiscard]] proto::KeyProof prove(
        std::array<std::byte, 32> const& nonce,
        std::array<std::byte, 32> const& binding,
        core::net::testing::UserPresence presence = core::net::testing::UserPresence::Present) const
    {
        auto const message = vthost::keyAuthMessage(nonce, binding);
        auto const signedData = core::net::sshsigSignedData(vthost::KeyAuthNamespace, message);
        auto const signature = device->sign(signedData, presence, 1);
        REQUIRE(signature.has_value());
        return proto::KeyProof { .publicKey = pub.blob,
                                 .signature =
                                     core::net::assembleSshsig(pub, vthost::KeyAuthNamespace, *signature) };
    }
};

/// @return The channel binding the daemon side of these cases is told its connection has.
std::array<std::byte, 32> testBinding()
{
    auto binding = std::array<std::byte, 32> {};
    binding.fill(std::byte { 0xB1 });
    return binding;
}

/// @return The binding of some other TLS session.
std::array<std::byte, 32> otherBinding()
{
    auto binding = testBinding();
    binding.back() ^= std::byte { 0xFF };
    return binding;
}

/// @return A Control hello, which is answered by the ServerHello alone, carrying @p proof or @p token.
proto::ClientHello controlHello(std::optional<proto::KeyProof> proof, std::string token = {})
{
    return proto::ClientHello { .token = std::move(token),
                                .role = proto::ConnectionRole::Control,
                                .keyProof = std::move(proof) };
}

/// Runs a session that authenticates by key through @p hellos and returns every frame it sent.
std::vector<proto::DecodedFrame> keyAuthExchange(vthost::KeyAuthPolicy policy,
                                                 std::vector<proto::ClientHello> const& hellos,
                                                 std::vector<vthost::AcceptedToken> tokens = {},
                                                 vtcontrol::ControlGrants* grantsOut = nullptr)
{
    auto options = HarnessOptions {};
    options.services.handshake.acceptedTokens = std::move(tokens);
    options.services.handshake.keyAuth = std::move(policy);
    auto h = NativeHarness { std::move(options) };
    auto pdus = std::vector<proto::DecodedPdu> {};
    for (auto const& hello: hellos)
        pdus.emplace_back(hello);
    auto frames = exchangeAll(&h, pdus);
    if (grantsOut != nullptr)
        *grantsOut = h.session->grants();
    return frames;
}

/// @return The policy of a connection bound to testBinding(), listing @p keys.
vthost::KeyAuthPolicy policyOf(std::vector<vthost::AuthorizedKey> keys,
                               CountingRandom& random,
                               vthost::DaemonAuth auth = vthost::DaemonAuth::KeysOnly)
{
    return vthost::KeyAuthPolicy {
        .keys = std::move(keys), .auth = auth, .channelBinding = testBinding(), .random = random
    };
}

/// @return Whether @p frame is a ServerHello that admits the client.
bool admits(proto::DecodedFrame const& frame)
{
    auto const* hello = std::get_if<proto::ServerHello>(&frame.pdu);
    return hello != nullptr && hello->agreedVersion != 0;
}

/// @return The bytes of the refusal that ends @p frames, as the client reads them, or empty when the exchange
///         did not end in one.
std::vector<std::byte> refusalBytes(std::vector<proto::DecodedFrame> const& frames)
{
    auto sink = proto::Writer {};
    if (frames.empty() || !std::holds_alternative<proto::ServerHello>(frames.back().pdu)
        || admits(frames.back()))
        return {};
    proto::encodePdu(sink, 1, frames.back().pdu);
    return { sink.view().begin(), sink.view().end() };
}
} // namespace

TEST_CASE("a daemon that authenticates by key speaks first, with a challenge", "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto random = CountingRandom {};
    auto const frames = keyAuthExchange(policyOf({ key.entry(vtcontrol::ControlGrants::all()) }, random),
                                        { controlHello(std::nullopt) });
    REQUIRE_FALSE(frames.empty());
    auto const* challenge = std::get_if<proto::AuthChallenge>(&frames.front().pdu);
    REQUIRE(challenge != nullptr);
    CHECK(challenge->nonce == CountingRandom::nonce(1));
    CHECK(challenge->methods == std::to_underlying(proto::AuthMethod::Key)); // keys only: no token offered
}

TEST_CASE("a unix-socket session sends no challenge", "[vthost][native][auth]")
{
    auto h = NativeHarness {};
    auto const frames = exchangeAll(&h, { proto::DecodedPdu { controlHello(std::nullopt) } });
    REQUIRE_FALSE(frames.empty());
    CHECK(std::holds_alternative<proto::ServerHello>(frames.front().pdu));
    CHECK(admits(frames.front()));
}

TEST_CASE("a valid proof is answered with a ServerHello and the key's grants", "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto const grants = vtcontrol::ControlGrants::all();
    auto random = CountingRandom {};
    auto got = vtcontrol::ControlGrants::none();
    auto capture = core::log::ScopedCapture {};
    auto const frames = keyAuthExchange(policyOf({ key.entry(grants) }, random),
                                        { controlHello(key.prove(CountingRandom::nonce(1), testBinding())) },
                                        {},
                                        &got);
    REQUIRE(frames.size() >= 2);
    CHECK(admits(frames[1]));
    CHECK(got == grants);
    // The log names the key, and never the signature.
    CHECK(capture.contains(core::net::sshFingerprintOf(key.pub)));
}

TEST_CASE("a proof carries the grants of the key that made it", "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto const narrow = vtcontrol::parseGrants("read,screen");
    REQUIRE(narrow.has_value());
    auto random = CountingRandom {};
    auto got = vtcontrol::ControlGrants::none();
    auto const frames = keyAuthExchange(policyOf({ key.entry(*narrow) }, random),
                                        { controlHello(key.prove(CountingRandom::nonce(1), testBinding())) },
                                        {},
                                        &got);
    REQUIRE(frames.size() >= 2);
    CHECK(admits(frames[1]));
    CHECK(got == *narrow);
}

TEST_CASE("a proof over another session's binding is refused after five attempts", "[vthost][native][auth]")
{
    // What a man in the middle holds: a proof the user made on ANOTHER connection, replayed here.
    auto const key = TestKey::make();
    auto random = CountingRandom {};
    auto hellos = std::vector<proto::ClientHello> {};
    for (auto const attempt: std::views::iota(std::size_t { 1 }, std::size_t { 6 }))
        hellos.push_back(controlHello(key.prove(CountingRandom::nonce(attempt), otherBinding())));
    auto const frames =
        keyAuthExchange(policyOf({ key.entry(vtcontrol::ControlGrants::all()) }, random), hellos);

    // The opening challenge, a fresh one after each of the first four refusals, then the refusal.
    REQUIRE(frames.size() == 6);
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 5 }))
    {
        auto const* challenge = std::get_if<proto::AuthChallenge>(&frames[index].pdu);
        REQUIRE(challenge != nullptr);
        CHECK(challenge->nonce == CountingRandom::nonce(index + 1)); // fresh every time
    }
    CHECK_FALSE(refusalBytes(frames).empty());
}

TEST_CASE("five bad proofs and then a good one is still refused", "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto random = CountingRandom {};
    auto hellos = std::vector<proto::ClientHello> {};
    for (auto const attempt: std::views::iota(std::size_t { 1 }, std::size_t { 6 }))
        hellos.push_back(controlHello(key.prove(CountingRandom::nonce(attempt), otherBinding())));
    // The sixth is made over the very nonce a sixth challenge would carry, and the right binding.
    hellos.push_back(controlHello(key.prove(CountingRandom::nonce(6), testBinding())));
    auto got = vtcontrol::ControlGrants::all();
    auto const frames =
        keyAuthExchange(policyOf({ key.entry(vtcontrol::ControlGrants::all()) }, random), hellos, {}, &got);

    CHECK(std::ranges::none_of(frames, admits));
    CHECK_FALSE(refusalBytes(frames).empty());
    CHECK(got == vtcontrol::ControlGrants::none()); // nothing was granted
}

TEST_CASE("a refused key, an unknown key, a token under keys-only and a wrong token are refused alike",
          "[vthost][native][auth]")
{
    auto const listed = TestKey::make();
    auto const stranger = TestKey::make();
    auto const all = vtcontrol::ControlGrants::all();

    auto const failedKey = [&] {
        auto random = CountingRandom {};
        auto hellos = std::vector<proto::ClientHello> {};
        for (auto const attempt: std::views::iota(std::size_t { 1 }, std::size_t { 6 }))
            hellos.push_back(controlHello(listed.prove(CountingRandom::nonce(attempt), otherBinding())));
        return keyAuthExchange(policyOf({ listed.entry(all) }, random), hellos);
    }();
    auto const unknownKey = [&] {
        auto random = CountingRandom {};
        auto hellos = std::vector<proto::ClientHello> {};
        for (auto const attempt: std::views::iota(std::size_t { 1 }, std::size_t { 6 }))
            hellos.push_back(controlHello(stranger.prove(CountingRandom::nonce(attempt), testBinding())));
        return keyAuthExchange(policyOf({ listed.entry(all) }, random), hellos);
    }();
    auto random = CountingRandom {};
    auto const tokenUnderKeysOnly = keyAuthExchange(policyOf({ listed.entry(all) }, random),
                                                    { controlHello(std::nullopt, "some-token") });
    auto random2 = CountingRandom {};
    auto const wrongToken = keyAuthExchange(policyOf({ listed.entry(all) }, random2, vthost::DaemonAuth::Any),
                                            { controlHello(std::nullopt, "the-wrong-token") },
                                            { { .token = "the-right-token" } });

    auto const reference = refusalBytes(wrongToken);
    REQUIRE_FALSE(reference.empty());
    CHECK(refusalBytes(failedKey) == reference);
    CHECK(refusalBytes(unknownKey) == reference);
    // Nor does the number of frames tell a listed key from an unlisted one: each is five challenges, then the
    // refusal.
    REQUIRE(unknownKey.size() == failedKey.size());
    CHECK(unknownKey.size() == 6);
    for (auto const index: std::views::iota(std::size_t { 0 }, unknownKey.size()))
        CHECK(unknownKey[index].pdu.index() == failedKey[index].pdu.index());
    CHECK(std::ranges::all_of(unknownKey | std::views::take(5), [](auto const& frame) {
        return std::holds_alternative<proto::AuthChallenge>(frame.pdu);
    }));
    CHECK(refusalBytes(tokenUnderKeysOnly) == reference);
}

TEST_CASE("a token still works beside keys unless the daemon is keys-only", "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto random = CountingRandom {};
    auto const frames = keyAuthExchange(
        policyOf({ key.entry(vtcontrol::ControlGrants::all()) }, random, vthost::DaemonAuth::Any),
        { controlHello(std::nullopt, "the-right-token") },
        { { .token = "the-right-token" } });
    REQUIRE(frames.size() >= 2);
    auto const* challenge = std::get_if<proto::AuthChallenge>(&frames.front().pdu);
    REQUIRE(challenge != nullptr);
    CHECK(challenge->methods
          == (std::to_underlying(proto::AuthMethod::Key) | std::to_underlying(proto::AuthMethod::Token)));
    CHECK(admits(frames[1]));
}

TEST_CASE("a security-key proof made without user presence is refused", "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto random = CountingRandom {};
    auto capture = core::log::ScopedCapture {};
    auto const frames = keyAuthExchange(
        policyOf({ key.entry(vtcontrol::ControlGrants::all()) }, random),
        { controlHello(
            key.prove(CountingRandom::nonce(1), testBinding(), core::net::testing::UserPresence::Absent)) });
    CHECK(std::ranges::none_of(frames, admits));
    CHECK(capture.contains("user presence"));
}

TEST_CASE("an endpoint with keys and no token never reads its empty token list as accept-any",
          "[vthost][native][auth]")
{
    auto const key = TestKey::make();
    auto random = CountingRandom {};
    auto const frames = keyAuthExchange(
        policyOf({ key.entry(vtcontrol::ControlGrants::all()) }, random, vthost::DaemonAuth::Any),
        { controlHello(std::nullopt) });
    CHECK(std::ranges::none_of(frames, admits));
    CHECK_FALSE(refusalBytes(frames).empty());
}

namespace
{
/// A key and an SSHSIG signature made once by OpenSSH over keyAuthMessage(nonce 32 x 0x01, binding 32 x
/// 0xB1).
struct OpenSshFixture
{
    std::string_view type;      ///< Names the case.
    std::string_view publicKey; ///< The `.pub` line.
    std::string_view signature; ///< The armored signature.
};

// Made once, so the daemon agrees with ssh-keygen and not only with itself:
//   ssh-keygen -t ed25519 -N "" -C fixture -f k      (and: ssh-keygen -t ecdsa -b 256 -N "" -C fixture -f e)
//   printf 'contour-auth-v1' > msg; head -c 32 /dev/zero | tr '\0' '\001' >> msg
//   head -c 32 /dev/zero | tr '\0' '\261' >> msg         # 32 x 0xB1, the binding testBinding() states
//   ssh-keygen -Y sign -f k -n contour-auth -O hashalg=sha512 msg
// The private keys are not kept. Should keyAuthMessage, the nonce or the binding change, so must these.
constexpr auto OpenSshFixtures = std::to_array<OpenSshFixture>({
    { "ssh-ed25519",
      "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPOGEVwBoLkljc/Bl66BFDumXvW5ZoSv3rpHQs87dqpJ fixture",
      R"(-----BEGIN SSH SIGNATURE-----
U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAg84YRXAGguSWNz8GXroEUO6Ze9b
lmhK/eukdCzzt2qkkAAAAMY29udG91ci1hdXRoAAAAAAAAAAZzaGE1MTIAAABTAAAAC3Nz
aC1lZDI1NTE5AAAAQIFlLyOF6a2BKbrsdxw2OQLVIgtRP17dUjX9KFSkqQ5tDpD0jtF1rY
rRGR0vbQXhSHNKpzS+PeD4xJZH3G65KQE=
-----END SSH SIGNATURE-----
)" },
    { "ecdsa-sha2-nistp256",
      "ecdsa-sha2-nistp256 "
      "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBDAIBDqOHQhYf6VMCPntLUchx90O868V45R7PFwJ7nxS5GmHg4"
      "FG6lFe"
      "1rTQkS3DaULuo4ZDlolPO4xxo8Z9Co0= fixture",
      R"(-----BEGIN SSH SIGNATURE-----
U1NIU0lHAAAAAQAAAGgAAAATZWNkc2Etc2hhMi1uaXN0cDI1NgAAAAhuaXN0cDI1NgAAAE
EEMAgEOo4dCFh/pUwI+e0tRyHH3Q7zrxXjlHs8XAnufFLkaYeDgUbqUV7WtNCRLcNpQu6j
hkOWiU87jHGjxn0KjQAAAAxjb250b3VyLWF1dGgAAAAAAAAABnNoYTUxMgAAAGQAAAATZW
Nkc2Etc2hhMi1uaXN0cDI1NgAAAEkAAAAgP/5A2imOewhJSn256DT9Kxv+8HXPaZ8Bqc6l
2nrVsU0AAAAhAO6drQ4V8kx0enjw3XxQAJQKIQrteQ9fHhMWLw4e70Fr
-----END SSH SIGNATURE-----
)" },
});
} // namespace

TEST_CASE("a proof by a plain key made by ssh-keygen is accepted", "[vthost][native][auth]")
{
    auto const message = vthost::keyAuthMessage(CountingRandom::nonce(1), testBinding());
    REQUIRE(message.size() == 15 + 32 + 32);
    CHECK(std::ranges::count(message, std::byte { 0x01 }) == 32);
    CHECK(std::ranges::count(message, std::byte { 0xB1 }) == 32);

    for (auto const& fixture: OpenSshFixtures)
    {
        INFO(fixture.type);
        auto const keys = vthost::parseAuthorizedKeys(fixture.publicKey);
        REQUIRE(keys.keys.size() == 1);
        auto const narrow = vtcontrol::parseGrants("read,screen");
        REQUIRE(narrow.has_value());
        auto random = CountingRandom {};
        auto got = vtcontrol::ControlGrants::none();
        auto const frames = keyAuthExchange(
            policyOf({ vthost::AuthorizedKey { .key = keys.keys.front().key, .grants = *narrow } }, random),
            { controlHello(proto::KeyProof { .publicKey = keys.keys.front().key.blob,
                                             .signature = std::string { fixture.signature } }) },
            {},
            &got);
        REQUIRE(frames.size() >= 2);
        CHECK(admits(frames[1]));
        CHECK(got == *narrow);
    }
}

// ---- the pre-authentication deadline ------------------------------------------------------------------

namespace
{
/// Runs @p session and says so once it has ended.
Task<void> runUntilEnded(NativeSession* session, bool* ended)
{
    co_await session->run();
    *ended = true;
}

/// A session on a ManualClock whose hello must be accepted PreAuthTimeout after "accept", turned one loop
/// turn at a time so each instant is exact.
struct DeadlineHarness
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + 10s };
    NativeHarness h { [this] {
        auto options = HarnessOptions { .clock = &clock };
        options.services.handshake.authDeadline = clock.now() + vthost::PreAuthTimeout;
        return options;
    }() };
    bool ended = false;
    bool clientSawEnd = false;
    std::vector<proto::DecodedFrame> received;

    DeadlineHarness()
    {
        h.loop.spawn(runUntilEnded(h.session.get(), &ended));
        h.loop.spawn(readAll(h.pair.second.get(), &received, &clientSawEnd));
    }

    /// Lets the session end (its client closes) before anything it reaches is destroyed.
    ~DeadlineHarness()
    {
        h.pair.second->close();
        std::ignore = turnUntil(h.loop, [this] { return ended; });
    }
};
} // namespace

TEST_CASE("a connection that sends no hello is closed at the pre-authentication deadline",
          "[vthost][native][auth][deadline]")
{
    auto capture = core::log::ScopedCapture {};
    auto d = DeadlineHarness {};
    turnIdle(d.h.loop);
    CHECK_FALSE(d.ended);

    d.clock.advance(vthost::PreAuthTimeout - 1s);
    turnIdle(d.h.loop);
    CHECK_FALSE(d.ended); // a second early: still waiting

    d.clock.advance(2s);
    CHECK(turnUntil(d.h.loop, [&] { return d.ended && d.clientSawEnd; }));
    CHECK(capture.contains("no hello was accepted"));
}

TEST_CASE("a connection whose hello was accepted outlives the pre-authentication deadline",
          "[vthost][native][auth][deadline]")
{
    auto d = DeadlineHarness {};
    auto const hello = encodeRequest({ proto::DecodedPdu { controlHello(std::nullopt) } });
    d.h.loop.spawn(feedBytes(d.h.pair.second.get(), &hello));
    CHECK(turnUntil(d.h.loop, [&] { return !d.received.empty(); }));
    REQUIRE_FALSE(d.received.empty());
    CHECK(admits(d.received.front()));

    d.clock.advance(vthost::PreAuthTimeout + 5s);
    turnIdle(d.h.loop);
    CHECK_FALSE(d.ended);
}

TEST_CASE("a unix-socket session has no pre-authentication deadline", "[vthost][native][auth][deadline]")
{
    auto clock = core::platform::ManualClock { core::platform::SteadyTimePoint {} + 10s };
    auto h = NativeHarness { HarnessOptions { .clock = &clock } };
    auto ended = false;
    h.loop.spawn(runUntilEnded(h.session.get(), &ended));
    turnIdle(h.loop);
    clock.advance(10min);
    turnIdle(h.loop);
    CHECK_FALSE(ended);
    h.pair.second->close();
    CHECK(turnUntil(h.loop, [&] { return ended; }));
}

TEST_CASE("a TLS listener with neither a key nor a token is refused", "[vthost][native][auth]")
{
    auto h = NativeHarness {};
    auto tls = core::net::makeSelfSignedServerContext();
    REQUIRE(tls.has_value());
    auto random = CountingRandom {};
    auto const made = vthost::makeTlsNativeHandler(
        h.loop,
        h.host,
        *tls,
        vthost::NativeSessionServices {},
        vthost::TlsKeyAuth { .keys = {}, .auth = vthost::DaemonAuth::Any, .random = random });
    REQUIRE_FALSE(made);
    CHECK(made.error().contains("token"));
}

TEST_CASE("a TLS peer that stalls its handshake is closed at the pre-authentication deadline",
          "[vthost][native][auth][deadline]")
{
    auto capture = core::log::ScopedCapture {};
    auto clock = core::platform::ManualClock { core::platform::SteadyTimePoint {} + 10s };
    auto h = NativeHarness { HarnessOptions { .clock = &clock } };
    auto tls = core::net::makeSelfSignedServerContext();
    REQUIRE(tls.has_value());
    auto random = CountingRandom {};
    auto services = vthost::NativeSessionServices {};
    services.handshake.acceptedTokens = { vthost::AcceptedToken { .token = "tok" } };
    auto const made = vthost::makeTlsNativeHandler(
        h.loop,
        h.host,
        *tls,
        std::move(services),
        vthost::TlsKeyAuth { .keys = {}, .auth = vthost::DaemonAuth::Any, .random = random });
    REQUIRE(made);
    auto const& handler = *made;

    // The peer connects over TCP and says nothing: the server end waits for a ClientHello that never comes.
    auto pair = makeConnection(h.loop, std::nullopt);
    auto served = false;
    h.loop.spawn([](core::async::Task<void> serve, bool* done) -> Task<void> {
        co_await std::move(serve);
        *done = true;
    }(handler(vthost::ConnectionId { .endpoint = "native-tcp", .index = 9 }, std::move(pair.first)),
                                                               &served));
    auto received = std::vector<proto::DecodedFrame> {};
    auto clientSawEnd = false;
    h.loop.spawn(readAll(pair.second.get(), &received, &clientSawEnd));

    turnIdle(h.loop);
    clock.advance(vthost::PreAuthTimeout - 1s);
    turnIdle(h.loop);
    CHECK_FALSE(served);
    CHECK_FALSE(clientSawEnd);

    clock.advance(2s);
    CHECK(turnUntil(h.loop, [&] { return served && clientSawEnd; }));
    CHECK(capture.contains("TLS handshake was not finished"));
}

// {{{ workspace metadata
namespace
{

/// Whether a test window offers Capability::WorkspaceMeta.
enum class OffersMeta : uint8_t
{
    No = 0,
    Yes,
};

/// Two workspaces of one session each, and windows on a ManualClock handshaken by hand.
struct MetaFixture
{
    FlushHarness flush {};
    vtworkspace::SessionId inFirst = newSession(flush.h.host);
    uint64_t first = flush.h.host.workspaceId().value;
    uint64_t second = flush.h.host.createWorkspace()->id().value;
    vtworkspace::SessionId inSecond { *sessionsOf(flush.h.host, second).begin() };

    /// Attaches a window selecting @p workspace, offering the metadata as @p offers says.
    FlushClient& attachSelecting(uint64_t workspace, OffersMeta offers = OffersMeta::Yes)
    {
        auto hello =
            proto::ClientHello { .capabilities = proto::CapabilitySet::of(
                                     { proto::Capability::AttachIntent, proto::Capability::DaemonFollow }),
                                 .attachIntent = { .kind = proto::AttachIntentKind::Select,
                                                   .workspace = workspace } };
        if (offers == OffersMeta::Yes)
            hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::WorkspaceMeta));
        return flush.attach(workspace == first ? inFirst : inSecond, std::move(hello));
    }

    /// Lets what is due go out, and the windows read it.
    void settle()
    {
        turnIdle(flush.h.loop);
        flush.clock.advance(30ms);
        turnIdle(flush.h.loop);
    }

    /// @return The control reference of the daemon's workspace @p workspace.
    [[nodiscard]] vtcontrol::ObjectRef refOf(uint64_t workspace) const
    {
        return vtcontrol::ObjectRef {
            .kind = vtcontrol::ObjectKind::Workspace,
            .uuid = *flush.h.host.objects().uuidOf(vtworkspace::WorkspaceId { workspace }),
        };
    }

    /// @return The WorkspaceMeta PDUs @p client received after its first @p skip frames.
    [[nodiscard]] static std::vector<proto::WorkspaceMeta> metasOf(FlushClient const& client,
                                                                   std::size_t skip = 0)
    {
        auto found = std::vector<proto::WorkspaceMeta> {};
        for (auto const& frame: client.received | std::views::drop(skip))
            if (auto const* meta = std::get_if<proto::WorkspaceMeta>(&frame.pdu))
                found.push_back(*meta);
        return found;
    }
};

} // namespace

TEST_CASE("a client with WorkspaceMeta gets one WorkspaceMeta per workspace after the hello",
          "[vthost][native][workspace-meta]")
{
    auto f = MetaFixture {};
    f.flush.h.host.objects().setLabel(f.refOf(f.second), "ops");
    auto& client = f.attachSelecting(f.first);
    f.settle();

    auto const metas = MetaFixture::metasOf(client);
    REQUIRE(metas.size() == 2);
    CHECK(metas[0]
          == proto::WorkspaceMeta { .workspace = f.first,
                                    .controlId = f.refOf(f.first).uuid.bytes,
                                    .label = std::string {},
                                    .git = std::nullopt });
    CHECK(metas[1]
          == proto::WorkspaceMeta { .workspace = f.second,
                                    .controlId = f.refOf(f.second).uuid.bytes,
                                    .label = "ops",
                                    .git = std::nullopt });
    // Each comes after its workspace's layout, so the client knows the workspace by then.
    auto const layoutAt = std::ranges::find_if(client.received, [&](auto const& frame) {
        auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu);
        return layout != nullptr && layout->workspace == f.second;
    });
    auto const metaAt = std::ranges::find_if(client.received, [&](auto const& frame) {
        auto const* meta = std::get_if<proto::WorkspaceMeta>(&frame.pdu);
        return meta != nullptr && meta->workspace == f.second;
    });
    CHECK(layoutAt < metaAt);

    SECTION("and one for a workspace created since, once")
    {
        auto const skip = client.received.size();
        auto const third = f.flush.h.host.createWorkspace()->id().value;
        f.settle();
        auto const created = MetaFixture::metasOf(client, skip);
        REQUIRE(created.size() == 1);
        CHECK(created[0].workspace == third);
        CHECK(created[0].controlId == f.refOf(third).uuid.bytes);
        CHECK(created[0].label == std::optional<std::string> { "" });
    }
}

TEST_CASE("a label change reaches every client", "[vthost][native][workspace-meta]")
{
    auto f = MetaFixture {};
    auto& one = f.attachSelecting(f.first);
    auto& other = f.attachSelecting(f.second);
    f.settle();
    auto const skipOne = one.received.size();
    auto const skipOther = other.received.size();

    f.flush.h.host.objects().setLabel(f.refOf(f.second), "build");
    f.flush.h.host.objects().setLabel(f.refOf(f.second), "");
    // A tab's label is no workspace's.
    auto const* tab = f.flush.h.host.model().workspace(vtworkspace::WorkspaceId { f.first })->activeTab();
    f.flush.h.host.objects().setLabel(
        vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Tab,
                               .uuid = *f.flush.h.host.objects().uuidOf(tab->id()) },
        "tab");
    f.settle();

    auto const expected = std::vector {
        proto::WorkspaceMeta { .workspace = f.second,
                               .controlId = f.refOf(f.second).uuid.bytes,
                               .label = "build",
                               .git = std::nullopt },
        proto::WorkspaceMeta { .workspace = f.second,
                               .controlId = f.refOf(f.second).uuid.bytes,
                               .label = std::string {},
                               .git = std::nullopt },
    };
    CHECK(MetaFixture::metasOf(one, skipOne) == expected);
    CHECK(MetaFixture::metasOf(other, skipOther) == expected);
}

TEST_CASE("a client without the capability gets none", "[vthost][native][workspace-meta]")
{
    auto f = MetaFixture {};
    auto& client = f.attachSelecting(f.first, OffersMeta::No);
    f.flush.h.host.objects().setLabel(f.refOf(f.first), "ops");
    client.session->sendWorkspaceGit(f.first,
                                     proto::WireGitInfo { .branch = "main",
                                                          .kind = proto::GitHeadKindWire::Branch,
                                                          .ahead = std::nullopt,
                                                          .behind = std::nullopt });
    std::ignore = f.flush.h.host.createWorkspace();
    f.settle();
    CHECK(MetaFixture::metasOf(client).empty());
}

namespace
{
/// Answers every look-up with one state.
struct FixedGitProbe final: vthost::IGitStatusProbe
{
    [[nodiscard]] std::optional<vthost::GitInfo> probe(std::filesystem::path const& /*cwd*/) const override
    {
        return answer;
    }
    vthost::GitInfo answer {};
};

/// @return The git state of the WorkspaceMeta deltas of @p workspace @p client received after @p skip frames.
[[nodiscard]] std::vector<std::optional<proto::WireGitInfo>> gitDeltasOf(FlushClient const& client,
                                                                         uint64_t workspace,
                                                                         std::size_t skip)
{
    auto found = std::vector<std::optional<proto::WireGitInfo>> {};
    for (auto const& meta: MetaFixture::metasOf(client, skip))
        if (meta.workspace == workspace)
            found.push_back(meta.git);
    return found;
}
} // namespace

TEST_CASE("a git change reaches a client", "[vthost][native][workspace-meta][git]")
{
    auto f = MetaFixture {};
    auto& following = f.attachSelecting(f.first);
    f.settle();
    auto const skip = following.received.size();

    following.session->workspaceGitChanged(
        vthost::WorkspaceGitChangedSignal { .workspace = vtworkspace::WorkspaceId { f.second },
                                            .git = vthost::GitInfo { .kind = vtbackend::GitHeadKind::Detached,
                                                                     .branch = "abc1234",
                                                                     .ahead = 2,
                                                                     .behind = std::nullopt } });
    following.session->sendWorkspaceGit(9999, std::nullopt); // no workspace the client was told of
    f.settle();

    auto const metas = MetaFixture::metasOf(following, skip);
    REQUIRE(metas.size() == 1);
    CHECK(metas[0]
          == proto::WorkspaceMeta { .workspace = f.second,
                                    .controlId = f.refOf(f.second).uuid.bytes,
                                    .label = std::nullopt,
                                    .git = proto::WireGitInfo { .branch = "abc1234",
                                                                .kind = proto::GitHeadKindWire::Detached,
                                                                .ahead = 2,
                                                                .behind = std::nullopt } });
}

TEST_CASE("leaving a repo sends kind None", "[vthost][native][workspace-meta][git]")
{
    auto f = MetaFixture {};
    auto& following = f.attachSelecting(f.first);
    f.settle();
    auto const skip = following.received.size();

    following.session->workspaceGitChanged(vthost::WorkspaceGitChangedSignal {
        .workspace = vtworkspace::WorkspaceId { f.first }, .git = std::nullopt });
    f.settle();

    // None, not absent: absent would say nothing changed.
    auto const deltas = gitDeltasOf(following, f.first, skip);
    REQUIRE(deltas.size() == 1);
    REQUIRE(deltas[0].has_value());
    CHECK(deltas[0]->kind == proto::GitHeadKindWire::None);
    CHECK(deltas[0]->branch.empty());
}

TEST_CASE("only a following client is told the git state", "[vthost][native][workspace-meta][git]")
{
    auto f = MetaFixture {};
    auto& plain = f.flush.attach(
        f.inFirst,
        proto::ClientHello {
            .capabilities = proto::CapabilitySet::of(
                { proto::Capability::AttachIntent, proto::Capability::WorkspaceMeta }),
            .attachIntent = { .kind = proto::AttachIntentKind::Select, .workspace = f.first } });
    f.settle();
    REQUIRE_FALSE(MetaFixture::metasOf(plain).empty()); // labels and control ids it is told
    auto const skip = plain.received.size();

    plain.session->workspaceGitChanged(vthost::WorkspaceGitChangedSignal {
        .workspace = vtworkspace::WorkspaceId { f.first }, .git = vthost::GitInfo { .branch = "main" } });
    f.settle();

    CHECK(MetaFixture::metasOf(plain, skip).empty());
}

TEST_CASE("the first metadata carries the current git", "[vthost][native][workspace-meta][git]")
{
    auto probe = FixedGitProbe {};
    probe.answer =
        vthost::GitInfo { .kind = vtbackend::GitHeadKind::Branch, .branch = "main", .ahead = 1, .behind = 0 };
    auto executor = core::async::testing::ManualExecutor {};
    auto flush = FlushHarness { .workspaceGit =
                                    vthost::WorkspaceGitServices { .probe = probe, .executor = executor } };
    auto const session = newSession(flush.h.host);
    auto const workspace = flush.h.host.workspaceId().value;
    flush.h.host.terminal(session)->writeToScreen("\033]7;file:///r/a\033\\");
    auto const hello = [workspace] {
        return proto::ClientHello {
            .capabilities = proto::CapabilitySet::of({ proto::Capability::AttachIntent,
                                                       proto::Capability::DaemonFollow,
                                                       proto::Capability::WorkspaceMeta }),
            .attachIntent = { .kind = proto::AttachIntentKind::Select, .workspace = workspace }
        };
    };
    auto const settle = [&] {
        turnIdle(flush.h.loop);
        while (executor.pending() > 0)
        {
            executor.drain();
            turnIdle(flush.h.loop);
        }
        flush.clock.advance(30ms);
        turnIdle(flush.h.loop);
    };

    // The first window to follow has the host look the workspace up, and is told what it found as it changes.
    auto& first = flush.attach(session, hello());
    settle();
    REQUIRE(flush.h.host.gitInfoOf(vtworkspace::WorkspaceId { workspace }) == std::optional { probe.answer });
    auto const wire = proto::WireGitInfo {
        .branch = "main", .kind = proto::GitHeadKindWire::Branch, .ahead = 1, .behind = 0
    };
    CHECK(gitDeltasOf(first, workspace, 0).back() == std::optional { wire });

    // A window attaching since is told it in its first metadata.
    auto& second = flush.attach(session, hello());
    settle();
    auto const metas = MetaFixture::metasOf(second);
    auto const announced = std::ranges::find(metas, workspace, &proto::WorkspaceMeta::workspace);
    REQUIRE(announced != metas.end());
    CHECK(announced->git == std::optional { wire });
}
// }}}
