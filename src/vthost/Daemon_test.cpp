// SPDX-License-Identifier: Apache-2.0

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/NonAsciiSample.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/WindowsCommandLineSplit.hpp>

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonImage.hpp>
#include <vthost/DaemonLifecycle.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/FilePrivacy.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/ShutdownSignals.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/WorkspacePersistence.hpp>
#include <vthost/handover/ImageHandoff.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/Wire.hpp>
#include <vthost/testing/FakeHandoverMemory.hpp>
#include <vthost/testing/FakeImageLeases.hpp>
#include <vthost/testing/FakeProcessImageReplacer.hpp>
#include <vthost/testing/FakeReadinessProbe.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>
#include <vthost/testing/RecordingFilePrivacy.hpp>
#include <vthost/testing/StagingFileSystem.hpp>
#include <vtworkspace/LayoutTree.hpp>

#ifndef _WIN32
    #include <sys/socket.h>
    #include <sys/un.h>

    #include <csignal>

    #include <fcntl.h>
    #include <pthread.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <span>

    #include <Windows.h>
    #include <shellapi.h>
#endif

namespace
{

namespace fs = std::filesystem;

/// A unique directory path under the system temp dir, removed on destruction.
/// Each test gets its own so concurrent runs cannot collide on a socket file.
///
/// The directory is deliberately NOT created here: listenUnix creates it with
/// owner-only permissions, and refuses to bind under a world-accessible parent.
/// Pre-creating it would hand it the default 0755 and fail that check.
///
/// Both components are kept terse on purpose: an AF_UNIX path must fit
/// sun_path's 108 bytes, and the system temp dir already eats much of that.
struct TempDir
{
    fs::path path = fs::temp_directory_path() / std::format("contour-d-{}", std::random_device {}());

    TempDir() = default;

    ~TempDir()
    {
        auto ec = std::error_code {};
        fs::remove_all(path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

} // namespace

TEST_CASE("the readiness poll starts at 5 ms and doubles to a 1 s cap", "[vthost][daemon]")
{
    using namespace std::chrono_literals;
    CHECK(vthost::nextProbeDelay(std::nullopt) == 5ms);
    CHECK(vthost::nextProbeDelay(5ms) == 10ms);
    CHECK(vthost::nextProbeDelay(640ms) == 1s);
    CHECK(vthost::nextProbeDelay(1s) == 1s);
}

TEST_CASE("ensureDaemon: a TCP endpoint is a no-op", "[vthost][daemon]")
{
    // A remote daemon cannot be auto-spawned, so this short-circuits before any
    // probe: the address below is TEST-NET-1 and is deliberately unreachable.
    auto const endpoint = vthost::AttachEndpoint { vthost::TcpEndpoint {
        .host = "192.0.2.1", .port = 1, .token = {}, .caPem = {} } };
    auto placement = vtpty::NoPlacement {};
    auto binary = vthost::InstalledDaemonBinary { "contour" };
    REQUIRE(vthost::ensureDaemon(endpoint, binary, placement, {}, std::chrono::seconds { 0 })
            == EXIT_SUCCESS);
}

// [afunix]: skips where AF_UNIX is missing, and Windows CI asserts it does not. @see the tag's
// explanation in net/Socket_test.cpp.
TEST_CASE("ensureDaemon: a listening daemon is detected, nothing is spawned", "[vthost][daemon][afunix]")
{
    // The probe is the AF_UNIX connect that must work on Windows (10 1803+) as
    // well as POSIX; a runtime SKIP documents platforms lacking AF_UNIX entirely.
    // Under a directory beyond ASCII too: core-cpp binds and connects to a path spelled in UTF-8.
    auto const directory = GENERATE(std::u8string {}, std::u8string { vtpty::testing::NonAsciiName });
    auto const tmp = TempDir {};
    auto const controlSocket = tmp.path / directory / "control";
    // listenUnix makes the socket's own directory, owner-only, but not the ones above it.
    fs::create_directories(controlSocket.parent_path().parent_path());

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };

    // Stand in for a running daemon by binding exactly the endpoint the probe
    // looks for. No accept loop is needed: reaching the listen backlog is the
    // whole liveness signal ensureDaemon asks for.
    auto listener = core::net::listenUnix(loop, core::pathToUtf8(vthost::nativeSocketPath(controlSocket)));
    if (!listener.has_value())
    {
        // Unsupported is the one acceptable failure (no AF_UNIX at all); anything
        // else is a real bind regression, so report what it actually said.
        INFO("listenUnix failed: " << listener.error().toString());
        REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    // The binary name is bogus on purpose: taking the spawn path at all is the
    // failure this guards against, and a zero timeout leaves no room to recover.
    auto const endpoint = vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = controlSocket } };
    auto placement = vtpty::NoPlacement {};
    auto binary = vthost::InstalledDaemonBinary { "contour-no-such-binary" };
    REQUIRE(vthost::ensureDaemon(endpoint, binary, placement, {}, std::chrono::seconds { 0 })
            == EXIT_SUCCESS);
}

#ifndef _WIN32
TEST_CASE("ensureDaemon: the daemon it spawns is placed as a session is", "[vthost][daemon]")
{
    // The daemon holds the scrollback of every session it hosts. Spawned into the client's own systemd
    // unit -- or into the scope of the tab `contour client` was typed into -- an out-of-memory kill of
    // anything there would end it too.
    auto const tmp = TempDir {};
    auto const endpoint =
        vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = tmp.path / "control" } };
    auto placement = vtpty::testing::ReleasingPlacement {};
    auto binary = vthost::InstalledDaemonBinary { "contour-no-such-binary" };
    REQUIRE(vthost::ensureDaemon(endpoint, binary, placement, {}, std::chrono::seconds { 0 })
            == EXIT_FAILURE);
    CHECK(placement.pids().size() == 1);
}

namespace
{

/// A Control client's whole conversation: sends @p request and collects every frame until the daemon hangs
/// up, or ten seconds pass without a byte.
core::async::Task<void> converseAsControl(core::net::EventLoop* loop,
                                          std::string socketPath,
                                          std::vector<std::byte> request,
                                          std::vector<vthost::proto::DecodedFrame>* received)
{
    auto connected = co_await core::net::connectUnix(loop, socketPath);
    REQUIRE(connected.has_value());
    auto const socket = std::move(*connected);
    // A daemon that never hangs up fails the test instead of hanging it.
    socket->setReceiveDeadline(std::chrono::seconds { 10 });
    auto const written = co_await socket->write(request);
    REQUIRE(written.has_value());

    auto pending = std::vector<std::byte> {};
    auto buffer = std::vector<std::byte>(4096);
    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return;
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
        while (true)
        {
            auto frame = vthost::proto::decodePdu(pending);
            if (!frame.has_value())
                break;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(frame->consumed));
            received->push_back(std::move(*frame));
        }
    }
}

/// A signal source the test fires by hand: what stops a daemon whose test failed before it could, so the
/// failure is reported rather than waited on forever. A fire before the watch starts is kept for it.
class ManualShutdownSignals final: public vthost::ShutdownSignals
{
  public:
    [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal,
                                               std::shared_future<void> lastWrite) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _onSignal = std::move(onSignal);
        _lastWrite = std::move(lastWrite);
        if (_fired)
            _onSignal();
        return std::make_unique<ManualWatch>(*this);
    }

    /// Asks the watching daemon to end, now or as soon as it watches.
    void fire()
    {
        auto const lock = std::scoped_lock { _mutex };
        _fired = true;
        if (_onSignal)
            _onSignal();
    }

    /// @return What the watching daemon fulfils once its last write is done.
    [[nodiscard]] std::shared_future<void> lastWrite()
    {
        auto const lock = std::scoped_lock { _mutex };
        return _lastWrite;
    }

  private:
    class ManualWatch final: public Watch
    {
      public:
        explicit ManualWatch(ManualShutdownSignals& source): _source { source } {}
        ~ManualWatch() override
        {
            auto const lock = std::scoped_lock { _source._mutex };
            _source._onSignal = nullptr;
        }

      private:
        ManualShutdownSignals& _source;
    };

    std::mutex _mutex;
    std::function<void()> _onSignal;
    std::shared_future<void> _lastWrite;
    bool _fired = false; ///< Whether fire() was called, so a watch started later fires at once.
};

