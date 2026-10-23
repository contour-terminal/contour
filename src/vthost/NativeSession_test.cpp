// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/async/WhenAll.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/PduTrace.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
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

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    HarnessOptions options {}; // set by the constructor, before `host` and `session` read it
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
                                                   .inputWatermarks = options.inputWatermarks,
                                                   .extensions = options.extensions } };
    core::net::testing::SocketPair pair = makeConnection(loop, options.pipeBytes);
    std::unique_ptr<NativeSession> session =
        std::make_unique<NativeSession>(loop,
                                        host,
                                        vthost::ConnectionId { .endpoint = "test", .index = 1 },
                                        std::move(pair.first),
                                        options.writeQueueBytes);

    /// What `serveNativeClient` installs in production, and what a resize resync now rides: the
    /// host announces a moved grid to its stream subscribers rather than the receiving connection
    /// resyncing itself. Screen updates still come from the helpers above, because the harness
    /// runs with the pumps stopped.
    vthost::ScopedStreamSubscription subscription = makeScopedStreamSubscription(host, *session);

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
        auto const bytes = encodeRequest(pdus);
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

/// Waits up to five seconds for @p done to hold, as what a pump does on its own thread comes to pass.
/// @return Whether it held by then.
[[nodiscard]] bool eventually(std::function<bool()> const& done)
{
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 500))
    {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    return done();
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
///
/// Both sessions are subscribed to the host's stream events here, because `serveNativeClient` is
/// what does that in production and nothing in NativeSession does it for itself.
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
    vthost::ScopedStreamSubscription subscriptionOne = makeScopedStreamSubscription(host, *serverOne);
    vthost::ScopedStreamSubscription subscriptionTwo = makeScopedStreamSubscription(host, *serverTwo);
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
        loop.blockOn(core::net::pollUntil(&loop, [&session] {
            return !session._flushScheduled && !session._streamingSession
                   && session._roomWaiter == NativeSession::RoomWaiter::Idle;
        }));
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
    for ([[maybe_unused]] auto const window: std::views::iota(0, 1000))
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
    CHECK(hello->codecVersion == proto::CodecVersion);

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
    auto const otherSubscription = makeScopedStreamSubscription(h.host, *other);
    auto const otherRequest =
        encodeRequest({ proto::ClientHello {}, proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } });
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

TEST_CASE("a closed session's follow state is pruned", "[vthost][native]")
{
    auto h = NativeHarness {};
    auto const sessionId = newSession(h.host);

    // The handshake snapshot makes the session followed (pushDelta seeds _followed).
    std::ignore = h.exchange({ proto::ClientHello {} }, 3);
    REQUIRE(vthost::NativeSessionFollowTester::follows(*h.session, sessionId));

    // The host learning its own session closed must fan out to stream subscribers,
    // and the session must drop the per-session follow state (otherwise it leaks
    // for the connection's whole lifetime -- one entry per session ever opened).
    h.host.subscribeStream(h.session.get());
    h.host.handleSessionExit(sessionId);
    h.host.unsubscribeStream(h.session.get());

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

TEST_CASE("a codec-version mismatch is reported with both versions", "[vthost][native][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};
    auto harness = NativeHarness {};
    auto const received = harness.exchange(
        { proto::DecodedPdu { proto::ClientHello { .codecVersion = proto::CodecVersion - 1 } } }, 1);

    // The peer still gets its ServerHello so it can report the mismatch itself.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ServerHello>(received[0].pdu));

    CHECK(capture.contains("test#1"));
    CHECK(capture.contains("handshake rejected"));
    CHECK(capture.contains(std::format("v{}", proto::CodecVersion - 1)));
    CHECK(capture.contains(std::format("v{}", proto::CodecVersion)));
}

TEST_CASE("a peer of the command-block revision is refused", "[vthost][native][handshake]")
{
    // Revision 2 (#1010) lays a Delta out without the status rows' display byte, and knows neither
    // permission PDU: past a handshake it would mis-read every Delta, so the handshake must refuse it.
    constexpr auto CommandBlockRevision = uint32_t { 2 };
    auto capture = core::log::ScopedCapture {};
    auto harness = NativeHarness {};
    auto const received = harness.exchange(
        { proto::DecodedPdu { proto::ClientHello { .codecVersion = CommandBlockRevision } } }, 1);

    REQUIRE(received.size() == 1);
    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->machineId.empty()); // a refusal names no machine
    CHECK(capture.contains("handshake rejected"));
}

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
    CHECK(vthost::testing::shippedServerHelloVersion(frame->body) == proto::CodecVersion);
    CHECK(frame->consumed == reply.size()); // and nothing follows it: no snapshot for a refused client
    CHECK(capture.contains(std::format("peer speaks codec v{}", vthost::testing::ShippedCodecVersion)));
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
        "the-right-token");

    auto const bytes = encodeRequest({ proto::DecodedPdu {
        proto::ClientHello { .codecVersion = proto::CodecVersion, .token = "the-wrong-token" } } });
    auto received = std::vector<proto::DecodedFrame> {};
    loop.blockOn(driveExchange(&loop, session.get(), pair.second.get(), &bytes, 1, &received));

    CHECK(capture.contains("native-tcp#3@10.0.0.7"));
    CHECK(capture.contains("preshared token mismatch"));
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

    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
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

    auto const* window = h.host.model().window(h.host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 2);
    auto* first = h.host.terminal(window->tabAt(0)->rootPane()->session());
    auto* second = h.host.terminal(window->tabAt(1)->rootPane()->session());
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

    auto const* window = h.host.model().window(h.host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 2);
    auto* second = h.host.terminal(window->tabAt(1)->rootPane()->session());
    REQUIRE(second != nullptr);
    CHECK(second->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a ClientHello without settings leaves the host's own in force", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    std::ignore = h.exchange({ proto::ClientHello {} }, 4);

    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
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

    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
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
    // quadratic walk a restart from the oldest each window would cost.
    using Tester = vthost::NativeSessionFollowTester;
    auto harness = ManualHarness {};
    auto& h = harness.h;
    auto const sessionId = newSession(h.host);
    std::ignore = Tester::snapshot(*h.session, sessionId); // the attach, before any record
    h.host.terminal(sessionId)->writeToScreen(commandBurst(std::string(4000, 'a'), 40));

    auto delivered = std::vector<uint32_t> {};
    auto increments = 0;
    for ([[maybe_unused]] auto const window: std::views::iota(0, 40))
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
    auto const subscription = makeScopedStreamSubscription(h.host, *session);
    auto const bytes = encodeRequest({ proto::ClientHello {} });
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

TEST_CASE("NativeSession.extension.permission", "[vthost][native][extensions]")
{
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
    auto const isRequest = [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::PermissionRequest>(frame.pdu);
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
            { proto::ClientHello {},
              proto::PermissionReply { .hostId = asked.hostId,
                                       .digest = asked.digest,
                                       .grantState = std::to_underlying(vthost::GrantState::Granted) } },
            [&](auto const& got) {
                return std::ranges::any_of(got, isRequest) && hosting->answers.size() == 1;
            });
        auto const request = std::ranges::find_if(received, isRequest);
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
            { proto::ClientHello {},
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
            { proto::ClientHello {},
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
            { proto::ClientHello {},
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
