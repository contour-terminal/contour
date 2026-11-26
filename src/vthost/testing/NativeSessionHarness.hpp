// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the harnesses NativeSession's suites drive a connection with — an in-process pair or a
// ManualClock flush loop — and the fixtures their workspace, attention and observer cases share.
#pragma once

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
#include <vtcontrol/LegacySpelling.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vtcontrol/testing/LegacyShapes.hpp>
#include <vthost/CommandBlockWire.hpp>
#include <vthost/DaemonControlWire.hpp>
#include <vthost/FlushPolicy.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/NodeMove.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/TabMerge.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/proto/ControlVersions.hpp>
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

namespace vthost::testing
{

/// Writes the pre-encoded request bytes onto the wire.
inline core::async::Task<void> feedBytes(core::net::ISocket* client, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// The longest a test client waits for what it counts on before it gives up and closes its end.
///
/// A bound, not a budget: an exchange finishes in milliseconds, and the longest snapshot runs here
/// took about 13 s on a machine loaded well past its cores. Without it, a PDU that never comes
/// leaves the loop waiting for I/O that never completes, and the suite hangs instead of failing
/// the count.
inline constexpr auto CollectBudget = std::chrono::milliseconds { 60'000 };

/// How long a test client waits for what it counts on, and from when.
///
/// A case that waits for something that must NOT come -- a PDU a session should never send -- spends its
/// budget in full, so it wants a short one. Counted from the start, a short budget is also all the time
/// the frames that DO come have, and a loaded machine fails the case on their count instead. Counted from
/// the @ref from'th frame, the budget times only the absence the case is about.
struct CollectBound
{
    std::chrono::milliseconds budget = CollectBudget; ///< How long to wait.
    std::size_t from = 0; ///< How many frames must have arrived before the budget starts; 0: at once.
};

/// Sleeps out @p bound: its budget, once @p out holds CollectBound::from frames. Waiting for those
/// frames is bounded too, by CollectBudget, so a session that sends fewer still ends the collection.
inline core::async::Task<void> spendBound(core::net::EventLoop* loop,
                                          std::vector<proto::DecodedFrame> const* out,
                                          CollectBound bound)
{
    if (bound.from != 0)
        std::ignore = co_await core::net::testing::waitUntil(
            loop,
            [out, from = bound.from] { return out->size() >= from; },
            static_cast<int>(CollectBudget.count()));
    co_await loop->delay(bound.budget);
}

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
inline core::async::Task<void> decodeUntil(core::net::ISocket* client,
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
/// @param bound How long to wait for @p done before giving up on it, with what arrived so far.
inline core::async::Task<void> collectUntil(core::net::EventLoop* loop,
                                            core::net::ISocket* client,
                                            CollectDone done,
                                            std::vector<proto::DecodedFrame>* out,
                                            ReadPacing pacing = {},
                                            CollectBound bound = {})
{
    co_await core::net::testing::anyOf(decodeUntil(client, std::move(done), out, pacing),
                                       spendBound(loop, out, bound));
    client->close();
}

/// @return Whether @p frame terminates a snapshot run.
inline bool endsSnapshotRun(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    return delta != nullptr && delta->snapshot != 0 && delta->completesSnapshot();
}

/// @return Whether @p frame is an increment: a Delta that belongs to no snapshot run.
inline bool isIncrement(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    return delta != nullptr && delta->snapshot == 0;
}

/// Encodes @p pdus into one contiguous request, serials counting from 1.
inline std::vector<std::byte> encodeRequest(std::vector<proto::DecodedPdu> const& pdus)
{
    auto request = proto::Writer {};
    auto serial = uint64_t { 1 };
    for (auto const& pdu: pdus)
        proto::encodePdu(request, serial++, pdu);
    return { request.view().begin(), request.view().end() };
}

/// @param history The hosted terminals' scrollback depth.
/// @param blockLimits How many command-block records each hosted terminal keeps.
/// @return Host settings with @p history worth of scrollback.
inline vtbackend::Settings hostSettings(vtbackend::LineCount history,
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
    CollectBound collectBound = {};
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
    /// What the host restores as it starts; nothing by default.
    std::optional<vthost::WorkspaceRestore> restore = std::nullopt;
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
[[nodiscard]] inline vtbackend::Settings harnessSettings(HarnessOptions const& options)
{
    auto settings = hostSettings(options.history, options.blockLimits);
    if (options.adjustSettings)
        options.adjustSettings(settings);
    return settings;
}

/// @param loop The loop a real pair is driven by.
/// @param pipeBytes HarnessOptions::pipeBytes.
/// @return The connection: `first` the server's end, `second` the client's.
inline core::net::testing::SocketPair makeConnection(core::net::EventLoop& loop,
                                                     std::optional<std::size_t> pipeBytes)
{
    if (!pipeBytes)
        return *core::net::testing::makeSocketPair(loop);
    auto pipe = core::net::testing::InMemorySocketPair::create(*pipeBytes);
    return { .first = std::move(pipe.server), .second = std::move(pipe.client) };
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
                                                   .instance = options.instance,
                                                   .restore = options.restore } };
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
            collectUntil(&loop, pair.second.get(), std::move(done), &received, {}, options.collectBound),
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

/// @param session The session the input is for.
/// @param text The input, as a client encodes it.
/// @return The Input request.
[[nodiscard]] inline proto::DecodedPdu inputRequest(uint64_t session, std::string_view text)
{
    auto input = proto::Input { .session = session, .data = {} };
    for (auto const ch: text)
        input.data.push_back(static_cast<std::byte>(ch));
    return input;
}

/// @return The incremental (non-snapshot) deltas in @p frames, in arrival order.
inline std::vector<proto::Delta const*> increments(std::vector<proto::DecodedFrame> const& frames)
{
    auto deltas = std::vector<proto::Delta const*> {};
    for (auto const& frame: frames)
        if (isIncrement(frame))
            deltas.push_back(&std::get<proto::Delta>(frame.pdu));
    return deltas;
}

} // namespace vthost::testing

namespace vthost
{

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

    /// Handles @p pdu as if it had arrived on the connection under @p serial.
    static void receive(NativeSession& session, proto::DecodedPdu pdu, uint64_t serial = 1)
    {
        session.handlePdu(proto::DecodedFrame { .serial = serial, .pdu = std::move(pdu) });
    }

    /// @return The client area the connection holds for the viewer it may become; nullopt for none.
    static std::optional<vtpty::PageSize> heldArea(NativeSession const& session)
    {
        return session._statedDisplay.area;
    }

    /// Handles @p hello as if it had arrived.
    /// @return Whether it was accepted.
    [[nodiscard]] static bool tryHandshake(NativeSession& session, proto::ClientHello hello)
    {
        return session.completeHandshake(proto::DecodedFrame { .pdu = std::move(hello) });
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

namespace vthost::testing
{

/// @return A harness whose sessions run on CountingPty, so a case can tell how often each PTY was resized.
inline HarnessOptions countingPtys()
{
    return HarnessOptions {
        .ptyFactory = [](vtbackend::PageSize size,
                         std::optional<vtpty::Process::ExecInfo> const&) -> std::unique_ptr<vtpty::Pty> {
            return std::make_unique<vthost::testing::CountingPty>(size);
        }
    };
}

/// @return The CountingPty under @p id's terminal.
inline vthost::testing::CountingPty& countingPtyOf(NativeHarness& h, vtworkspace::SessionId id)
{
    return dynamic_cast<vthost::testing::CountingPty&>(
        dynamic_cast<vthost::TappingPty&>(h.host.terminal(id)->device()).inner());
}

/// Turns @p loop until @p ready holds, without ever waiting on its clock: what a case on a ManualClock
/// drives the loop with, since every timed wait there lasts until the case advances the clock.
/// @param turns The most turns to take; each one is non-blocking.
/// @return Whether @p ready held.
[[nodiscard]] inline bool turnUntil(core::net::EventLoop& loop,
                                    std::function<bool()> const& ready,
                                    int turns = 1000)
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
inline void turnIdle(core::net::EventLoop& loop, int turns = 50)
{
    std::ignore = turnUntil(loop, [] { return false; }, turns);
}

/// Decodes everything the server sends @p client into @p out until the stream ends, then says so.
inline core::async::Task<void> readAll(core::net::ISocket* client,
                                       std::vector<proto::DecodedFrame>* out,
                                       bool* ended)
{
    co_await decodeUntil(client, [](auto const&) { return false; }, out, {});
    *ended = true;
}

/// @return How many increments are among @p frames.
[[nodiscard]] inline std::size_t incrementCount(std::vector<proto::DecodedFrame> const& frames)
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
    /// What each connection is constructed with.
    vthost::NativeSessionServices services {};
    /// The status-line extensions the hosted sessions run; none by default.
    std::shared_ptr<vthost::IExtensionHosting> extensions {};
    /// What makes each session's PTY; unset, a MockPty.
    vthost::PtyFactory ptyFactory {};
    /// What the host restores as it starts; nothing by default.
    std::optional<vthost::WorkspaceRestore> restore = std::nullopt;
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + std::chrono::seconds { 10 } };
    NativeHarness h { HarnessOptions { .ptyFactory = ptyFactory,
                                       .extensions = extensions,
                                       .services = services,
                                       .clock = &clock,
                                       .restore = restore,
                                       .workspaceGit = workspaceGit } };
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

    /// Runs what is ready, then what the coalescing deadline held back, so every flush due has gone out.
    void runDueFlushes()
    {
        turnIdle(h.loop);
        clock.advance(std::chrono::milliseconds { 30 });
        turnIdle(h.loop);
    }

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

/// @return The ServerHello among @p frames, or nullptr when none arrived.
[[nodiscard]] inline proto::ServerHello const* serverHelloOf(std::vector<proto::DecodedFrame> const& frames)
{
    for (auto const& frame: frames)
        if (auto const* hello = std::get_if<proto::ServerHello>(&frame.pdu))
            return hello;
    return nullptr;
}

/// @return The workspaces the LayoutStates among @p frames describe.
[[nodiscard]] inline std::set<uint64_t> layoutWorkspaces(std::vector<proto::DecodedFrame> const& frames)
{
    auto workspaces = std::set<uint64_t> {};
    for (auto const& frame: frames)
        if (auto const* layout = std::get_if<proto::LayoutState>(&frame.pdu))
            workspaces.insert(layout->workspace);
    return workspaces;
}

/// @return The sessions a SessionState among @p frames snapshots.
[[nodiscard]] inline std::set<uint64_t> snapshotSessions(std::vector<proto::DecodedFrame> const& frames)
{
    auto sessions = std::set<uint64_t> {};
    for (auto const& frame: frames)
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            sessions.insert(state->session);
    return sessions;
}

/// @return The sessions @p host hosts in @p workspace.
[[nodiscard]] inline std::set<uint64_t> sessionsOf(SessionHost& host, uint64_t workspace)
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

/// @return A harness whose connection calls a ControlService over its host, and whose handshake offers it.
inline HarnessOptions controlApiOptions(HarnessOptions options = {})
{
    options.control = ControlServing::Served;
    options.services.handshake.capabilities =
        proto::CapabilitySet::of({ proto::Capability::ControlApi, proto::Capability::Names });
    return options;
}

/// @return A control harness whose handshake accepts "ro" for reading and the screen, "arranger" for that and
///         changing the workspace, and "admin" for all.
inline HarnessOptions narrowTokenOptions()
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

/// The directory each session a harness spawns starts in, in spawn order: empty for one spawned without
/// a directory of its own.
using SpawnedDirectories = std::shared_ptr<std::vector<std::filesystem::path>>;

/// @param spawned Receives the directory of every session the factory spawns.
/// @return Harness options whose PTY factory records the directory each spawn asks for.
[[nodiscard]] inline HarnessOptions recordingSpawnDirectories(SpawnedDirectories const& spawned)
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
[[nodiscard]] inline std::filesystem::path statedDirectory()
{
    return std::filesystem::temp_directory_path() / "contour-attach-cwd";
}

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
    void settle() { flush.runDueFlushes(); }

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

/// Whether a test window offers Capability::WorkspaceMeta, and Capability::WorkspaceColor with it.
enum class OffersMeta : uint8_t
{
    No = 0,
    Yes,
    WithColor,
    ColorOnly, ///< WorkspaceColor without the WorkspaceMeta that carries it.
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
        if (offers == OffersMeta::Yes || offers == OffersMeta::WithColor)
            hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::WorkspaceMeta));
        if (offers == OffersMeta::WithColor || offers == OffersMeta::ColorOnly)
            hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::WorkspaceColor));
        return flush.attach(workspace == first ? inFirst : inSecond, std::move(hello));
    }