/// Fires @p signals on destruction: declared after the daemon's future, it stops the daemon before the
/// future's destructor waits for it, on every path out of the test.
struct StopOnExit
{
    ManualShutdownSignals& signals;
    explicit StopOnExit(ManualShutdownSignals& source): signals { source } {}
    StopOnExit(StopOnExit const&) = delete;
    StopOnExit& operator=(StopOnExit const&) = delete;
    StopOnExit(StopOnExit&&) = delete;
    StopOnExit& operator=(StopOnExit&&) = delete;
    ~StopOnExit() { signals.fire(); }
};

/// Waits for a daemon to accept on @p socketPath, for up to five seconds.
[[nodiscard]] bool awaitAccepting(core::net::EventLoop& loop, std::string const& socketPath)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 500))
    {
        if (loop.blockOn(core::net::connectUnix(&loop, socketPath)).has_value())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    return false;
}

} // namespace

TEST_CASE("a StopDaemon ends runDaemon with a StopRequested shutdown", "[vthost][daemon][afunix]")
{
    auto const tmp = TempDir {};
    auto causes = std::vector<vthost::ShutdownCause> {};
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    // Signals stay the test process's own: the daemon's sigwait thread would take this whole process down.
    // The manual source stands in, fired only on the way out, where the StopDaemon has already stopped it.
    auto const signals = std::make_shared<ManualShutdownSignals>();
    config.shutdownSignals = signals;
    // Run on the daemon's loop thread; read here only once runDaemon has returned.
    config.beforeShutdown = [&causes](vthost::ShutdownCause cause) {
        causes.push_back(cause);
    };

    auto daemon = std::async(std::launch::async, [&config] { return vthost::runDaemon(config); });
    auto const stopOnExit = StopOnExit { *signals };

    auto const nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, nativePath));

    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(
        request, 1, vthost::proto::ClientHello { .role = vthost::proto::ConnectionRole::Control });
    vthost::proto::encodePdu(request, 2, vthost::proto::StopDaemon { .reason = 0 });
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(
        converseAsControl(&loop, nativePath, { request.view().begin(), request.view().end() }, &received));

    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
    CHECK(causes == std::vector { vthost::ShutdownCause::StopRequested });
    CHECK_FALSE(fs::exists(nativePath));

    // The daemon offers what it serves: a Control connection is answered, not refused.
    REQUIRE_FALSE(received.empty());
    auto const* const hello = std::get_if<vthost::proto::ServerHello>(&received.front().pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->capabilities.contains(vthost::proto::Capability::DaemonControl));
    CHECK(hello->capabilities.contains(vthost::proto::Capability::ExtensionPermissions));
}

namespace
{

/// Blocks SIGINT and SIGTERM on the calling thread for its lifetime, restoring the previous mask after.
class BlockedShutdownSignals
{
  public:
    BlockedShutdownSignals()
    {
        auto const blocked = shutdownSignalSet();
        pthread_sigmask(SIG_BLOCK, &blocked, &_previous);
    }
    ~BlockedShutdownSignals() { pthread_sigmask(SIG_SETMASK, &_previous, nullptr); }
    BlockedShutdownSignals(BlockedShutdownSignals const&) = delete;
    BlockedShutdownSignals& operator=(BlockedShutdownSignals const&) = delete;
    BlockedShutdownSignals(BlockedShutdownSignals&&) = delete;
    BlockedShutdownSignals& operator=(BlockedShutdownSignals&&) = delete;

    /// @return SIGINT and SIGTERM.
    [[nodiscard]] static sigset_t shutdownSignalSet() noexcept
    {
        auto signals = sigset_t {};
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        return signals;
    }

  private:
    sigset_t _previous {};
};

/// Sends the process a SIGTERM on destruction unless @p daemon has already returned: what stops a daemon
/// watching the process's real signals whose test failed before it could.
struct TerminateOnExit
{
    std::future<int>& daemon;
    explicit TerminateOnExit(std::future<int>& future): daemon { future } {}
    TerminateOnExit(TerminateOnExit const&) = delete;
    TerminateOnExit& operator=(TerminateOnExit const&) = delete;
    TerminateOnExit(TerminateOnExit&&) = delete;
    TerminateOnExit& operator=(TerminateOnExit&&) = delete;
    ~TerminateOnExit()
    {
        if (daemon.valid() && daemon.wait_for(std::chrono::seconds { 0 }) != std::future_status::ready)
            std::ignore = ::kill(::getpid(), SIGTERM);
    }
};

} // namespace

TEST_CASE("a StopDaemon ends a daemon with a startup layout cleanly under the process's signals",
          "[vthost][daemon][afunix]")
{
    // The daemon watches the process's real SIGINT/SIGTERM, as `contour daemon` does: its pump thread for
    // the startup tab must inherit the watch's mask, or the SIGTERM ~SigwaitWatch sends itself after the
    // StopDaemon lands on that thread and kills the process (here: Catch2 reports the fatal signal).
    //
    // This test thread blocks both signals, so the only threads able to take one are the daemon's own;
    // the daemon's thread unblocks them again, starting out as a real daemon's main thread does.
    auto const blocked = BlockedShutdownSignals {};
    auto const tmp = TempDir {};
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    config.shell.program = "/bin/sh";
    config.escapeSandbox = false;
    config.startupLayout = vtworkspace::Layout { .tabs = { vtworkspace::LayoutTab {} } };
    config.shutdownSignals = vthost::processShutdownSignals();

    auto daemon = std::async(std::launch::async, [&config] {
        auto const signals = BlockedShutdownSignals::shutdownSignalSet();
        pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
        return vthost::runDaemon(config);
    });
    auto const terminateOnExit = TerminateOnExit { daemon };

    auto const nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, nativePath));

    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(
        request, 1, vthost::proto::ClientHello { .role = vthost::proto::ConnectionRole::Control });
    vthost::proto::encodePdu(request, 2, vthost::proto::DaemonStatusRequest {});
    vthost::proto::encodePdu(request, 3, vthost::proto::StopDaemon { .reason = 0 });
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(
        converseAsControl(&loop, nativePath, { request.view().begin(), request.view().end() }, &received));

    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
    CHECK_FALSE(fs::exists(config.socketPath));
    CHECK_FALSE(fs::exists(nativePath));

    // The startup tab really was realized, so a pump thread existed when the daemon stopped.
    auto const status = std::ranges::find_if(received, [](auto const& frame) {
        return std::holds_alternative<vthost::proto::DaemonStatusReply>(frame.pdu);
    });
    REQUIRE(status != received.end());
    CHECK(std::get<vthost::proto::DaemonStatusReply>(status->pdu).sessions == 1);
}
#endif

#ifndef _WIN32
TEST_CASE("a daemon's socket directory is made owner-only before anything else creates it",
          "[vthost][daemon]")
{
    // The default log file lies beside the socket, and a log sink creating that directory gives it the
    // umask's 0755, which the listener then refuses. Prepared first, it is the listener's own 0700.
    auto const tmp = TempDir {};
    auto const socket = tmp.path / "control";
    REQUIRE(vthost::prepareSocketDirectory(socket).has_value());
    CHECK(fs::status(tmp.path).permissions() == fs::perms::owner_all);
    CHECK(vthost::prepareSocketDirectory(socket).has_value()); // and again, once it exists

    fs::permissions(tmp.path, fs::perms::owner_all | fs::perms::others_read | fs::perms::others_exec);
    auto const refused = vthost::prepareSocketDirectory(socket);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("world-accessible"));

    CHECK(vthost::prepareSocketDirectory("control").has_value()); // the current directory is the caller's
}
#endif

