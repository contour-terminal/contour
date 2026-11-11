// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vthost/SessionSnapshotCodec.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/testing/KeeperFixture.hpp>

/// @file
/// The keeper with real processes: how it bounds a crashing daemon's restarts, how it stops, and that what
/// runs in the sessions survives the daemon's crash or hang and is served again by its successor.

using namespace std::chrono_literals;
using vthost::testing::HarnessLifecycle;
using vthost::testing::HarnessSetup;
using vthost::testing::HarnessStart;
using vthost::testing::HarnessTimings;
using vthost::testing::KeeperFixture;

namespace fs = std::filesystem;

namespace
{

[[nodiscard]] bool eventually(std::function<bool()> const& done, std::chrono::milliseconds bound = 20s)
{
    auto const deadline = std::chrono::steady_clock::now() + bound;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

/// @return Whether process @p pid still exists (a zombie included).
[[nodiscard]] bool exists(int pid)
{
    return pid > 0 && ::kill(pid, 0) == 0;
}

/// The grace period the crash-loop cases give their keeper: long enough to look at what it keeps, short
/// enough for a test.
constexpr auto Grace = 3s;

/// A keeper whose restarts crash before serving, with one shell that writes its pid to `shell.pid`.
[[nodiscard]] HarnessSetup crashLoop()
{
    return HarnessSetup {
        .start = HarnessStart::ServesThenCrashes,
        .panes = { "echo $$ > @DIR@/shell.pid; exec sleep 300" },
        .snapshot = vthost::testing::HarnessSnapshot::None,
        .tcp = vthost::testing::HarnessTcp::Off,
        .softDescriptorLimit = std::nullopt,
        .timings =
            HarnessTimings { .backoffMs = 50,
                             .graceMs = static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(Grace).count()) },
    };
}

/// @return The pid the fixture's shell wrote; 0 until it did.
[[nodiscard]] int shellOf(KeeperFixture const& f)
{
    auto file = std::ifstream { f.directory() / "shell.pid" };
    auto pid = 0;
    file >> pid;
    return pid;
}

/// Kills the serving daemon and waits until the keeper gave up: five crashes, the first daemon's and four
/// restarts'.
void crashUntilGivenUp(KeeperFixture& f)
{
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    f.killDaemon(SIGKILL);
    REQUIRE(eventually([&] { return f.log().contains("giving up on restarting it"); }));
}

} // namespace

TEST_CASE("a daemon that crashes on every start gives up after five, and the shells live", "[keeper][crash]")
{
    auto f = KeeperFixture { crashLoop() };
    INFO(f.log());
    crashUntilGivenUp(f);
    CHECK(f.launches().size() == 5);
    CHECK(f.log().contains("the daemon crashed 5 times within 300 s"));
    // The shells are kept through the grace period, and nothing else is started in it.
    std::this_thread::sleep_for(Grace / 2);
    CHECK(exists(shellOf(f)));
    CHECK(f.launches().size() == 5);
    CHECK(exists(f.keeperPid()));
}

TEST_CASE("after the grace the keeper ends the shells, unlinks the socket and exits non-zero",
          "[keeper][crash]")
{
    auto f = KeeperFixture { crashLoop() };
    INFO(f.log());
    crashUntilGivenUp(f);
    auto const shell = shellOf(f);
    REQUIRE(fs::exists(f.socketPath()));

    auto const exited = f.awaitKeeperExit(Grace + 20s);
    REQUIRE(exited.has_value());
    CHECK(*exited != 0);
    CHECK(f.launches().size() == 6); // the one retry, which crashed too
    CHECK_FALSE(exists(shell));
    CHECK_FALSE(fs::exists(f.socketPath()));
    CHECK_FALSE(fs::exists(vthost::nativeSocketPath(f.socketPath())));
}