    /// Lets what is due go out, and the windows read it.
    void settle() { flush.runDueFlushes(); }

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

/// A SummaryFixture whose windows share Capability::DaemonAttention.
struct AttentionFixture
{
    SummaryFixture f {};

    /// Attaches a window selecting @p workspace that shares the daemon's attention ledger.
    FlushClient& attachSelecting(uint64_t workspace)
    {
        auto hello = SummaryFixture::helloSelecting(workspace);
        hello.capabilities.idents.push_back(std::to_underlying(proto::Capability::DaemonAttention));
        return f.flush.attach(workspace == f.first ? f.inFirst : f.inSecond, std::move(hello));
    }

    /// @return The hosted terminal of @p session.
    [[nodiscard]] vtbackend::Terminal& terminal(vtworkspace::SessionId session)
    {
        return *f.flush.h.host.terminal(session);
    }

    /// @return The newest generation among @p session's program-status records.
    [[nodiscard]] uint64_t statusGeneration(vtworkspace::SessionId session)
    {
        auto newest = uint64_t { 0 };
        for (auto const& record: terminal(session).programStatus().records())
            newest = std::max(newest, record.generation);
        return newest;
    }

    /// @return What the host's ledger states of @p session.
    [[nodiscard]] vthost::PaneAttention ledgerOf(vtworkspace::SessionId session) const
    {
        for (auto const& [id, attention]: f.flush.h.host.paneAttention())
            if (id == session.value)
                return attention;
        FAIL("the ledger holds no entry for the session");
        return {};
    }