TEST_CASE("ensureDaemon: an unreachable socket fails once the timeout elapses", "[vthost][daemon]")
{
    // Nothing is bound, so the probe must report the daemon down. The spawn that
    // follows cannot succeed (no such binary), and the zero timeout ends the poll
    // immediately — so this also pins that a failed spawn cannot report success.
    auto const tmp = TempDir {};
    auto const endpoint =
        vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = tmp.path / "control" } };
    auto placement = vtpty::NoPlacement {};
    auto binary = vthost::InstalledDaemonBinary { "contour-no-such-binary" };
    REQUIRE(vthost::ensureDaemon(endpoint, binary, placement, {}, std::chrono::seconds { 0 })
            == EXIT_FAILURE);
}

// ---------------------------------------------------------------------------
// The spawn command line, which both platforms' spawn paths share.

TEST_CASE("daemonSpawnArgs spells the daemon verb and its socket", "[vthost][spawn]")
{
    auto const args = vthost::daemonSpawnArgs(
        "/usr/bin/contour", fs::path { "/run/user/1000/mux" }, {}, vthost::DaemonLifecycle::Persistent);

    REQUIRE(args.size() >= 3);
    CHECK(args[0] == "/usr/bin/contour");
    CHECK(args[1] == "daemon");
    CHECK(args[2] == "--socket=/run/user/1000/mux");
}

TEST_CASE("a registration's socket is read back out of the command line its backend reports",
          "[vthost][spawn]")
{
    using vthost::registeredSocketPath;

    SECTION("as the Task Scheduler joins it, quoted where the path has a blank")
    {
        auto const socket = fs::path { "/Users/A B/contour/work" };
        auto const args =
            vthost::daemonSpawnArgs("/opt/contour", socket, {}, vthost::DaemonLifecycle::Persistent);
        CHECK(registeredSocketPath(vthost::joinCommandLine(args)) == socket);
    }
    SECTION("as the Task Scheduler quotes a Windows path, whose backslashes escape nothing")
    {
        auto const args =
            std::vector<std::string> { "C:/contour.exe", "daemon", R"(--socket=C:\Users\A B\mux)" };
        CHECK(registeredSocketPath(vthost::joinCommandLine(args)) == fs::path { R"(C:\Users\A B\mux)" });
    }
    SECTION("as the Task Scheduler joins it, bare where nothing needs quoting")
    {
        auto const args = vthost::daemonSpawnArgs(
            "/opt/contour", fs::path { "/run/contour/work" }, {}, vthost::DaemonLifecycle::Persistent);
        CHECK(registeredSocketPath(vthost::joinCommandLine(args)) == fs::path { "/run/contour/work" });
    }
    SECTION("as a systemd unit quotes every argument")
    {
        CHECK(registeredSocketPath(
                  R"("/usr/bin/contour" "daemon" "--socket=/run/user/1000/a b" "--lifecycle=persistent")")
              == fs::path { "/run/user/1000/a b" });
    }
    SECTION("as launchd joins it, unquoted")
    {
        CHECK(registeredSocketPath("/Applications/Contour.app/Contents/MacOS/contour daemon --socket=/tmp/x")
              == fs::path { "/tmp/x" });
    }
    SECTION("absent: the daemon serves on its label's default socket")
    {
        CHECK(registeredSocketPath(R"("/usr/bin/contour" "daemon")") == std::nullopt);
        CHECK(registeredSocketPath("") == std::nullopt);
        CHECK(registeredSocketPath("contour daemon --socket=") == std::nullopt);
    }
}

TEST_CASE("daemonSpawnArgs spells the lifecycle as --lifecycle=<name>", "[vthost][spawn]")
{
    auto const transient =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::ExitWhenEmpty);
    CHECK(std::ranges::contains(transient, "--lifecycle=exit-with-last-session"));

    auto const onDemand =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::ExitWhenIdle);
    CHECK(std::ranges::contains(onDemand, "--lifecycle=exit-when-idle"));

    for (auto const lifecycle: { vthost::DaemonLifecycle::Persistent,
                                 vthost::DaemonLifecycle::ExitWhenEmpty,
                                 vthost::DaemonLifecycle::ExitWhenIdle })
    {
        auto const args = vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, lifecycle);
        CHECK_FALSE(std::ranges::contains(args, "--exit-with-last-session"));
        CHECK(std::ranges::contains(args, std::format("--lifecycle={}", vthost::nameOf(lifecycle))));
    }
}

TEST_CASE("daemonSpawnArgs always names a log file", "[vthost][spawn]")
{
    auto const bare =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    CHECK_FALSE(std::ranges::any_of(bare, [](auto const& a) { return a.starts_with("--log="); }));
    CHECK(std::ranges::contains(bare, "--log-file=/tmp/s.log"));

    auto const logged =
        vthost::daemonSpawnArgs("contour",
                                fs::path { "/tmp/s" },
                                vthost::DaemonSpawnOptions { .filter = "vthost.*", .logFile = "/tmp/d.log" },
                                vthost::DaemonLifecycle::Persistent);
    CHECK(std::ranges::contains(logged, "--log=vthost.*"));
    CHECK(std::ranges::contains(logged, "--log-file=/tmp/d.log"));
}

TEST_CASE("daemonSpawnArgs forwards the client's config and profile", "[vthost][spawn]")
{
    // A spawned daemon that reads a different configuration than the client that started it
    // hosts its terminals with a different scrollback depth — and the client then renders more
    // history than the daemon can name, which a resync truncates.
    auto const bare =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    CHECK_FALSE(std::ranges::any_of(bare, [](auto const& a) { return a.starts_with("--config"); }));
    CHECK_FALSE(std::ranges::any_of(bare, [](auto const& a) { return a.starts_with("--profile"); }));

    auto const configured = vthost::daemonSpawnArgs(
        "contour",
        fs::path { "/tmp/s" },
        vthost::DaemonSpawnOptions { .configPath = "/etc/contour.yml", .profileName = "work" },
        vthost::DaemonLifecycle::Persistent);
    CHECK(std::ranges::contains(configured, "--config=/etc/contour.yml"));
    CHECK(std::ranges::contains(configured, "--profile=work"));
}

TEST_CASE("daemonSpawnArgs forwards the client-area size policy", "[vthost][spawn]")
{
    // The policy is a DAEMON setting, and the daemon most people run is the one their client
    // spawned -- so without this the option is reachable only by starting `contour daemon` by hand,
    // and everyone else silently gets `latest` whatever they asked for.
    auto const bare =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    // Stated even at the default, unlike the string options: absence is itself a value here, so a
    // reader of the process list should not have to know what the default was.
    CHECK(std::ranges::contains(bare, "--size-policy=latest"));

    auto const smallest = vthost::daemonSpawnArgs(
        "contour",
        fs::path { "/tmp/s" },
        vthost::DaemonSpawnOptions { .sizePolicy = vthost::ClientSizePolicy::Smallest },
        vthost::DaemonLifecycle::Persistent);
    CHECK(std::ranges::contains(smallest, "--size-policy=smallest"));

    // The spelling has to be one the daemon's own parser accepts, or the spawned daemon dies on
    // its command line -- which is exactly what a hand-written second spelling would risk.
    for (auto const& [name, policy]: vthost::ClientSizePolicyNames)
    {
        auto const args = vthost::daemonSpawnArgs("contour",
                                                  fs::path { "/tmp/s" },
                                                  vthost::DaemonSpawnOptions { .sizePolicy = policy },
                                                  vthost::DaemonLifecycle::Persistent);
        CHECK(std::ranges::contains(args, std::format("--size-policy={}", name)));
        CHECK(vthost::clientSizePolicyFrom(name) == policy);
    }
}

TEST_CASE("a daemon hosts sessions with scrollback by default", "[vthost][daemon]")
{
    // DaemonConfig used to inherit `vtbackend::Settings{}`'s LineCount(0) verbatim, so the daemon
    // retained no history at all — @see vthost::DefaultSessionHistoryLineCount for what that costs.
    auto const settings = vthost::defaultSessionSettings();
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(settings.historyLimits.capacity));
    CHECK(unbox(std::get<vtbackend::LineCount>(settings.historyLimits.capacity)) > 0);

    // And DaemonConfig itself, since that is what `contour daemon` starts from.
    auto const configured = vthost::DaemonConfig {}.settings.historyLimits.capacity;
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(configured));
    CHECK(std::get<vtbackend::LineCount>(configured)
          == std::get<vtbackend::LineCount>(settings.historyLimits.capacity));
}