namespace
{
/// @return A keeper with one shell that writes its pid to `shell.pid`, set up as @p start, @p lifecycle and
///         @p timings say.
[[nodiscard]] HarnessSetup withShell(HarnessStart start,
                                     HarnessTimings timings = {},
                                     HarnessLifecycle lifecycle = HarnessLifecycle::Persistent)
{
    return HarnessSetup { .start = start,
                          .panes = { "echo $$ > @DIR@/shell.pid; exec sleep 300" },
                          .snapshot = vthost::testing::HarnessSnapshot::None,
                          .tcp = vthost::testing::HarnessTcp::Off,
                          .softDescriptorLimit = std::nullopt,
                          .timings = timings,
                          .snapshotIntervalMs = 0,
                          .lifecycle = lifecycle };
}
} // namespace

TEST_CASE("SIGTERM to the keeper writes the final snapshot and restarts nothing", "[keeper][stop]")
{
    // Written only as the daemon ends: the interval is far longer than the test.
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = { "echo FINAL-SNAPSHOT-MARKER; exec sleep 300" },
                                            .snapshot = vthost::testing::HarnessSnapshot::Kept,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt,
                                            .timings = {},
                                            .snapshotIntervalMs = 3'600'000,
                                            .lifecycle = HarnessLifecycle::Persistent } };
    INFO(f.log());
    REQUIRE_FALSE(fs::exists(f.snapshotPath()));
    ::kill(f.keeperPid(), SIGTERM);
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(fs::exists(f.snapshotPath())); // the daemon's last write, as a stop asks for
    CHECK(f.launches().size() == 1);     // a stop is no crash
    CHECK(f.log().contains("asked to stop"));
}

TEST_CASE("StopDaemon ends the keeper", "[keeper][stop]")
{
    auto f = KeeperFixture { withShell(HarnessStart::Serves) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const shell = shellOf(f);
    REQUIRE(f.client()->stop(vthost::proto::StopReason::User).has_value());
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(f.launches().size() == 1);
    CHECK_FALSE(exists(shell)); // hung up, and reaped by the keeper
    CHECK_FALSE(fs::exists(f.socketPath()));
}

TEST_CASE("an idle ExitWhenIdle daemon ends its keeper", "[keeper][stop]")
{
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = {},
                                            .snapshot = vthost::testing::HarnessSnapshot::None,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt,
                                            .timings = {},
                                            .snapshotIntervalMs = 0,
                                            .lifecycle = HarnessLifecycle::ExitWhenIdle } };
    INFO(f.log());
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(f.launches().size() == 1);
    CHECK_FALSE(fs::exists(f.socketPath()));
}

TEST_CASE("a daemon that ignores a forwarded stop is killed after the bound", "[keeper][stop]")
{
    constexpr auto Bound = 2s;
    auto f = KeeperFixture { withShell(
        HarnessStart::IgnoresStop,
        HarnessTimings { .backoffMs = 0,
                         .graceMs = 0,
                         .daemonExitBoundMs = static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(Bound).count()) }) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const shell = shellOf(f);
    auto const daemon = f.daemonPid();
    auto const stopped = std::chrono::steady_clock::now();
    ::kill(f.keeperPid(), SIGTERM);
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(std::chrono::steady_clock::now() - stopped >= Bound);
    CHECK(f.log().contains("did not end within 2 s of being stopped; killing it"));
    CHECK(f.launches() == std::vector { daemon }); // killed for the stop, not restarted
    CHECK_FALSE(exists(shell));
    CHECK_FALSE(fs::exists(f.socketPath())); // the keeper removed what the killed daemon could not
}