    /// @return The PaneAttentionState PDUs @p client received for @p session after its first @p skip frames.
    [[nodiscard]] static std::vector<proto::PaneAttentionState> statesOf(FlushClient const& client,
                                                                         vtworkspace::SessionId session,
                                                                         std::size_t skip = 0)
    {
        auto found = std::vector<proto::PaneAttentionState> {};
        for (auto const& frame: client.received | std::views::drop(skip))
            if (auto const* state = std::get_if<proto::PaneAttentionState>(&frame.pdu);
                state != nullptr && state->session == session.value)
                found.push_back(*state);
        return found;
    }

    /// Reports from @p client that its window has @p focus and shows @p sessions.
    static void reportShown(FlushClient& client,
                            proto::WindowFocus focus,
                            std::vector<vtworkspace::SessionId> const& sessions)
    {
        auto report = proto::ReportFocus { .focus = focus };
        for (auto const session: sessions)
            report.shownSessions.push_back(session.value);
        NativeSessionFlushTester::serveWorkspaceVerb(*client.session, std::move(report));
    }

    /// Marks @p session seen from @p client up to @p seen.
    static void markSeen(FlushClient& client, vtworkspace::SessionId session, vthost::SeenSerials seen)
    {
        NativeSessionFlushTester::serveWorkspaceVerb(
            *client.session, proto::MarkSeen { .session = session.value, .seen = seen });
    }
};

} // namespace vthost::testing