TEST_CASE("DaemonConfig defaults to escaping the sandbox, matching the prior hardcoded behavior",
          "[vthost][daemon]")
{
    // makeShellPtyFactory used to hardcode escapeSandbox=true regardless of DaemonConfig, silently
    // ignoring a profile's `escape_sandbox: false`. A bare DaemonConfig{} (no profile resolved, e.g.
    // in tests that construct one directly) must still default to the behavior every hosted shell
    // had before this field existed.
    CHECK(vthost::DaemonConfig {}.escapeSandbox);
}

TEST_CASE("DaemonConfig defaults to an empty startup layout, matching the prior single-tab behavior",
          "[vthost][daemon]")
{
    // A bare DaemonConfig{} (no profile resolved, e.g. constructed directly in a test) must keep
    // behaving exactly as before this field existed: no configured layout, so SessionHost falls
    // back to its usual single default tab.
    CHECK(vthost::DaemonConfig {}.startupLayout.tabs.empty());
}

TEST_CASE("joinCommandLine quotes the program and the arguments that need it", "[vthost][spawn]")
{
    // The Windows spawn path used to quote only the socket VALUE, leaving argv[0] bare — which
    // breaks the default install location `C:\Program Files\contour\contour.exe`.
    auto const args = vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                              fs::path { R"(C:\Users\A B\mux)" },
                                              {},
                                              vthost::DaemonLifecycle::ExitWhenEmpty);

    auto const line = vthost::joinCommandLine(args);

    CHECK(line.starts_with(R"("C:\Program Files\contour\contour.exe" daemon )"));
    CHECK(line.contains(" --lifecycle=exit-with-last-session"));
    CHECK(vthost::joinCommandLine({}).empty());
}

TEST_CASE("joinCommandLine survives trailing backslashes and embedded quotes", "[vthost][spawn]")
{
    // A backslash immediately before the closing quote ESCAPES it under CommandLineToArgvW's rules,
    // so `--config=C:\Users\me\configs\` naively wrapped ends its own quoting and swallows the rest
    // of the command line into that single argument — the spawned daemon then fails to parse its
    // options (or reads the wrong config) and never binds, and `contour client` reports only a
    // timeout. The same string is reused for the Task Scheduler registration, so an installed
    // daemon would be broken persistently.
    auto const args = std::vector<std::string> {
        R"(C:\Program Files\contour\contour.exe)",
        "daemon",
        R"(--config=C:\Users\me\configs\)", // ends in ONE backslash
        R"(--log-file=D:\logs\\)",          // ends in TWO
        R"(--title=say "hi")",              // embedded quotes
        R"(--odd=trailing\"quote)",         // a backslash-quote pair mid-argument
        "--exit-with-last-session",
    };

    CHECK(vtpty::testing::splitWindowsCommandLine(vthost::joinCommandLine(args)) == args);
}

TEST_CASE("joinCommandLine round-trips the arguments a spawn actually builds", "[vthost][spawn]")
{
    auto const args = vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                              fs::path { R"(C:\Users\A B\mux\)" },
                                              vthost::DaemonSpawnOptions { .filter = "vthost.*",
                                                                           .logFile = R"(D:\logs\)",
                                                                           .configPath = R"(C:\cfg\)",
                                                                           .profileName = "work" },
                                              vthost::DaemonLifecycle::ExitWhenEmpty);
    CHECK(vtpty::testing::splitWindowsCommandLine(vthost::joinCommandLine(args)) == args);
}

#ifdef _WIN32
TEST_CASE("joinCommandLine reads back through CommandLineToArgvW as the argv it joined", "[vthost][spawn]")
{
    // CommandLineToArgvW() is what contour.exe reads its own command line with, and the task a
    // registration starts hands it the same: the reader that decides, measured rather than modelled.
    auto const args =
        vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                fs::path { R"(C:\Users\A B\mux\)" },
                                vthost::DaemonSpawnOptions { .filter = "vthost.*",
                                                             .logFile = R"(D:\logs dir\)",
                                                             .configPath = R"(C:\cfg\say "hi"\)",
                                                             .profileName = "" },
                                vthost::DaemonLifecycle::ExitWhenEmpty);
    auto const commandLine = core::toUtf16(vthost::joinCommandLine(args));
    REQUIRE(commandLine.has_value());

    auto count = 0;
    auto* const read = CommandLineToArgvW(commandLine->c_str(), &count);
    REQUIRE(read != nullptr);
    auto readBack = std::vector<std::string> {};
    for (wchar_t const* const argument: std::span { read, static_cast<std::size_t>(count) })
        readBack.push_back(core::toUtf8(argument));
    LocalFree(static_cast<HLOCAL>(read));

    CHECK(readBack == args);
}
#endif

TEST_CASE("workspaceFor discards only on a plain stop", "[vthost][daemon][snapshot]")
{
    using vthost::ShutdownCause;
    using vthost::ShutdownWorkspace;
    STATIC_CHECK(vthost::workspaceFor(ShutdownCause::Signal) == ShutdownWorkspace::Keep);
    STATIC_CHECK(vthost::workspaceFor(ShutdownCause::LastSessionClosed) == ShutdownWorkspace::Keep);
    STATIC_CHECK(vthost::workspaceFor(ShutdownCause::Idle) == ShutdownWorkspace::Keep);
    STATIC_CHECK(vthost::workspaceFor(ShutdownCause::StopRequested) == ShutdownWorkspace::Discard);
    STATIC_CHECK(vthost::workspaceFor(ShutdownCause::UpgradeRestart) == ShutdownWorkspace::Keep);
    STATIC_CHECK(vthost::workspaceFor(ShutdownCause::StopRequestedKeepingWorkspace)
                 == ShutdownWorkspace::Keep);
}

TEST_CASE("DaemonConfig persists nothing unless a snapshot path is set", "[vthost][daemon]")
{
    CHECK(vthost::DaemonConfig {}.persistence.snapshotPath.empty());
    CHECK(vthost::DaemonConfig {}.persistence.rerunPrograms.empty());
    CHECK(vthost::DaemonConfig {}.persistence.interval == std::chrono::seconds { 30 });
}

namespace
{

constexpr auto RestartSnapshot = std::string_view { "/state/contour/sessions/default.snapshot" };

/// Two daemons' lives over one in-memory snapshot file, without their sockets: what runDaemon() wires
/// around its host, with the writer inline and every session a MockPty.
struct RestartHarness
{
    core::platform::ManualClock clock {};
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend, clock };
    core::platform::ManualWallClock wallClock { std::chrono::system_clock::time_point {
        std::chrono::seconds { 1'761'000'000 } } };
    vthost::testing::RecordingDurableFiles durability {};
    core::platform::testing::InMemoryFileSystem& files { durability.files };
    vthost::testing::RecordingFilePrivacy privacy { files };
    vthost::SessionSnapshotStore store { durability, privacy, fs::path { RestartSnapshot } };
    vthost::WorkspacePersistenceConfig config { .snapshotPath = fs::path { RestartSnapshot } };
    std::vector<std::string> lines {}; ///< What the persistence reported.

    /// The daemon's startup layout: one plain tab.
    vtworkspace::Layout startup { .tabs = { vtworkspace::LayoutTab {} } };

    [[nodiscard]] vthost::DiagnosticSink sink()
    {
        return [this](std::string_view line) {
            lines.emplace_back(line);
        };
    }

    /// @return A host as runDaemon() builds it: the restore read from the file, else the startup layout.
    [[nodiscard]] std::unique_ptr<vthost::SessionHost> startHost(
        std::optional<vthost::WorkspaceRestore> restore)
    {
        return std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            vtbackend::Settings {},
            core::defaultEnvironment(),
            vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                        .startupLayout = startup,
                                        .restore = std::move(restore) });
    }

    /// The first daemon: a split tab and a plain one, ended by @p cause.
    void runFirstDaemon(vthost::ShutdownCause cause)
    {
        auto host = startHost(std::nullopt);
        auto* const tab = host->createTab();
        REQUIRE(tab != nullptr);
        host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical);
        REQUIRE(tab->paneCount() == 2);
        auto persistence = vthost::DaemonPersistence {
            loop, *host, store, std::nullopt, config, wallClock, vthost::WriterThreading::Inline, sink()
        };
        persistence.finalize(cause);
    }