TEST_CASE("stop followed at once by start succeeds", "[keeper][stop]")
{
    // Review Focus 2: the second keeper finds the label free as soon as the first daemon stopped answering,
    // while the first keeper still hangs up its shells.
    auto f = KeeperFixture { withShell(HarnessStart::Serves) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const first = f.daemonPid();
    REQUIRE(f.client()->stop(vthost::proto::StopReason::User).has_value());
    f.startSecondKeeper();
    auto second = 0;
    CHECK(eventually([&] {
        second = f.daemonPid();
        return second > 0 && second != first;
    }));
    CHECK(f.secondKeeperRuns()); // not refused for a held label
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(f.daemonPid() == second); // the first keeper's teardown leaves the new socket alone
}

// {{{ crash survival

namespace
{
namespace proto = vthost::proto;
namespace client = vthost::client;

/// How long a daemon the keeper restarts may take to serve, and a reconnected client to see it: generous, for
/// debug and sanitizer builds on a loaded machine.
constexpr auto RecoveryBound = std::chrono::milliseconds { 60s };

/// @return A keeper whose daemon runs one tab per command of @p panes, set up as the remaining arguments say.
[[nodiscard]] HarnessSetup withPanes(
    std::vector<std::string> panes,
    HarnessLifecycle lifecycle = HarnessLifecycle::Persistent,
    vthost::testing::HarnessSnapshot snapshot = vthost::testing::HarnessSnapshot::None,
    uint64_t heartbeatTimeoutMs = 0)
{
    return HarnessSetup { .start = HarnessStart::Serves,
                          .panes = std::move(panes),
                          .snapshot = snapshot,
                          .tcp = vthost::testing::HarnessTcp::Off,
                          .softDescriptorLimit = std::nullopt,
                          .timings = {},
                          .snapshotIntervalMs = 0,
                          .lifecycle = lifecycle,
                          .heartbeatTimeoutMs = heartbeatTimeoutMs };
}

/// A program named @p name that answers every line it reads with `<name>:<line>`, its terminal's echo off so
/// that only the answer shows. It writes its pid to `<name>.pid` once the echo is off, and then shows
/// `READY-<name>`.
[[nodiscard]] std::string answerer(std::string_view name)
{
    return std::format(R"(stty -echo; echo $$ > @DIR@/{0}.pid; echo READY-{0}; )"
                       R"(while IFS= read -r line; do echo "{0}:$line"; done)",
                       name);
}

/// @return The pid the program named @p name wrote; 0 until it did.
[[nodiscard]] int pidOf(KeeperFixture const& f, std::string_view name)
{
    auto file = std::ifstream { f.directory() / std::format("{}.pid", name) };
    auto pid = 0;
    file >> pid;
    return pid;
}

void collectSessions(proto::WirePane const& pane, std::vector<uint64_t>& sessions)
{
    if (!pane.isSplit())
        sessions.push_back(pane.session);
    for (auto const& child: pane.children)
        collectSessions(child, sessions);
}

/// A client of a fixture's daemon that stays attached across the daemon's crash, as a window does: a
/// ClientSupervisor that reconnects through the daemon's socket, one NativeClient per connection. Driven a
/// turn at a time from the test's thread (turnUntil()), so that the test blocks on the keeper in between.
class Mirror
{
  public:
    explicit Mirror(KeeperFixture const& f):
        _transport { std::make_shared<vthost::UnixTransportFactory>(f.socketPath(), nullptr) },
        _supervisor { _loop,
                      _transport,
                      [this](std::unique_ptr<vthost::Connection> connection, proto::AttachIntent intent) {
                          return makeClient(std::move(connection), intent);
                      },
                      proto::AttachIntent { .kind = proto::AttachIntentKind::Default },
                      client::BackoffSchedule { .first = 50ms, .ceiling = 500ms },
                      [this](client::ConnectionState state, std::string const& /*reason*/) {
                          if (state == client::ConnectionState::Connected)
                              ++_connections;
                      } }
    {
        _loop.spawn(runToEnd());
    }

    Mirror(Mirror const&) = delete;
    Mirror& operator=(Mirror const&) = delete;
    Mirror(Mirror&&) = delete;
    Mirror& operator=(Mirror&&) = delete;

    ~Mirror()
    {
        _supervisor.stop();
        std::ignore = turnUntil([this] { return _run == Run::Ended; }, 10s);
    }

    /// Turns the loop until @p done holds, for at most @p bound.
    /// @return Whether it came to hold.
    [[nodiscard]] bool turnUntil(std::function<bool()> const& done, std::chrono::milliseconds bound = 30s)
    {
        auto const deadline = std::chrono::steady_clock::now() + bound;
        while (!done())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::ignore = _loop.runOnce(10ms);
        }
        return true;
    }

    /// @return How many connections the daemon accepted so far.
    [[nodiscard]] int connections() const noexcept { return _connections; }

    /// @return The current connection's client, attached; null between connections.
    [[nodiscard]] client::NativeClient* attached() noexcept
    {
        auto* const current = _supervisor.current();
        return current != nullptr && current->connected() ? current : nullptr;
    }

    /// @return Every pane's session, sorted, as the newest layout the daemon sent shows them.
    [[nodiscard]] std::vector<uint64_t> const& sessions() const noexcept { return _sessions; }

    /// @return What @p session's screen shows on the current connection; empty without one.
    [[nodiscard]] std::string screenOf(uint64_t session) noexcept
    {
        auto* const current = attached();
        if (current == nullptr)
            return {};
        auto const screen = current->screens().find(session);
        return screen == current->screens().end() ? std::string {} : screen->second.viewportText();
    }

    /// Types @p text into @p session.
    void type(uint64_t session, std::string_view text)
    {
        auto* const current = attached();
        REQUIRE(current != nullptr);
        current->sendInput(session, text, proto::InputKind::Keyboard);
    }

  private:
    enum class Run : uint8_t
    {
        Running = 0,
        Ended,
    };

    [[nodiscard]] std::unique_ptr<client::NativeClient> makeClient(
        std::unique_ptr<vthost::Connection> connection, proto::AttachIntent intent)
    {
        return std::make_unique<client::NativeClient>(
            _loop,
            std::move(connection->socket),
            client::NativeClient::HandshakeOptions { .token = connection->token, .attachIntent = intent },
            vtbackend::CommandBlockStoreLimits {},
            client::NativeClient::UpdateHandler {},
            client::NativeClient::ImageHandler {},
            client::NativeClient::SessionEventHandler {},
            [this](proto::LayoutState const& layout) {
                _sessions.clear();
                for (auto const& tab: layout.tabs)
                    collectSessions(tab.root, _sessions);
                std::ranges::sort(_sessions);
            });
    }

    [[nodiscard]] core::async::Task<void> runToEnd()
    {
        co_await _supervisor.run();
        _run = Run::Ended;
    }

    std::unique_ptr<core::net::IoBackend> _backend = core::net::makeDefaultBackend();
    core::net::EventLoop _loop { *_backend };
    std::shared_ptr<vthost::UnixTransportFactory> _transport;
    std::vector<uint64_t> _sessions;
    int _connections = 0;
    Run _run = Run::Running;
    client::ClientSupervisor _supervisor; ///< Last: it ends before what its clients use.
};

/// Kills @p f's daemon with @p signum, and waits for the keeper's next one to serve.
/// @return The new daemon's pid.
int crashAndRecover(KeeperFixture& f, int signum)
{
    auto const before = f.daemonPid();
    REQUIRE(before > 0);
    ::kill(before, signum);
    auto const after = f.awaitNewDaemon(before, RecoveryBound);
    REQUIRE(after > 0);
    return after;
}

/// One program's run across the daemon's crash: it answers before, lives through it, and its answers after
/// reach the client that reconnected.
/// @param setup The keeper and its daemon, with one answerer("A") pane.
/// @param crash Ends @p f's daemon as a crash would.
void programSurvivesCrash(HarnessSetup const& setup, std::function<void(KeeperFixture&)> const& crash)
{
    auto f = KeeperFixture { setup };
    INFO(f.log());
    auto mirror = Mirror { f };
    REQUIRE(mirror.turnUntil([&] { return mirror.sessions().size() == 1; }));
    auto const session = mirror.sessions().front();
    REQUIRE(mirror.turnUntil([&] { return mirror.screenOf(session).contains("READY-A"); }));
    auto const program = pidOf(f, "A");
    REQUIRE(program > 0);
    mirror.type(session, "BEFORE-CRASH\r");
    REQUIRE(mirror.turnUntil([&] { return mirror.screenOf(session).contains("A:BEFORE-CRASH"); }));

    auto const crashed = f.daemonPid();
    REQUIRE(crashed > 0);
    crash(f);
    REQUIRE(f.awaitNewDaemon(crashed, RecoveryBound) > 0);
    CHECK(exists(program));
    // Not the signal itself: a sanitizer's handler may turn a SIGSEGV into an exit status of its own. Waited
    // for: the keeper's output reaches the file as its stream flushes, not as the new daemon starts.
    CHECK(eventually(
        [&] { return f.log().contains(std::format("the daemon (pid {}) crashed with ", crashed)); }));
    CHECK(f.launches().size() == 2);

    REQUIRE(mirror.turnUntil([&] { return mirror.connections() == 2 && mirror.attached() != nullptr; },
                             RecoveryBound));
    REQUIRE(mirror.turnUntil([&] { return mirror.sessions() == std::vector { session }; }));
    mirror.type(session, "AFTER-CRASH\r");
    CHECK(mirror.turnUntil([&] { return mirror.screenOf(session).contains("A:AFTER-CRASH"); }));
    CHECK(pidOf(f, "A") == program); // the very program, not a new one
}

/// One program's run across the daemon's crash by @p signum (programSurvivesCrash()).
void programSurvivesSignal(int signum)
{
    programSurvivesCrash(withPanes({ answerer("A") }), [signum](KeeperFixture& f) { f.killDaemon(signum); });
}

} // namespace