    /// @return What the second daemon's host holds once it has started.
    [[nodiscard]] vthost::WorkspaceSnapshot runSecondDaemon()
    {
        auto restore = vthost::prepareRestore(store, config, sink(), wallClock);
        auto const restoredFrom =
            restore.transform([](vthost::WorkspaceRestore const& restored) { return restored.source; });
        auto host = startHost(std::move(restore));
        auto const persistence = vthost::DaemonPersistence {
            loop, *host, store, restoredFrom, config, wallClock, vthost::WriterThreading::Inline, sink()
        };
        return vthost::captureWorkspace(*host, {}, std::chrono::sys_seconds {});
    }
};

[[nodiscard]] std::size_t paneCount(vthost::PaneNode const& node)
{
    if (node.children.empty())
        return 1;
    return paneCount(node.children[0]) + paneCount(node.children[1]);
}

} // namespace

TEST_CASE("what one host keeps at shutdown, the next one restores", "[vthost][daemon][snapshot]")
{
    auto h = RestartHarness {};
    h.runFirstDaemon(vthost::ShutdownCause::UpgradeRestart);

    auto const restored = h.runSecondDaemon();
    REQUIRE(restored.windows.size() == 1);
    // The startup tab, then the split one: what the first daemon held, not a startup layout beside it.
    REQUIRE(restored.windows[0].tabs.size() == 2);
    CHECK(paneCount(restored.windows[0].tabs[0].root) == 1);
    CHECK(paneCount(restored.windows[0].tabs[1].root) == 2);
    CHECK(std::ranges::any_of(h.lines, [](std::string const& line) {
        return line.contains("restoring 1 window(s) from") && line.contains(RestartSnapshot);
    }));
}

TEST_CASE("what one host discards at shutdown, the next one does not restore", "[vthost][daemon][snapshot]")
{
    auto h = RestartHarness {};
    h.runFirstDaemon(vthost::ShutdownCause::StopRequested);

    REQUIRE_FALSE(vthost::prepareRestore(h.store, h.config, h.sink(), h.wallClock).has_value());
    auto const started = h.runSecondDaemon();
    REQUIRE(started.windows.size() == 1);
    REQUIRE(started.windows[0].tabs.size() == 1); // the startup layout
    CHECK(paneCount(started.windows[0].tabs[0].root) == 1);
}

#ifndef _WIN32
TEST_CASE("a daemon that keeps its workspace offers the restore and writes it as it stops",
          "[vthost][daemon][snapshot][afunix]")
{
    auto const tmp = TempDir {};
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    config.shell.program = "/bin/sh";
    config.escapeSandbox = false;
    config.startupLayout = vtworkspace::Layout { .tabs = { vtworkspace::LayoutTab {} } };
    config.persistence.snapshotPath = tmp.path / "state" / "sessions" / "default.snapshot";
    auto const signals = std::make_shared<ManualShutdownSignals>();
    config.shutdownSignals = signals;

    // UpgradeRestart keeps the workspace, a plain stop empties the file.
    auto const [reason, verdict] = GENERATE(table<std::uint8_t, vthost::LoadVerdict>({
        { std::uint8_t { 1 }, vthost::LoadVerdict::Restorable },
        { std::uint8_t { 0 }, vthost::LoadVerdict::Empty },
    }));

    auto daemon = std::async(std::launch::async, [&config] { return vthost::runDaemon(config); });
    auto const stopOnExit = StopOnExit { *signals };

    auto const nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, nativePath));

    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(
        request, 1, vthost::proto::ClientHello { .role = vthost::proto::ConnectionRole::Control });
    vthost::proto::encodePdu(request, 2, vthost::proto::StopDaemon { .reason = reason });
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(
        converseAsControl(&loop, nativePath, { request.view().begin(), request.view().end() }, &received));

    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);

    REQUIRE_FALSE(received.empty());
    auto const* const hello = std::get_if<vthost::proto::ServerHello>(&received.front().pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->capabilities.contains(vthost::proto::Capability::WorkspaceRestore));

    // The last write was done before the daemon let a console-control handler return.
    CHECK(signals->lastWrite().wait_for(std::chrono::seconds { 0 }) == std::future_status::ready);
    auto const store = vthost::SessionSnapshotStore { core::platform::NativeFileSystem::instance(),
                                                      vthost::NativeFilePrivacy::instance(),
                                                      config.persistence.snapshotPath };
    auto const loaded = store.load();
    CHECK(loaded.verdict == verdict);
    CHECK((fs::status(config.persistence.snapshotPath).permissions() & fs::perms::all)
          == (fs::perms::owner_read | fs::perms::owner_write));
}

TEST_CASE("a daemon without persistence offers no restore", "[vthost][daemon][afunix]")
{
    auto const tmp = TempDir {};
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    auto const signals = std::make_shared<ManualShutdownSignals>();
    config.shutdownSignals = signals;

    auto daemon = std::async(std::launch::async, [&config] { return vthost::runDaemon(config); });
    auto const stopOnExit = StopOnExit { *signals };

    auto const nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, nativePath));

    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(
        request, 1, vthost::proto::ClientHello { .role = vthost::proto::ConnectionRole::Control });
    vthost::proto::encodePdu(request, 2, vthost::proto::StopDaemon { .reason = 0 });
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(
        converseAsControl(&loop, nativePath, { request.view().begin(), request.view().end() }, &received));

    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
    REQUIRE_FALSE(received.empty());
    auto const* const hello = std::get_if<vthost::proto::ServerHello>(&received.front().pdu);
    REQUIRE(hello != nullptr);
    CHECK_FALSE(hello->capabilities.contains(vthost::proto::Capability::WorkspaceRestore));
}
#endif

#ifndef _WIN32
namespace
{

/// An Attach client's conversation: sends @p request and collects every frame until one @p done accepts
/// arrives, the daemon hangs up, or ten seconds pass without a byte.
core::async::Task<void> attachUntil(core::net::EventLoop* loop,
                                    std::string socketPath,
                                    std::vector<std::byte> request,
                                    std::vector<vthost::proto::DecodedFrame>* received,
                                    std::function<bool(vthost::proto::DecodedPdu const&)> done)
{
    auto connected = co_await core::net::connectUnix(loop, socketPath);
    REQUIRE(connected.has_value());
    auto const socket = std::move(*connected);
    socket->setReceiveDeadline(std::chrono::seconds { 10 });
    auto const written = co_await socket->write(request);
    REQUIRE(written.has_value());

    auto pending = std::vector<std::byte> {};
    auto buffer = std::vector<std::byte>(4096);
    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return;
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
        while (auto frame = vthost::proto::decodePdu(pending))
        {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(frame->consumed));
            auto const isLast = done(frame->pdu);
            received->push_back(*std::move(frame));
            if (isLast)
                co_return;
        }
    }
}

/// An Attach client's conversation up to its first layout (@see attachUntil).
core::async::Task<void> attachUntilLayout(core::net::EventLoop* loop,
                                          std::string socketPath,
                                          std::vector<std::byte> request,
                                          std::vector<vthost::proto::DecodedFrame>* received)
{
    co_await attachUntil(
        loop, std::move(socketPath), std::move(request), received, [](vthost::proto::DecodedPdu const& pdu) {
            return std::holds_alternative<vthost::proto::LayoutState>(pdu);
        });
}