TEST_CASE("a program survives SIGKILL of the daemon and its output reaches the reconnected client",
          "[keeper][survival]")
{
    programSurvivesSignal(SIGKILL);
}

TEST_CASE("a program survives SIGSEGV of the daemon and its output reaches the reconnected client",
          "[keeper][survival]")
{
    programSurvivesSignal(SIGSEGV);
}

TEST_CASE("a program survives an exception that escapes the daemon's loop", "[keeper][survival]")
{
    // Unwound, the daemon would run a clean stop's teardown -- its host releases every session, and the
    // keeper hangs every shell up -- so behind a keeper it ends as a crash instead, and its successor
    // recovers the sessions.
    auto setup = withPanes({ answerer("A") });
    setup.start = HarnessStart::ThrowsThenServes;
    programSurvivesCrash(setup, [](KeeperFixture& f) {
        std::ofstream { f.directory() / vthost::testing::HarnessThrowTrigger } << "now\n";
        CHECK(eventually([&] { return f.log().contains("an exception escaped the daemon's loop"); }));
    });
}

#ifdef __linux__
TEST_CASE("a shell under the keeper does not inherit its ignored SIGPIPE", "[keeper][survival]")
{
    // The keeper ignores SIGPIPE; its children reset every disposition before their exec.
    auto f = KeeperFixture { withPanes({ "grep SigIgn /proc/$$/status > @DIR@/sigign; exec sleep 300" }) };
    INFO(f.log());
    auto ignored = std::string {};
    REQUIRE(eventually([&] {
        auto file = std::ifstream { f.directory() / "sigign" };
        return static_cast<bool>(std::getline(file, ignored)) && ignored.starts_with("SigIgn:");
    }));
    auto const mask = std::stoull(ignored.substr(ignored.find_first_not_of(" \t", 7)), nullptr, 16);
    CHECK((mask & (1ULL << (SIGPIPE - 1))) == 0);
}
#endif

TEST_CASE("a stopped daemon is detected as hung and replaced", "[keeper][survival]")
{
    constexpr auto HeartbeatTimeout = 2s;
    auto f = KeeperFixture { withPanes(
        { "echo $$ > @DIR@/shell.pid; exec sleep 300" },
        HarnessLifecycle::Persistent,
        vthost::testing::HarnessSnapshot::None,
        static_cast<uint64_t>(std::chrono::milliseconds { HeartbeatTimeout }.count())) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const shell = shellOf(f);
    auto const stopped = f.daemonPid();
    REQUIRE(stopped > 0);

    ::kill(stopped, SIGSTOP); // it lives, but says nothing: no heartbeat, no answer
    auto const replacement = f.awaitNewDaemon(stopped, RecoveryBound);
    REQUIRE(replacement > 0);
    CHECK(eventually([&] { return !exists(stopped); })); // killed and reaped, not left stopped
    CHECK(f.launches() == std::vector { stopped, replacement });
    CHECK(exists(shell));
    auto const log = f.log();
    CHECK(log.contains(std::format("the daemon (pid {}) is hung: no heartbeat for", stopped)));
    CHECK(log.contains(std::format("the daemon (pid {}) was killed as hung", stopped)));
}

TEST_CASE("a shell that exits during the gap is reported to the restarted daemon", "[keeper][survival]")
{
    // Review Focus 1. The daemon is stopped while the shell ends, so the keeper's word of it reaches no
    // daemon that reads it; the next one learns it from the inventory, closes the session at once, and --
    // being ExitWhenEmpty, with nothing left -- ends, and its keeper with it.
    auto f = KeeperFixture { withPanes({ "echo $$ > @DIR@/shell.pid; exec sleep 300" },
                                       HarnessLifecycle::ExitWhenEmpty) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const shell = shellOf(f);
    auto const daemon = f.daemonPid();
    REQUIRE(daemon > 0);

    ::kill(daemon, SIGSTOP);
    ::kill(shell, SIGKILL);
    REQUIRE(eventually([&] { return !exists(shell); })); // reaped by the keeper, its status kept
    ::kill(daemon, SIGKILL);

    auto const exited = f.awaitKeeperExit(RecoveryBound);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    INFO(f.log());                   // with what the restart said
    CHECK(f.launches().size() == 2); // the restart, which recovered and ended
    // Told by the keeper, not given up on after waiting for a status that never came.
    CHECK_FALSE(f.log().contains("no exit status came within"));
    // The restart took its listeners over, and the keeper removed their files: none is left behind.
    CHECK_FALSE(fs::exists(f.socketPath()));
    CHECK_FALSE(fs::exists(vthost::nativeSocketPath(f.socketPath())));
    CHECK_FALSE(fs::exists(vthost::tmuxSocketPath(f.socketPath())));
}

TEST_CASE("after a crash the same instance and ids are served and pane A's id reaches pane A's program",
          "[keeper][survival]")
{
    // Review Focus 3.
    auto f = KeeperFixture { withPanes({ answerer("A"), answerer("B") }) };
    INFO(f.log());
    auto mirror = Mirror { f };
    REQUIRE(mirror.turnUntil([&] { return mirror.sessions().size() == 2; }));
    auto const sessions = mirror.sessions();
    auto const shows = [&](std::string_view text) {
        return std::ranges::find_if(
            sessions, [&](uint64_t session) { return mirror.screenOf(session).contains(text); });
    };
    REQUIRE(mirror.turnUntil(
        [&] { return shows("READY-A") != sessions.end() && shows("READY-B") != sessions.end(); }));
    auto const a = *shows("READY-A");
    auto const b = *shows("READY-B");
    REQUIRE(a != b);
    auto const instance = mirror.attached()->daemonInstance();
    REQUIRE(instance != 0);
    auto const programs = std::pair { pidOf(f, "A"), pidOf(f, "B") };

    mirror.type(a, "MARK-1\r");
    REQUIRE(mirror.turnUntil([&] { return mirror.screenOf(a).contains("A:MARK-1"); }));

    std::ignore = crashAndRecover(f, SIGKILL);
    REQUIRE(mirror.turnUntil([&] { return mirror.connections() == 2 && mirror.attached() != nullptr; },
                             RecoveryBound));
    REQUIRE(mirror.turnUntil([&] { return mirror.sessions().size() == 2; }));
    CHECK(mirror.sessions() == sessions);
    CHECK(mirror.attached()->daemonInstance() == instance);

    mirror.type(a, "MARK-2\r");
    CHECK(mirror.turnUntil([&] { return mirror.screenOf(a).contains("A:MARK-2"); }));
    mirror.type(b, "MARK-3\r");
    CHECK(mirror.turnUntil([&] { return mirror.screenOf(b).contains("B:MARK-3"); }));
    // A sentinel behind MARK-3: once A answered it, A would have answered a misrouted MARK-3 before it.
    mirror.type(a, "MARK-4\r");
    CHECK(mirror.turnUntil([&] { return mirror.screenOf(a).contains("A:MARK-4"); }));
    // Each marker reached its own program only. B's MARK-3 came after MARK-2 was typed, so it is a sentinel
    // too.
    CHECK_FALSE(mirror.screenOf(b).contains("MARK-2"));
    CHECK_FALSE(mirror.screenOf(a).contains("MARK-3"));
    CHECK(std::pair { pidOf(f, "A"), pidOf(f, "B") } == programs);
}

namespace
{
/// @return The panes of the workspace snapshot at @p path, depth-first; none while it cannot be read.
[[nodiscard]] std::vector<vthost::PaneSnapshot> snapshotPanes(fs::path const& path)
{
    auto file = std::ifstream { path, std::ios::binary };
    auto const content =
        std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    auto const decoded = vthost::decodeSnapshot(std::as_bytes(std::span { content }));
    if (!decoded)
        return {};
    auto panes = std::vector<vthost::PaneSnapshot> {};
    auto const collect = [&](this auto const& self, vthost::PaneNode const& node) -> void {
        if (node.children.empty())
            panes.push_back(node.pane);
        for (auto const& child: node.children)
            self(child);
    };
    for (auto const& workspace: decoded->snapshot.workspaces)
        for (auto const& tab: workspace.tabs)
            collect(tab.root);
    return panes;
}
} // namespace

TEST_CASE("cwd and foreground name answer after recovery", "[keeper][survival]")
{
    // The recovered daemon asks the program's terminal, which the keeper kept, what runs in it and where:
    // what it writes to the workspace snapshot.
    auto f = KeeperFixture { withPanes({ "mkdir -p @DIR@/work && cd @DIR@/work && while [ ! -e @DIR@/go ]; "
                                         "do sleep 0.1; done; echo GONE-TO-SLEEP; exec sleep 300" },
                                       HarnessLifecycle::Persistent,
                                       vthost::testing::HarnessSnapshot::Kept) };
    INFO(f.log());
    REQUIRE(eventually([&] { return fs::exists(f.directory() / "work"); }));
    auto const work = fs::canonical(f.directory() / "work");

    std::ignore = crashAndRecover(f, SIGKILL);
    // Whatever is written from now on, the recovered daemon wrote; the output after `go` has it write again.
    fs::remove(f.snapshotPath());
    std::ofstream { f.directory() / "go" } << "go";

    auto panes = std::vector<vthost::PaneSnapshot> {};
    std::ignore = eventually(
        [&] {
            panes = snapshotPanes(f.snapshotPath());
            return panes.size() == 1 && panes.front().foregroundProcess == "sleep"
                   && panes.front().workingDirectory == work;
        },
        RecoveryBound);
    REQUIRE(panes.size() == 1);
    CHECK(panes.front().foregroundProcess == "sleep");
    CHECK(panes.front().workingDirectory == work);
}

// }}}