/// A TLS client's handshake with a daemon's native TCP listener, trusting @p clientTls alone: sends an Attach
/// hello with @p token and reports whether the daemon answered it.
core::async::Task<void> helloOverTls(core::net::EventLoop* loop,
                                     std::uint16_t port,
                                     std::shared_ptr<core::net::ITlsContext> clientTls,
                                     std::string token,
                                     bool* answered)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", port);
    REQUIRE(connected.has_value());
    auto const socket = clientTls->wrap(std::move(*connected), *loop);
    socket->setReceiveDeadline(std::chrono::seconds { 10 });
    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(request,
                             1,
                             vthost::proto::ClientHello { .token = std::move(token),
                                                          .role = vthost::proto::ConnectionRole::Attach });
    auto const bytes = std::vector<std::byte> { request.view().begin(), request.view().end() };
    if (!(co_await socket->write(bytes)).has_value())
        co_return;
    auto pending = std::vector<std::byte> {};
    auto buffer = std::vector<std::byte>(4096);
    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return;
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
        if (auto frame = vthost::proto::decodePdu(pending))
        {
            *answered = std::holds_alternative<vthost::proto::ServerHello>(frame->pdu);
            co_return;
        }
    }
}

/// @return A TCP listening socket on loopback, on a port the system picks, as the previous image of a daemon
///         left it bound: a descriptor and nothing else.
/// @param port Set to the port it is bound to.
[[nodiscard]] int rawTcpListener(std::uint16_t& port)
{
    // Close-on-exec set after the fact: macOS has no SOCK_CLOEXEC.
    auto const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(fd, 8) == 0);
    auto length = static_cast<socklen_t>(sizeof(address));
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    port = ntohs(address.sin_port);
    return fd;
}

/// @return A listening socket at @p path, as the previous image of a daemon left it bound: a descriptor and
///         nothing else.
[[nodiscard]] int rawListener(std::string const& path)
{
    // Close-on-exec set after the fact: macOS has no SOCK_CLOEXEC.
    auto const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    REQUIRE(path.size() < sizeof(address.sun_path));
    std::ranges::copy(path, std::begin(address.sun_path));
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(fd, 8) == 0);
    return fd;
}

/// The instance the image a test hands over names.
constexpr auto HandedOverInstance = uint64_t { 0xFEEDFACE };

/// A daemon about to start from a handover image: a host with two sessions in one tab, captured with a native
/// listener of its own, the host gone as the exec would leave it.
struct HandedOverDaemon
{
    TempDir tmp;
    vthost::DaemonConfig config;
    /// What the previous image handed over; each test hands runDaemon() a copy, and asks it about the
    /// children.
    vthost::handover::HandoverImage image;
    std::string nativePath;
    std::vector<vtworkspace::SessionId> sessions;
    std::vector<std::optional<vtcontrol::Uuid>> uuids;
    std::shared_ptr<ManualShutdownSignals> signals = std::make_shared<ManualShutdownSignals>();

    HandedOverDaemon()
    {
        config.socketPath = tmp.path / "control";
        nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
        fs::create_directories(tmp.path);
        fs::permissions(tmp.path, fs::perms::owner_all, fs::perm_options::replace);
        config.shutdownSignals = signals;

        auto old = std::make_unique<vthost::testing::LiveHost>(
            [](vtpty::PageSize size) { return vthost::testing::shellProcess(size); });
        old->spawnTwo();
        sessions = old->host.sessionIds();
        REQUIRE(old->eventually([&] {
            return std::ranges::all_of(
                sessions, [&](vtworkspace::SessionId id) { return old->pageText(id).contains("ready"); });
        }));
        for (auto const id: sessions)
            uuids.push_back(old->host.objects().uuidOf(id));
        old->settle();
        auto image = old->capture(
            { { .name = "native", .handle = vtpty::CrossingHandle::of(rawListener(nativePath)) } },
            vtpty::CrossingHandle {},
            HandedOverInstance);
        REQUIRE(image.has_value());
        vthost::testing::releaseForAdoption(old->host);
        old.reset();
        this->image = *std::move(image);
    }
};

/// @return Every leaf session @p pane carries, in tree order.
void collectSessions(vthost::proto::WirePane const& pane, std::vector<uint64_t>& sessions)
{
    if (!pane.isSplit())
        sessions.push_back(pane.session);
    for (auto const& child: pane.children)
        collectSessions(child, sessions);
}

} // namespace

TEST_CASE("runDaemon from a handover image keeps instance, ids and sessions",
          "[vthost][daemon][handover][afunix][threading]")
{
    auto d = HandedOverDaemon {};
    auto daemon = std::async(std::launch::async, [&d] { return vthost::runDaemon(d.config, d.image); });
    auto const stopOnExit = StopOnExit { *d.signals };

    // The inherited listener serves at once: no bind, no gap.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, d.nativePath));

    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(
        request, 1, vthost::proto::ClientHello { .role = vthost::proto::ConnectionRole::Attach });
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(
        attachUntilLayout(&loop, d.nativePath, { request.view().begin(), request.view().end() }, &received));

    // The same run of the daemon, by the instance a reconnecting client tells it by.
    REQUIRE_FALSE(received.empty());
    auto const* const hello = std::get_if<vthost::proto::ServerHello>(&received.front().pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->daemonInstance == HandedOverInstance);

    // The same panes, under the same session ids.
    auto const* const layout = std::get_if<vthost::proto::LayoutState>(&received.back().pdu);
    REQUIRE(layout != nullptr);
    REQUIRE(layout->tabs.size() == 1);
    auto shown = std::vector<uint64_t> {};
    collectSessions(layout->tabs.front().root, shown);
    std::ranges::sort(shown);
    CHECK(shown
          == std::ranges::to<std::vector>(
              d.sessions | std::views::transform([](vtworkspace::SessionId id) { return id.value; })));
    // Stopped as it would be any day: the adopted sessions and listeners end cleanly.
    d.signals->fire();
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
}
TEST_CASE("a daemon taken over from a handover serves its programs on whatever endpoints it can",
          "[vthost][daemon][handover][afunix][threading]")
{
    auto d = HandedOverDaemon {};
    // Taken by something else, so that neither can be bound; the image hands over neither.
    auto const controlTaken = rawListener(core::pathToUtf8(d.config.socketPath));
    auto const imsgTaken = rawListener(core::pathToUtf8(vthost::tmuxSocketPath(d.config.socketPath)));
    // A TCP listener whose certificate is gone: its TLS cannot be built.
    d.config.nativeTcp = vthost::NativeTcpListenerConfig {
        .token = "tok",
        .tlsCertPath = core::pathToUtf8(d.tmp.path / "gone-cert.pem"),
        .tlsKeyPath = core::pathToUtf8(d.tmp.path / "gone-key.pem"),
    };
    // What the composition root could not load.
    d.config.degradations = { "the configuration did not load" };
    auto daemon = std::async(std::launch::async, [&d] { return vthost::runDaemon(d.config, d.image); });
    auto const stopOnExit = StopOnExit { *d.signals };

    // Serving on the inherited native listener, every program alive.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, d.nativePath));
    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(
        request, 1, vthost::proto::ClientHello { .role = vthost::proto::ConnectionRole::Attach });
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    auto notices = std::vector<std::string> {};
    loop.blockOn(attachUntil(&loop,
                             d.nativePath,
                             { request.view().begin(), request.view().end() },
                             &received,
                             [&notices](vthost::proto::DecodedPdu const& pdu) {
                                 if (auto const* notify = std::get_if<vthost::proto::SessionNotify>(&pdu))
                                     notices.push_back(notify->body);
                                 return notices.size() == 4;
                             }));
    REQUIRE_FALSE(received.empty());
    CHECK(std::holds_alternative<vthost::proto::ServerHello>(received.front().pdu));
    for (auto const& session: d.image.sessions)
        CHECK(::kill(static_cast<pid_t>(session.child.pid), 0) == 0);

    // Each thing it serves on without is told to the client, once.
    REQUIRE(notices.size() == 4);
    CHECK(notices[0] == "the configuration did not load");
    CHECK(std::ranges::count_if(notices, [](std::string const& notice) { return notice.contains("control"); })
          == 1);
    CHECK(std::ranges::count_if(notices, [](std::string const& notice) { return notice.contains("imsg"); })
          == 1);
    CHECK(std::ranges::count_if(notices,
                                [](std::string const& notice) { return notice.contains("native-tcp"); })
          == 1);

    d.signals->fire();
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
    ::close(controlTaken);
    ::close(imsgTaken);
}

TEST_CASE("a daemon taken over from a handover that can serve no endpoint at all keeps its programs alive",
          "[vthost][daemon][handover][afunix][threading]")
{
    auto d = HandedOverDaemon {};
    // The image hands over no listener, and every path is taken: the native one by the listener it named.
    auto const nativeTaken = d.image.listeners.front().handle.native();
    d.image.listeners.clear();
    auto const controlTaken = rawListener(core::pathToUtf8(d.config.socketPath));
    auto const imsgTaken = rawListener(core::pathToUtf8(vthost::tmuxSocketPath(d.config.socketPath)));
    auto daemon = std::async(std::launch::async, [&d] { return vthost::runDaemon(d.config, d.image); });
    auto const stopOnExit = StopOnExit { *d.signals };

    // It runs on, unreachable, rather than hanging its programs up.
    CHECK(daemon.wait_for(std::chrono::seconds { 1 }) == std::future_status::timeout);
    for (auto const& session: d.image.sessions)
        CHECK(::kill(static_cast<pid_t>(session.child.pid), 0) == 0);

    // Until it is asked to end.
    d.signals->fire();
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
    ::close(nativeTaken);
    ::close(controlTaken);
    ::close(imsgTaken);
}

TEST_CASE("a daemon that starts afresh still refuses an endpoint it cannot bind", "[vthost][daemon][afunix]")
{
    auto const tmp = TempDir {};
    fs::create_directories(tmp.path);
    fs::permissions(tmp.path, fs::perms::owner_all, fs::perm_options::replace);
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    config.shutdownSignals = std::make_shared<ManualShutdownSignals>();
    auto const imsgTaken = rawListener(core::pathToUtf8(vthost::tmuxSocketPath(config.socketPath)));
    CHECK(vthost::runDaemon(config) == EXIT_FAILURE);
    ::close(imsgTaken);
}

TEST_CASE("a daemon taken over from a handover presents the TLS certificate it made up before",
          "[vthost][daemon][handover][threading]")
{
    auto d = HandedOverDaemon {};
    auto const identity = core::net::generateSelfSignedCertificate({ .commonName = "contour-daemon" });
    REQUIRE(identity.has_value());
    auto port = std::uint16_t { 0 };
    d.image.listeners.push_back(vthost::handover::InheritedListener {
        .name = "native-tcp",
        .handle = vtpty::CrossingHandle::of(rawTcpListener(port)),
        .tlsIdentity =
            vthost::handover::TlsIdentity { .certPem = identity->certPem, .keyPem = identity->keyPem },
    });
    d.config.nativeTcp = vthost::NativeTcpListenerConfig {};
    d.config.nativeTcp->port = port;
    d.config.nativeTcp->token = "tok";
    auto daemon = std::async(std::launch::async, [&d] { return vthost::runDaemon(d.config, d.image); });
    auto const stopOnExit = StopOnExit { *d.signals };

    // A client that pinned the certificate it trusted on first use: only that very one passes.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, d.nativePath));
    auto const pinned = core::net::makeTlsClientContext(identity->certPem);
    REQUIRE(pinned.has_value());
    auto answered = false;
    loop.blockOn(helloOverTls(&loop, port, *pinned, "tok", &answered));
    CHECK(answered);

    d.signals->fire();
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
}

TEST_CASE("a daemon that ends with its last session ends when a handover leaves it none",
          "[vthost][daemon][handover][afunix]")
{
    auto const tmp = TempDir {};
    fs::create_directories(tmp.path);
    fs::permissions(tmp.path, fs::perms::owner_all, fs::perm_options::replace);
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    auto const signals = std::make_shared<ManualShutdownSignals>();
    config.shutdownSignals = signals;
    config.lifecycle = vthost::DaemonLifecycle::ExitWhenEmpty;
    auto image = vthost::handover::HandoverImage {};
    image.listeners.push_back(vthost::handover::InheritedListener {
        .name = "native",
        .handle = vtpty::CrossingHandle::of(
            rawListener(core::pathToUtf8(vthost::nativeSocketPath(config.socketPath)))),
        .tlsIdentity = std::nullopt,
    });
    auto daemon = std::async(std::launch::async,
                             [&config, &image] { return vthost::runDaemon(config, std::move(image)); });
    auto const stopOnExit = StopOnExit { *signals };
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
}
#endif

#ifndef _WIN32
namespace
{

/// A daemon that upgrades itself in place, over fakes for everything that would leave the process: a packaged
/// install (`/usr/bin/contour`, running in place), a target that reads every format, an exec that fails as it
/// is asked, so that the daemon serves on, and memory that keeps the image in this process.
struct UpgradableDaemon
{
    /// @return The command line the daemon was started with, which the exec repeats.
    [[nodiscard]] static std::vector<std::string> commandLine()
    {
        return { "/usr/bin/contour", "daemon", "--label", "upgrade-test" };
    }

    TempDir tmp;
    vthost::testing::StagingFileSystem files;
    vthost::testing::FakeImageLeases leases;
    vthost::testing::FakeSymbolicLinks links;
    vthost::DaemonImageStore store {
        vthost::testing::linuxPackageStoreConfig("/state/bin", "0.9.1"), files, leases, links
    };
    vthost::testing::FakeProcessImageReplacer replacer;
    vthost::testing::FakeHandoverMemory memory;
    vthost::handover::ExecHandoff handoff { replacer, memory };
    vthost::testing::FakeReadinessProbe probe { "0.9.1" };
    /// The command line the exec was asked to run, fulfilled where the exec would replace the process.
    std::promise<std::vector<std::string>> replaced;
    std::shared_ptr<ManualShutdownSignals> signals = std::make_shared<ManualShutdownSignals>();
    vthost::DaemonConfig config;
    std::string nativePath;

    UpgradableDaemon()
    {
        config.socketPath = tmp.path / "control";
        config.shutdownSignals = signals;
        config.releaseVersion = "0.9.0";
        config.liveUpgrade.emplace(vthost::LiveUpgradeServices {
            .store = store,
            .leases = leases,
            .handoff = handoff,
            .probe = probe,
            .argv = commandLine(),
            .installedExecutable = "/usr/bin/contour",
            .sandbox = vtpty::SandboxState::Host,
        });
        nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
        // On the daemon's loop thread, where the exec would happen; the exec then fails as told.
        replacer.onReplace = [this] {
            replaced.set_value(replacer.replacements.back().second);
        };
    }
};

/// @return A Control client's hello that negotiates the live upgrade.
[[nodiscard]] vthost::proto::ClientHello upgradingControlHello()
{
    return vthost::proto::ClientHello {
        .capabilities = vthost::proto::CapabilitySet::of({ vthost::proto::Capability::LiveUpgrade }),
        .role = vthost::proto::ConnectionRole::Control,
    };
}

/// @return The bytes of @p pdus, numbered from 1.
[[nodiscard]] std::vector<std::byte> requestOf(std::vector<vthost::proto::DecodedPdu> const& pdus)
{
    auto request = vthost::proto::Writer {};
    auto serial = uint64_t { 0 };
    for (auto const& pdu: pdus)
        vthost::proto::encodePdu(request, ++serial, pdu);
    return { request.view().begin(), request.view().end() };
}

/// @return The frames of @p received that carry a @p Pdu, in order.
template <typename Pdu>
[[nodiscard]] std::vector<Pdu> framesOf(std::vector<vthost::proto::DecodedFrame> const& received)
{
    auto found = std::vector<Pdu> {};
    for (auto const& frame: received)
        if (auto const* const pdu = std::get_if<Pdu>(&frame.pdu))
            found.push_back(*pdu);
    return found;
}

/// How a tmux control-mode connection ended.
enum class TmuxEnd : uint8_t
{
    StillOpen = 0, ///< Nothing ended it before its receive deadline.
    Closed,        ///< The daemon closed it.
};

/// A tmux control-mode client that is served, then sees its connection go: connects to @p socketPath, waits
/// for the answer to a command, then, still connected, has a Control client converse with the daemon's native
/// endpoint (converseAsControl), and reads until the daemon closes the connection or ten seconds pass.
/// @param loop The client's loop.
/// @param socketPath The daemon's tmux control-mode socket.
/// @param nativePath The daemon's native socket.
/// @param request What the Control client sends.
/// @param received What the Control client receives.
/// @param end How the tmux connection ended.
core::async::Task<void> tmuxClientAcross(core::net::EventLoop* loop,
                                         std::string socketPath,
                                         std::string nativePath,
                                         std::vector<std::byte> request,
                                         std::vector<vthost::proto::DecodedFrame>* received,
                                         TmuxEnd* end)
{
    auto connected = co_await core::net::connectUnix(loop, socketPath);
    REQUIRE(connected.has_value());
    auto const socket = std::move(*connected);
    socket->setReceiveDeadline(std::chrono::seconds { 10 });
    auto const command = std::string_view { "list-sessions\n" };
    auto const written = co_await socket->write(std::as_bytes(std::span { command }));
    REQUIRE(written.has_value());

    // Served: the command's guard closes, so the daemon tracks the connection by now.
    auto text = std::string {};
    auto buffer = std::vector<std::byte>(4096);
    while (!text.contains("%end") && !text.contains("%error"))
    {
        auto const read = co_await socket->read(buffer);
        REQUIRE(read.has_value());
        REQUIRE(*read != 0);
        std::ranges::transform(buffer | std::views::take(*read), std::back_inserter(text), [](std::byte b) {
            return static_cast<char>(b);
        });
    }

    co_await converseAsControl(loop, std::move(nativePath), std::move(request), received);

    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value())
        {
            *end =
                read.error().code == core::net::NetErrorCode::Timeout ? TmuxEnd::StillOpen : TmuxEnd::Closed;
            co_return;
        }
        if (*read == 0)
        {
            *end = TmuxEnd::Closed;
            co_return;
        }
    }
}

} // namespace

TEST_CASE("a daemon upgrades itself in place when a local client asks",
          "[vthost][daemon][upgrade][afunix][threading]")
{
    auto d = UpgradableDaemon {};
    auto replaced = d.replaced.get_future();
    auto daemon = std::async(std::launch::async, [&d] { return vthost::runDaemon(d.config); });
    auto const stopOnExit = StopOnExit { *d.signals };

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, d.nativePath));

    // The requester is answered, told, and let go, as every client is.
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(converseAsControl(&loop,
                                   d.nativePath,
                                   requestOf({ upgradingControlHello(), vthost::proto::UpgradeDaemon {} }),
                                   &received));
    auto const hellos = framesOf<vthost::proto::ServerHello>(received);
    REQUIRE(hellos.size() == 1);
    CHECK(hellos.front().capabilities.contains(vthost::proto::Capability::LiveUpgrade));
    // Started; and, should the connection still be flushing when the (fake) exec fails, why it did.
    auto const replies = framesOf<vthost::proto::UpgradeDaemonReply>(received);
    REQUIRE_FALSE(replies.empty());
    REQUIRE(replies.size() <= 2);
    CHECK(replies.front().result == 0);
    CHECK(replies.back().result
          == (replies.size() == 1 ? 0 : vthost::proto::UpgradeDaemonReply::ExecFailedResult));
    auto const announced = framesOf<vthost::proto::DaemonUpgrading>(received);
    REQUIRE(announced.size() == 1);
    CHECK(announced.front().toRelease == "0.9.1");
    // Answered first, then told: the reply precedes the announcement on the wire.
    CHECK(std::holds_alternative<vthost::proto::UpgradeDaemonReply>(received.at(1).pdu));
    CHECK(std::holds_alternative<vthost::proto::DaemonUpgrading>(received.at(2).pdu));

    // The exec runs the same command line, the installed executable's, naming the handover.
    REQUIRE(replaced.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    auto const argv = replaced.get();
    auto const commandLine = UpgradableDaemon::commandLine();
    REQUIRE(argv.size() == commandLine.size() + 1);
    CHECK(std::ranges::equal(argv | std::views::take(commandLine.size()), commandLine));
    CHECK(argv.back().starts_with("--handover-fd="));

    // The exec failed: the daemon rolled back and serves on, and still stops as it would any day.
    auto after = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(converseAsControl(&loop,
                                   d.nativePath,
                                   requestOf({ upgradingControlHello(),
                                               vthost::proto::DaemonStatusRequest {},
                                               vthost::proto::StopDaemon { .reason = 0 } }),
                                   &after));
    CHECK(framesOf<vthost::proto::DaemonStatusReply>(after).size() == 1);
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
    REQUIRE(d.replacer.replacements.size() == 1);
    CHECK(d.replacer.replacements.front().first == "/usr/bin/contour");
    CHECK(d.probe.asked == std::vector<fs::path> { "/usr/bin/contour" });
}

TEST_CASE("a live upgrade closes a tmux control-mode connection",
          "[vthost][daemon][upgrade][afunix][threading]")
{
    auto d = UpgradableDaemon {};
    auto replaced = d.replaced.get_future();
    auto daemon = std::async(std::launch::async, [&d] { return vthost::runDaemon(d.config); });
    auto const stopOnExit = StopOnExit { *d.signals };

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, d.nativePath));

    auto received = std::vector<vthost::proto::DecodedFrame> {};
    auto end = TmuxEnd::StillOpen;
    loop.blockOn(tmuxClientAcross(&loop,
                                  core::pathToUtf8(d.config.socketPath),
                                  d.nativePath,
                                  requestOf({ upgradingControlHello(), vthost::proto::UpgradeDaemon {} }),
                                  &received,
                                  &end));
    CHECK(end == TmuxEnd::Closed);
    CHECK(framesOf<vthost::proto::DaemonUpgrading>(received).size() == 1);
    REQUIRE(replaced.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    // The exec failed: the daemon serves on until it is stopped.
    d.signals->fire();
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);
}

TEST_CASE("a daemon given nothing to upgrade with offers no live upgrade and refuses one",
          "[vthost][daemon][upgrade][afunix]")
{
    auto const tmp = TempDir {};
    auto config = vthost::DaemonConfig {};
    config.socketPath = tmp.path / "control";
    auto const signals = std::make_shared<ManualShutdownSignals>();
    config.shutdownSignals = signals;
    auto daemon = std::async(std::launch::async, [&config] { return vthost::runDaemon(config); });
    auto const stopOnExit = StopOnExit { *signals };

    auto const nativePath = core::pathToUtf8(vthost::nativeSocketPath(config.socketPath));
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    REQUIRE(awaitAccepting(loop, nativePath));
    auto received = std::vector<vthost::proto::DecodedFrame> {};
    loop.blockOn(converseAsControl(&loop,
                                   nativePath,
                                   requestOf({ upgradingControlHello(),
                                               vthost::proto::UpgradeDaemon {},
                                               vthost::proto::StopDaemon { .reason = 0 } }),
                                   &received));
    REQUIRE(daemon.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    CHECK(daemon.get() == EXIT_SUCCESS);

    auto const hellos = framesOf<vthost::proto::ServerHello>(received);
    REQUIRE(hellos.size() == 1);
    CHECK_FALSE(hellos.front().capabilities.contains(vthost::proto::Capability::LiveUpgrade));
    CHECK(framesOf<vthost::proto::UpgradeDaemonReply>(received).empty());
    CHECK(framesOf<vthost::proto::DaemonUpgrading>(received).empty());
}
#endif
