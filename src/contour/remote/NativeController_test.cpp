// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/HostTransport.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RoutingSessionFactory.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>

#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <contour/remote/RemoteLayout.hpp>

#include <core/async/Cancellation.hpp>
#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <cstdint>
#include <expected>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;

namespace
{

/// The timeline a test controller stamps, on a clock nothing advances.
struct TestTimeline
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} };
    vthost::latency::StartupTimeline timeline { clock, clock.now() };
};

/// Builds a controller as every test here does: no attach settings of its own, on the steady clock.
/// The one place the constructor is spelled, so a collaborator it gains is added here alone.
/// @param endpoint The daemon to attach to, over TLS.
/// @param timeline What the controller stamps; it must outlive the controller.
/// @param sessionSettings Asked before each session the controller creates; empty to keep none.
/// @return The controller, not yet connecting.
[[nodiscard]] std::unique_ptr<contour::remote::NativeController> makeNativeController(
    vthost::TcpEndpoint endpoint,
    TestTimeline& timeline,
    contour::remote::NativeController::SessionSettingsSource sessionSettings = {})
{
    return std::make_unique<contour::remote::NativeController>(
        std::make_shared<vthost::TlsTransportFactory>(std::move(endpoint)),
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows, .windows = {} },
        std::string {},
        std::nullopt,
        std::move(sessionSettings),
        core::platform::defaultSteadyClock(),
        timeline.timeline);
}

/// Captures every log category into one buffer that several threads may write while another
/// reads it. core::log::ScopedCapture is single-threaded by design; a test with a daemon thread and
/// a GUI reactor thread logging while the test thread polls would race on its buffer, which
/// corrupts the heap.
class SharedLogCapture
{
  public:
    SharedLogCapture()
    {
        for (auto const& each: core::log::get())
        {
            auto& target = each.get();
            _restorePoints.push_back(RestorePoint {
                .target = &target, .previousSink = &target.sink(), .wasEnabled = target.isEnabled() });
            target.enable();
            target.setSink(_sink);
        }
    }

    /// Restores every category. Runs after the threads that logged have been joined: declare the
    /// capture before the objects that own them.
    ~SharedLogCapture()
    {
        for (auto const& point: _restorePoints)
        {
            point.target->setSink(*point.previousSink);
            point.target->enable(point.wasEnabled);
        }
    }

    SharedLogCapture(SharedLogCapture const&) = delete;
    SharedLogCapture& operator=(SharedLogCapture const&) = delete;
    SharedLogCapture(SharedLogCapture&&) = delete;
    SharedLogCapture& operator=(SharedLogCapture&&) = delete;

    /// @param needle The substring to look for.
    /// @return Whether any captured output contains @p needle.
    [[nodiscard]] bool contains(std::string_view needle) const
    {
        auto const lock = std::lock_guard { _mutex };
        return _text.contains(needle);
    }

    /// @return A copy of everything captured so far.
    [[nodiscard]] std::string text() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _text;
    }

  private:
    struct RestorePoint
    {
        core::log::Category* target;
        core::log::Sink* previousSink;
        bool wasEnabled;
    };

    mutable std::mutex _mutex;
    std::string _text;
    core::log::Sink _sink { true, [this](std::string_view const& text) {
                               auto const lock = std::lock_guard { _mutex };
                               _text += text;
                           } };
    std::vector<RestorePoint> _restorePoints;
};

/// An in-process `contour daemon` (native protocol) on its own thread, serving
/// TLS over a LOOPBACK TCP socket on an EPHEMERAL port. Using TCP (the same
/// transport `client --connect-tcp` uses) rather than AF_UNIX lets these
/// end-to-end tests run on every platform, Windows included.
/// A daemon end of a connection that the fixture can drop, as a crashing daemon would: it is listed in
/// @c live for as long as it exists.
class DroppableSocket final: public core::net::testing::SocketDecorator
{
  public:
    DroppableSocket(std::unique_ptr<core::net::ISocket> inner, std::set<DroppableSocket*>& live):
        SocketDecorator(*inner), _inner(std::move(inner)), _live(live)
    {
        _live.insert(this);
    }
    ~DroppableSocket() override { _live.erase(this); }

    DroppableSocket(DroppableSocket const&) = delete;
    DroppableSocket& operator=(DroppableSocket const&) = delete;
    DroppableSocket(DroppableSocket&&) = delete;
    DroppableSocket& operator=(DroppableSocket&&) = delete;

  private:
    std::unique_ptr<core::net::ISocket> _inner;
    std::set<DroppableSocket*>& _live;
};

struct DaemonFixture
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::shared_ptr<vthost::IExtensionHosting> extensions;
    vthost::NativeSessionServices services;
    std::unique_ptr<vthost::SessionHost> host;
    /// Serves each accepted connection on @c host; replaced with it by restartHost().
    vthost::ConnectionHandler native;
    std::unique_ptr<vthost::ConnectionAcceptor> server;
    std::set<DroppableSocket*> live; ///< The daemon ends of the connections up now. Daemon thread only.
    std::uint16_t port = 0;          ///< The OS-assigned loopback port the daemon listens on.
    std::thread thread;
    bool cancelled = false; ///< Whether teardown unwound the accept loop.
    uint64_t instance = 1;  ///< Which run of the daemon @c host is; 0 states none.
    /// The host's profiles, by name; none resolves every spawn to the host's own settings.
    std::shared_ptr<vthost::ProfileCatalog const> profiles;
    /// The workspace the first host restores as it starts; none restores nothing.
    std::optional<vthost::WorkspaceRestore> restore;

    /// @return A host of the fixture's run @c instance, serving MockPty sessions.
    [[nodiscard]] std::unique_ptr<vthost::SessionHost> makeHost()
    {
        return std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            vtbackend::Settings {},
            core::defaultEnvironment(),
            vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                        .extensions = extensions,
                                        .profiles = profiles,
                                        .instance = instance,
                                        .restore = std::exchange(restore, std::nullopt) });
    }

    /// @param extensions The status-line extensions the hosted sessions run; none by default.
    /// @param services What each native connection is served with: what its handshake admits.
    /// @param instance Which run of the daemon the first host is; 0 for one that states none.
    /// @param profiles The host's profiles; none by default.
    /// @param restore The workspace the first host restores as it starts; none by default. Only that host:
    ///        restartHost()'s is a run with nothing to restore.
    explicit DaemonFixture(std::shared_ptr<vthost::IExtensionHosting> extensions = {},
                           vthost::NativeSessionServices services = {},
                           uint64_t instance = 1,
                           std::shared_ptr<vthost::ProfileCatalog const> profiles = {},
                           std::optional<vthost::WorkspaceRestore> restore = std::nullopt):
        extensions(std::move(extensions)),
        services(std::move(services)),
        instance(instance),
        profiles(std::move(profiles)),
        restore(std::move(restore))
    {
        host = makeHost();
        auto listener = core::net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        port = (*listener)->boundPort();
        // Encrypt each accepted socket (server-side TLS, self-signed) before the
        // native protocol runs over it — the daemon's real TCP path. This exercises
        // the two-reactor TLS handshake (client + daemon on independent reactors),
        // which NativeClient's concurrent read+write drives.
        auto tls = core::net::makeSelfSignedServerContext();
        REQUIRE(tls.has_value());
        native = vthost::makeNativeHandler(loop, *host, this->services);
        auto const& context = *tls;
        auto handler = [this, context](vthost::ConnectionId id, std::unique_ptr<core::net::ISocket> socket) {
            return native(std::move(id),
                          std::make_unique<DroppableSocket>(context->wrap(std::move(socket), loop), live));
        };
        server = std::make_unique<vthost::ConnectionAcceptor>(loop, "test", std::move(*listener), handler);
        thread = std::thread { [this] {
            try
            {
                loop.blockOn(server->serve());
            }
            catch (core::async::OperationCancelled const&)
            {
                cancelled = true; // teardown cancelled the accept loop
            }
        } };
    }

    ~DaemonFixture()
    {
        loop.post([this] {
            server->close();
            loop.requestStop();
        });
        thread.join();
        // The accept loop has returned, but the connection flows it spawned may still be parked.
        // End them now, while the SessionHost they subscribe to still exists; ~EventLoop runs after
        // the host is gone.
        {
            auto const drain = vthost::LoopDrain { loop };
        }
    }

    DaemonFixture(DaemonFixture const&) = delete;
    DaemonFixture& operator=(DaemonFixture const&) = delete;
    DaemonFixture(DaemonFixture&&) = delete;
    DaemonFixture& operator=(DaemonFixture&&) = delete;

    /// The endpoint a client dials: loopback TCP + TLS (TOFU), no token.
    [[nodiscard]] vthost::TcpEndpoint endpoint() const
    {
        return vthost::TcpEndpoint { .host = "127.0.0.1", .port = port, .token = {}, .caPem = {} };
    }

    /// Runs @p fn on the daemon's loop thread and waits for its result —
    /// SessionHost is confined to that thread.
    template <typename F>
    auto onDaemon(F&& fn) -> decltype(fn())
    {
        auto promise = std::promise<decltype(fn())> {};
        auto future = promise.get_future();
        loop.post([&promise, fn = std::forward<F>(fn)]() mutable { promise.set_value(fn()); });
        return future.get();
    }

    /// Drops every connection, as a daemon that went away would, and keeps serving: what a client reconnects
    /// to finds the same host, sessions and all.
    void dropConnections()
    {
        onDaemon([this] {
            for (auto* socket: live)
                socket->close();
            return 0;
        });
    }

    /// Replaces the host with a new run's, as a restarted daemon: every window and session gone, ids counting
    /// from 1 again, and another instance stated.
    void restartHost()
    {
        whileDisconnected([this] {
            if (instance != 0)
                ++instance;
            host = makeHost();
        });
    }

    /// Drops every connection, runs @p change on the daemon's thread once none is left, and only then serves
    /// again: a client reconnecting meanwhile finds nothing, and so meets the daemon as @p change left it.
    template <typename F>
    void whileDisconnected(F change)
    {
        refuseConnections();
        serveAgain(std::move(change));
    }

    /// Drops every connection and closes each new one as it is accepted, until serveAgain(): a client
    /// reconnecting meanwhile keeps retrying.
    void refuseConnections()
    {
        onDaemon([this] {
            native = [](vthost::ConnectionId,
                        std::unique_ptr<core::net::ISocket> socket) -> core::async::Task<void> {
                socket->close();
                co_return;
            };
            for (auto* socket: live)
                socket->close();
            return 0;
        });
        // The connections' flows hold the host until they ended.
        REQUIRE(waitUntilOnDaemon([this] { return live.empty(); }));
    }

    /// Runs @p change on the daemon's thread, then serves connections again (@see refuseConnections).
    template <typename F = void (*)()>
    void serveAgain(F change = [] {})
    {
        onDaemon([this, &change] {
            change();
            native = vthost::makeNativeHandler(loop, *host, services);
            return 0;
        });
    }

    /// Polls @p predicate on the daemon's thread until it holds, for up to two seconds.
    template <typename F>
    [[nodiscard]] bool waitUntilOnDaemon(F predicate)
    {
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 400))
        {
            if (onDaemon(predicate))
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    /// @return What the pty of @p session has been sent so far.
    [[nodiscard]] std::string ptyInput(vtworkspace::SessionId session)
    {
        return onDaemon([this, session] {
            auto& tapped = dynamic_cast<vthost::TappingPty&>(host->terminal(session)->device());
            return std::string { dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer() };
        });
    }

    /// Seeds one session and writes @p text on its terminal.
    [[nodiscard]] vtworkspace::SessionId seedSession(std::string text)
    {
        return onDaemon([this, text = std::move(text)] {
            host->createTab();
            auto const id = host->model().window(host->windowId())->activeTab()->rootPane()->session();
            host->terminal(id)->writeToScreen(text);
            return id;
        });
    }
};

/// Polls @p predicate at 5ms intervals (pumping the Qt event loop, so queued work runs) until it
/// holds or @p attempts elapse. Also used inverted, to assert a state is never reached.
[[nodiscard]] bool waitUntil(std::function<bool()> const& predicate, int attempts = 400)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, attempts))
    {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/// A page size in the (columns, lines) order these tests read in.
[[nodiscard]] vtpty::PageSize cells(int columns, int lines)
{
    return vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) };
}

/// The router's default factory, unreachable in these tests: attach mode always installs a delegate.
struct UnusedDefaultFactory final: contour::session::SessionFactory
{
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string>,
                                                        std::optional<vtbackend::PageSize>,
                                                        std::optional<vtpty::Process::ExecInfo>,
                                                        std::optional<std::string>) override
    {
        return nullptr;
    }
};

/// The production route to a `NativeController`.
///
/// The app never holds a controller as its factory: it holds a `RoutingSessionFactory` for its whole
/// life and swaps the DELEGATE when attach mode starts, so every call a session makes travels through
/// the router. Tests used to call the controller directly, which skips it — and a verb the router
/// forgets to forward is then invisible, because the base `SessionFactory` gives every verb a no-op
/// default. That is exactly how `bindTerminal` came to be dropped: a daemon-hosted pane rendered
/// nothing at all in the real GUI while every test here passed. Going through the router is what
/// makes these tests able to see it.
struct AttachRoute
{
    contour::remote::RoutingSessionFactory router { std::make_unique<UnusedDefaultFactory>() };

    explicit AttachRoute(contour::remote::NativeController& controller) { router.setDelegate(&controller); }

    AttachRoute(AttachRoute const&) = delete;
    AttachRoute& operator=(AttachRoute const&) = delete;
    AttachRoute(AttachRoute&&) = delete;
    AttachRoute& operator=(AttachRoute&&) = delete;
    ~AttachRoute() = default;
};

/// The local half of a mirrored pane: the terminal a GUI session builds around a router-issued
/// pty, wired up exactly as `TerminalSessionManager` wires it.
///
/// Building the terminal is not scaffolding around the thing under test — it IS the production path.
/// The controller populates this terminal's GRID rather than feeding reconstructed escape sequences
/// into the pty, so `createPty` alone mirrors nothing; `bindTerminal` is what starts it, and the
/// grid is where the answer shows up.
struct MirrorPane
{
    vtbackend::Terminal::NullEvents events;
    std::unique_ptr<vtbackend::Terminal> terminal;

    // SELF-REFERENTIAL, hence immovable: the terminal holds a reference to `events` above, so a move
    // would leave it pointing at the source object. Construct in place (optional::emplace), never
    // `std::optional { MirrorPane { ... } }` or a by-value return — either moves a temporary, and the
    // terminal then calls into a destroyed event listener from the reactor thread. Hold it by
    // unique_ptr where it has to be resettable.
    MirrorPane(MirrorPane const&) = delete;
    MirrorPane& operator=(MirrorPane const&) = delete;
    MirrorPane(MirrorPane&&) = delete;
    MirrorPane& operator=(MirrorPane&&) = delete;
    ~MirrorPane() = default;

    /// @param factory The ROUTER, not the controller — @see AttachRoute.
    /// @param pty The pty that factory handed out.
    /// @param interruptions Where the session is told what a restart stopped; none by default.
    MirrorPane(contour::session::SessionFactory& factory,
               std::unique_ptr<vtpty::Pty> pty,
               contour::session::InterruptionSink const& interruptions = {})
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pty->pageSize();
        auto* device = pty.get();
        terminal = std::make_unique<vtbackend::Terminal>(events,
                                                         core::defaultEnvironment(),
                                                         core::platform::NativeFileSystem::instance(),
                                                         std::move(pty),
                                                         std::move(settings),
                                                         std::nullopt,
                                                         vtbackend::TerminalClocks::system(),
                                                         vtbackend::randomSemanticBlockTokens());
        factory.bindTerminal(device, *terminal, interruptions);
    }

    [[nodiscard]] vtpty::Pty& pty() const noexcept { return terminal->device(); }

    /// @return True once @p needle shows on the mirrored page. Under the terminal lock: the
    ///         controller populates the grid from its reactor thread.
    [[nodiscard]] bool shows(std::string_view needle) const
    {
        return waitUntil([&] {
            auto const guard = std::lock_guard { *terminal };
            return terminal->primaryScreen().grid().renderMainPageText().contains(needle);
        });
    }
};

/// Connects @p controller to the fixture's daemon, failing the test with the
/// controller's own reason (a timeout, a closed connection, a TLS error) rather
/// than a bare `false` -- the difference between a diagnosable CI failure and a
/// guess.
void requireConnected(contour::remote::RemoteController& controller, std::chrono::milliseconds timeout = 10s)
{
    auto const connected = controller.connectAndWait(timeout);
    INFO("connectAndWait: " << (connected ? std::string { "ok" } : connected.error()));
    REQUIRE(connected.has_value());
}

/// Connects @p controller, waits for the daemon's first layout, and realizes it into @p window.
/// @return The realized tab, or nullptr if no layout ever arrived.
[[nodiscard]] vtworkspace::Tab* attachAndRealize(contour::remote::NativeController& controller,
                                                 contour::session::TerminalSessionManager& manager,
                                                 vtworkspace::WindowId window)
{
    requireConnected(controller);
    if (!waitUntil([&] { return controller.layout().has_value(); }))
        return nullptr;
    contour::remote::applyRemoteLayout(manager, window, controller);
    return manager.model().window(window)->tabAt(0);
}
} // namespace

/// Renders a PageSize in Catch2 failure output (it would print `{?}` otherwise), through the
/// formatter vtbackend already defines for it.
template <>
struct Catch::StringMaker<vtpty::PageSize>
{
    static std::string convert(vtpty::PageSize value) { return std::format("{}", value); }
};

namespace
{

/// A factory that can never back a session (the attach guard's stand-in).
struct RefusingFactory final: contour::session::SessionFactory
{
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string> /*cwd*/,
                                                        std::optional<vtbackend::PageSize> pageSize,
                                                        std::optional<vtpty::Process::ExecInfo> /*command*/,
                                                        std::optional<std::string> /*profile*/) override
    {
        return std::make_unique<vtpty::MockPty>(
            pageSize.value_or(vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) }));
    }
    [[nodiscard]] bool canCreateSession() const noexcept override { return false; }
};

} // namespace

TEST_CASE("attach controller mirrors a remote session over a real socket", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("hello attach");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());
    REQUIRE(controller.pendingCount() == 1);
    REQUIRE(controller.canCreateSession());
    CHECK(controller.hosting() == contour::session::SessionHosting::Remote);

    // Through the ROUTER, exactly as the app does — see AttachRoute.
    auto route = AttachRoute { controller };
    // The factory hands out a pty bound to the remote session...
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    CHECK(!controller.canCreateSession()); // the one pending session is consumed

    // ...and once the terminal built around it is announced, the mirror populates its grid with
    // the remote screen.
    auto pane = MirrorPane { route.router, std::move(pty) };
    CHECK(pane.shows("hello attach"));

    // Input written by the terminal reaches the remote PTY.
    std::ignore = pane.pty().write("ls\r");
    CHECK(waitUntil([&] {
        return daemon.onDaemon([&] {
            auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
            return dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer();
        }) == "ls\r";
    }));

    // A local resize proposes the client area upstream. waitUntil pumps the Qt event loop, which
    // the geometry report needs: it is coalesced through a queued call (@see reportPaneGeometry).
    pane.pty().resizeScreen(cells(100, 40));
    CHECK(waitUntil(
        [&] { return daemon.onDaemon([&] { return daemon.host->pageSize(); }) == cells(100, 40); }));

    // Detaching closes the bound pty: its session would see a shell exit.
    controller.stop();
    CHECK(pane.pty().isClosed());
    pane.terminal.reset(); // unbind (the controller outlives its ptys' registrations)
}

TEST_CASE("NativeController.interruption.postedToTheSessionsThread", "[attach][controller][interrupted]")
{
    // The daemon restored a pane over the command its restart stopped. The mirror learns it on the reactor
    // thread; the session it belongs to is told on the GUI thread (this one), by a posted call.
    auto const snapshot = vthost::WorkspaceSnapshot {
        .takenAt = std::chrono::sys_seconds { std::chrono::seconds { 1'761'000'000 } },
        .windows = { vthost::WindowSnapshot {
            .tabs = { vthost::TabSnapshot {
                .root = vthost::PaneNode { .pane = vthost::PaneSnapshot { .runningCommandLine =
                                                                              "make test" } } } } } }
    };
    auto daemon =
        DaemonFixture { {}, {}, 1, {}, vthost::WorkspaceRestore { .snapshot = snapshot, .marker = {} } };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(controller.pendingCount() == 1);
    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);

    auto context = QObject {}; // the session's stand-in: it lives on this thread, the GUI's
    auto told = std::vector<std::optional<vtbackend::CommandLineText>> {};
    auto threads = std::vector<std::thread::id> {};
    auto pane = MirrorPane { route.router,
                             std::move(pty),
                             contour::session::InterruptionSink {
                                 .context = &context,
                                 .deliver = [&](std::optional<vtbackend::CommandLineText> commandLine) {
                                     told.push_back(std::move(commandLine));
                                     threads.push_back(std::this_thread::get_id());
                                 } } };

    REQUIRE(waitUntil([&] { return !told.empty(); }));
    REQUIRE(told.size() == 1);
    CHECK(told.front() == vtbackend::CommandLineText { "make test" });
    // Posted, not called from the reactor.
    CHECK(threads.front() == std::this_thread::get_id());

    // The shell's next command starts on the daemon: the daemon forgets it, the mirror follows, and the
    // session is told it is gone -- once.
    auto const session = vtworkspace::SessionId { 1 };
    std::ignore = daemon.onDaemon([&] {
        auto& terminal = *daemon.host->terminal(session);
        auto& device = dynamic_cast<vthost::TappingPty&>(terminal.device());
        dynamic_cast<vtpty::MockPty&>(device.inner())
            .appendStdOutBuffer("\033]133;A\033\\$ \033]133;B\033\\\033]133;C\033\\");
        std::ignore = terminal.processInputOnce();
        return 0;
    });
    CHECK(waitUntil([&] { return told.size() == 2; }));
    REQUIRE(told.size() == 2);
    CHECK_FALSE(told.back().has_value());
    CHECK_FALSE(waitUntil([&] { return told.size() > 2; }, /*attempts*/ 20));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("NativeController.extension.permission", "[attach][controller][extensions]")
{
    // A daemon's extension asked while no client was attached: the attaching client hands the question to
    // the pane it is asked in, and sends the user's answer back, which the daemon records.
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto daemon = DaemonFixture { hosting };
    std::ignore = daemon.seedSession("hello");
    auto const asked = vthost::proto::PermissionRequest {
        .session = 0,
        .hostId = "git@/usr/share/contour/extensions/git.wasm",
        .name = "git",
        .version = 1,
        .digest = "5f3c",
        .commands = { vthost::proto::WireDeclaredCommand { .name = "status", .argv = { "git", "status" } } },
    };
    REQUIRE(daemon.onDaemon([&] {
        hosting->sessions.front()->askPermission(asked);
        return true;
    }));

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    auto pane = MirrorPane { route.router, std::move(pty) };

    auto taken = std::vector<contour::remote::NativeController::HostedPermissionRequest> {};
    REQUIRE(waitUntil([&] {
        auto more = controller.takePermissionRequests();
        taken.insert(taken.end(), more.begin(), more.end());
        return !taken.empty();
    }));
    REQUIRE(taken.size() == 1);
    CHECK(taken.front().pty == &pane.pty());
    CHECK(taken.front().request.name == "git");
    CHECK(taken.front().request.hostId == asked.hostId);
    CHECK(taken.front().request.commands == asked.commands);

    controller.answerPermission(asked.hostId, asked.digest, vthost::GrantState::Granted);
    CHECK(waitUntil([&] {
        return daemon.onDaemon([&] { return hosting->lookup(asked.hostId, asked.digest); })
               == vthost::GrantState::Granted;
    }));
    CHECK(daemon.onDaemon([&] { return daemon.host->pendingPermissionRequests().empty(); }));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a GUI detach ends the daemon's TLS connection as a disconnect", "[attach][controller]")
{
    // stop() is the GUI's detach: stopMuxReactor posts the detach and only then cancels the loop.
    // Cancelled too early, the client's orderly close -- close_notify over TLS -- is unwound half
    // done, and the daemon logs an ordinary detach as a truncated read.
    auto capture = SharedLogCapture {};
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("detach");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    REQUIRE(controller.connectAndWait(10s).has_value());
    controller.stop();

    // The daemon's side of the connection ends a few turns later, on its own thread.
    CHECK(waitUntil([&] { return capture.contains(": disconnected") || capture.contains("read failed"); }));
    INFO(capture.text());
    CHECK(capture.contains(": disconnected"));
    CHECK_FALSE(capture.contains("read failed"));
}

// B2 foundation: the controller captures the daemon's authoritative tab/pane
// tree (the daemon pushes LayoutState leading the attach snapshot), so the GUI
// can reconstruct its own split tree from it rather than flattening one tab per
// session.
TEST_CASE("attach controller captures the daemon's tab and pane layout", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("one");
    // Split the seeded tab so the layout carries a non-trivial pane tree.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());

    // The layout is pushed leading the snapshot; poll until the controller has it.
    auto layout = std::optional<vthost::proto::LayoutState> {};
    for (auto i = 0; i < 200 && !(layout = controller.layout()).has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(layout.has_value());
    REQUIRE(layout->tabs.size() == 1);

    // The tab's root is a vertical split with two distinct-session leaves.
    auto const& root = layout->tabs.front().root;
    CHECK(root.split == std::to_underlying(vtworkspace::SplitState::Vertical));
    REQUIRE(root.children.size() == 2);
    CHECK(root.children[0].session != 0);
    CHECK(root.children[1].session != 0);
    CHECK(root.children[0].session != root.children[1].session);

    controller.stop();
}

// B2 executor end-to-end: a split daemon layout is realized as a real 2-pane tab
// in the GUI's own SessionModel — over a real TCP+TLS attach connection, driven by
// the shared applyRemoteLayout. Each pane binds to its remote session through the
// beforeLeafSeed → setNextBindSession seam (the path createPty takes here).
TEST_CASE("attach realizes a split daemon layout as a 2-pane tab", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("root");
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.6);
        return 0;
    });

    // The NativeController is the manager's session factory (attach mode).
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    // The layout leads the snapshot; poll until the split tree has arrived.
    for (auto i = 0; i < 200; ++i)
    {
        auto const l = ac->layout();
        if (l && l->tabs.size() == 1 && l->tabs.front().root.children.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }
    auto const wl = ac->wireLayout();
    REQUIRE(wl.layout.tabs.size() == 1);

    // Realize the daemon's tree into the GUI window.
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);

    auto* window = app.manager().model().window(win.id);
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 1);
    auto* tab = window->tabAt(0);
    REQUIRE(tab != nullptr);
    CHECK(tab->paneCount() == 2); // the daemon's split reproduced locally
    REQUIRE_FALSE(tab->rootPane()->isLeaf());
    CHECK(tab->rootPane()->splitState() == vtworkspace::SplitState::Vertical);

    ac->stop();
}

// B3-Qt: a split authored on the daemon AFTER attach reconciles into the already-
// shown tab as a second pane (intra-tab incremental reconciliation), rather than
// only whole new tabs.
TEST_CASE("attach reconciles a split authored on the daemon after attach", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    // Split the tab on the daemon (after the client attached) and wait for the
    // split to reach the client's layout.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Horizontal, 0.5);
        return 0;
    });
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout(); l && !l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE_FALSE(ac->layout()->tabs.front().root.children.empty());

    // Reconcile: the already-shown tab grows a second pane.
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);
    CHECK(app.manager().model().window(win.id)->tabCount() == 1); // it stayed one tab, now split

    ac->stop();
}

// The incremental reconcile must reproduce the daemon split's ACTUAL ratio, not a
// hard-coded even split — a non-even split authored on the daemon should mirror
// with matching proportions (regression: the SplitOp discarded node.ratio).
TEST_CASE("attach reconciles an uneven daemon split with matching proportions", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    // A distinctly uneven split (0.7 to the first/acting child) authored on the daemon.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.7);
        return 0;
    });
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout(); l && !l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE_FALSE(ac->layout()->tabs.front().root.children.empty());

    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* root = app.manager().model().window(win.id)->tabAt(0)->rootPane();
    REQUIRE_FALSE(root->isLeaf());
    // The first child's share is ~0.7, NOT the 0.5 default the dropped ratio produced.
    CHECK(root->ratio() > 0.6);
    CHECK(root->ratio() < 0.8);

    ac->stop();
}

// B3-Qt: a pane closed on the daemon (here, or by another client) is removed
// locally by the subtractive reconciler — the remote session leaves the layout,
// so its local pane is terminated.
TEST_CASE("attach reconciles a pane closed on the daemon by removing it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");
    auto const second = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return tab->activePane()->session(); // the new (active) pane's session
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);

    // Close the second pane ON THE DAEMON; wait for the layout to drop it.
    daemon.onDaemon([&daemon, second] {
        daemon.host->handleSessionExit(second);
        return 0;
    });
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout(); l && l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(ac->layout()->tabs.front().root.children.empty());

    // Reconcile: the local pane for the vanished session is terminated (async
    // teardown, so pump the event loop until the tab is back to a single pane).
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto const closed = [&] {
        for (auto i = 0; i < 200; ++i)
        {
            QCoreApplication::processEvents();
            if (app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1)
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }();
    CHECK(closed);
    // The surviving pane's session is untouched: reconciling a daemon-side close must not take
    // the rest of the tab down with it. (The redundant ClosePane the reconciler used to echo for
    // the ALREADY-removed session is not separately observable — session ids are monotonic, so an
    // echoed id can never name a live session — which is why this asserts the survivor instead.)
    CHECK(daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); }) == 1);

    ac->stop();
}

// Releasing a pty is NOT ending its session. `unbind()` runs from the pty's destructor, which
// cannot tell a pane close from a window close, an app quit or a lost connection — so it must only
// forget the binding. Authoring a close from there (what it used to do, gated on a `_stopped` flag
// that is set far too late to mean anything) got the answer wrong in both directions: nothing that
// ends a session destroyed a pty early enough for it to fire, so no close was ever authored at all,
// and any future change that DID delete a session on window close would silently have started
// killing the user's shells. This pins the contract from the side a test can drive directly.
TEST_CASE("releasing a mirrored pty leaves its remote session running", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("keep me running");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(controller.pendingCount() == 1);
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };
    REQUIRE(liveSessions() == 1);

    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    auto pane = std::make_unique<MirrorPane>(route.router, std::move(pty));
    CHECK(pane->shows("keep me running"));

    // The pane's pty goes away with the connection still up. Inverted wait: give any ClosePane a
    // generous window to cross the wire and be applied, then assert the session never died.
    pane.reset();
    CHECK_FALSE(waitUntil([&] { return liveSessions() == 0; }));
    CHECK(liveSessions() == 1);

    controller.stop();
}

// The user-visible half of the same defect, and the counterpart of the case above: closing a PANE
// really does end its session, remotely exactly as locally. It did not before — the close was
// authored only from the pty destructor, and a TerminalSession is a QObject child of the manager
// that nothing deletes until app teardown, so the ClosePane never went out. A pane closed while
// attached stayed alive on the daemon with no view, and reappeared on the next attach.
TEST_CASE("the ClosePane action ends a remote session like a local one", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout();
            l && l->tabs.size() == 1 && l->tabs.front().root.children.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };
    REQUIRE(liveSessions() == 2);

    // The ClosePane action's own path: TerminalSession::operator()(actions::ClosePane) forwards
    // here, and the acting session names the tab whose ACTIVE pane closes.
    app.manager().closeActivePane(win->activeSession(), contour::session::SessionEnd::Destroy);

    CHECK(waitUntil([&] { return liveSessions() == 1; }));
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    ac->stop();
}

TEST_CASE("an action that changes hosted state is performed by the daemon, and the mirror follows",
          "[attach][controller][operations][threading]")
{
    static_assert(contour::remote::toWireOperation(contour::session::HostedOperation::WriteScreen)
                  == vthost::proto::EngineOperation::WriteScreen);

    auto daemon = DaemonFixture {};
    auto rows = std::string {};
    for (auto const row: std::views::iota(0, 60))
        rows += std::format("row {}\r\n", row);
    auto const session = daemon.seedSession(rows + "\033]133;A\033\\$ prompt-here");
    auto const daemonHistory = [&] {
        return daemon.onDaemon(
            [&] { return daemon.host->terminal(session)->primaryScreen().historyLineCount(); });
    };
    REQUIRE(daemonHistory() > vtbackend::LineCount(0));

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->layout().has_value(); }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* pane = win->activeSession();
    REQUIRE(pane != nullptr);
    auto const& terminal = pane->terminal();
    auto const& mirror = terminal.primaryScreen();
    // Under the terminal lock: the controller writes the mirror from its reactor thread.
    auto const mirrorShows = [&](auto const& predicate) {
        return waitUntil([&] {
            auto const guard = std::lock_guard { terminal };
            return predicate();
        });
    };
    REQUIRE(mirrorShows([&] { return mirror.historyLineCount() > vtbackend::LineCount(0); }));

    // The action's own path, as a key binding or the context menu runs it.
    CHECK((*pane)(contour::actions::ClearToPrompt {}));

    CHECK(waitUntil([&] { return daemonHistory() == vtbackend::LineCount(0); }));
    CHECK(mirrorShows([&] { return mirror.historyLineCount() == vtbackend::LineCount(0); }));
    CHECK(mirrorShows(
        [&] { return mirror.grid().lineText(vtbackend::LineOffset(0)).starts_with("$ prompt-here"); }));

    ac->stop();
}

TEST_CASE("an action that changes hosted state does not report success without a daemon",
          "[attach][controller][operations]")
{
    // Never connected, as after the daemon went away: there is nowhere for the operation to run that would
    // stick, so it is not "sent".
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} }, timeline);
    auto& controller = *controllerOwned;
    auto const pty =
        vtpty::MockPty { vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) } };
    for (auto const& row: contour::session::HostedOperationTable)
        CHECK(controller.requestHostedOperation(
                  &pty, contour::session::HostedOperationRequest { .operation = row.operation })
              == contour::session::HostedOperationRoute::Unreachable);
}

namespace
{

/// Types @p keys into @p session, each one pressed and released, as a keyboard delivers them.
void typeKeys(contour::session::TerminalSession& session, std::string_view keys)
{
    for (auto const key: keys)
    {
        auto const ch = static_cast<char32_t>(key);
        auto const identity =
            vtbackend::KeyIdentity { .unshiftedKey = ch, .nativeVirtualKey = static_cast<uint32_t>(ch) };
        for (auto const eventType:
             { vtbackend::KeyboardEventType::Press, vtbackend::KeyboardEventType::Release })
            session.sendCharEvent(
                ch, identity, vtbackend::KeyboardModifiers {}, eventType, std::chrono::steady_clock::now());
    }
}

/// The rows of @p terminal's page that carry a vi mark, by their text.
[[nodiscard]] std::vector<std::string> markedRows(vtbackend::Terminal const& terminal)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto marked = std::vector<std::string> {};
    for (auto const row: std::views::iota(0, unbox<int>(terminal.pageSize().lines)))
        if (grid.lineAt(vtbackend::LineOffset(row)).isFlagEnabled(vtbackend::LineFlag::UserMark))
        {
            auto text = grid.lineText(vtbackend::LineOffset(row));
            text.erase(text.find_last_not_of(' ') + 1);
            marked.push_back(std::move(text));
        }
    return marked;
}

} // namespace

TEST_CASE("a vi mark on an attached pane is the daemon's, and the mirror shows it",
          "[attach][controller][operations][vi]")
{
    static_assert(contour::remote::toWireOperation(contour::session::HostedOperation::SetUserMark)
                  == vthost::proto::EngineOperation::SetUserMark);

    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("first\r\nsecond\r\nthird\r\n$ ");
    auto const daemonMarks = [&] {
        return daemon.onDaemon([&] { return markedRows(*daemon.host->terminal(session)); });
    };

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->layout().has_value(); }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* pane = win->activeSession();
    REQUIRE(pane != nullptr);
    auto const& terminal = pane->terminal();
    // Under the terminal lock: the controller writes the mirror from its reactor thread.
    auto const mirrorMarks = [&] {
        auto const guard = std::lock_guard { terminal };
        return markedRows(terminal);
    };
    REQUIRE(waitUntil([&] {
        auto const guard = std::lock_guard { terminal };
        return terminal.primaryScreen().grid().renderMainPageText().contains("third");
    }));

    // The keys' own path, as the keyboard delivers them in normal mode: up from the prompt to "second".
    (*pane)(contour::actions::ViNormalMode {});
    typeKeys(*pane, "kkmm");

    CHECK(waitUntil([&] { return daemonMarks() == std::vector<std::string> { "second" }; }));
    CHECK(waitUntil([&] { return mirrorMarks() == std::vector<std::string> { "second" }; }));

    // `mm` on the line the daemon marked takes the mark off there, and so here.
    typeKeys(*pane, "mm");
    CHECK(waitUntil([&] { return daemonMarks().empty(); }));
    CHECK(waitUntil([&] { return mirrorMarks().empty(); }));

    ac->stop();
}

// Regression: what a client reports upstream after a split. `proto::ResizeRequest` names the whole
// CLIENT AREA — the daemon projects the tab's pane tree into it (SessionHost::reprojectLayouts ->
// vtworkspace::layoutInCells). Reporting one PANE's grid there halved the area on every split: a
// 100-column window split in two produced ~25-column remote grids inside 50-column local panes, so
// the right half of each pane rendered empty. And because the daemon projects by split RATIO while
// the GUI lays out in PIXELS, the client area alone cannot express a dragged divider — each pane's
// own grid has to reach its PTY (proto::ResizePane).
TEST_CASE("attach sizes remote panes to the panes the GUI renders", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("root");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return ac->layout().has_value(); }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(app.manager().sessionsOfTab(tab).size() == 1);

    auto const clientArea = [&] {
        return daemon.onDaemon([&] { return daemon.host->pageSize(); });
    };
    auto const remoteSize = [&](uint64_t session) {
        return daemon.onDaemon([&] {
            auto* terminal = daemon.host->terminal(vtworkspace::SessionId { session });
            return terminal != nullptr ? terminal->pageSize() : vtpty::PageSize {};
        });
    };

    // The window is 100x30 cells while still unsplit; the daemon adopts that as its client area.
    app.manager().sessionsOfTab(tab).front()->terminal().device().resizeScreen(cells(100, 30));
    REQUIRE(waitUntil([&] { return clientArea() == cells(100, 30); }));

    // Split it evenly on the daemon and reconcile the second pane in.
    daemon.onDaemon([&daemon] {
        auto* daemonTab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(daemonTab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });
    REQUIRE(waitUntil([&] {
        auto const pushed = ac->layout();
        return pushed && pushed->tabs.front().root.children.size() == 2;
    }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().sessionsOfTab(tab).size() == 2);

    // Which remote session backs which local pane (first child = left, second = right). layout()
    // hands out a COPY, so keep it alive while its tree is read.
    auto const pushed = ac->layout();
    auto const leftSession = pushed->tabs.front().root.children[0].session;
    auto const rightSession = pushed->tabs.front().root.children[1].session;
    auto const paneFor = [&](uint64_t remote) -> contour::session::TerminalSession* {
        for (auto* session: app.manager().sessionsOfTab(tab))
            if (ac->sessionForPty(&session->terminal().device()) == remote)
                return session;
        return nullptr;
    };
    auto* left = paneFor(leftSession);
    auto* right = paneFor(rightSession);
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);

    SECTION("the split alone must not shrink anything")
    {
        // What PaneNode.qml's SplitView hands the two panes of a 100-column window. The daemon
        // already projected exactly this, so nothing on it may move — which makes the assertion a
        // NEGATIVE one: waiting for the right state would pass on the state that is about to break.
        left->terminal().device().resizeScreen(cells(50, 30));
        right->terminal().device().resizeScreen(cells(49, 30));

        auto const moved = [&] {
            return clientArea() != cells(100, 30) || remoteSize(leftSession) != cells(50, 30)
                   || remoteSize(rightSession) != cells(49, 30);
        };
        CHECK_FALSE(waitUntil(moved, /*attempts=*/100));
        // Named individually, so a failure says which of the three collapsed.
        CHECK(clientArea() == cells(100, 30));
        CHECK(remoteSize(leftSession) == cells(50, 30));
        CHECK(remoteSize(rightSession) == cells(49, 30));
    }

    SECTION("a window resize after a split reports the composed area")
    {
        // The window grew to 120 columns; the SplitView hands the panes 60 and 59.
        left->terminal().device().resizeScreen(cells(60, 30));
        right->terminal().device().resizeScreen(cells(59, 30));

        CHECK(waitUntil([&] { return clientArea() == cells(120, 30); }));
        CHECK(waitUntil([&] { return remoteSize(leftSession) == cells(60, 30); }));
        CHECK(waitUntil([&] { return remoteSize(rightSession) == cells(59, 30); }));
    }

    SECTION("a dragged divider sizes each pane exactly")
    {
        // The user dragged the divider, so the panes no longer match the daemon's 0.5 ratio — whose
        // projection would still say 50 / 49. Each pane's own grid is what must reach its PTY.
        left->terminal().device().resizeScreen(cells(70, 30));
        right->terminal().device().resizeScreen(cells(29, 30));

        CHECK(waitUntil([&] { return remoteSize(leftSession) == cells(70, 30); }));
        CHECK(waitUntil([&] { return remoteSize(rightSession) == cells(29, 30); }));
        // ...while the client area still describes the whole window.
        CHECK(clientArea() == cells(100, 30));
    }

    ac->stop();
}

// B3-Qt: a GUI split in attach mode is authored on the daemon (routed by
// requestRemoteSplit), which splits the right pane and re-pushes its layout; the
// reconciler realizes the new pane locally — the full split-authoring loop.
TEST_CASE("attach authors a split on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(app.manager().sessionsOfTab(tab).size() == 1);

    // A GUI split of the only pane routes to the daemon (requestRemoteSplit).
    app.manager().splitActivePane(/*vertical=*/true, app.manager().sessionsOfTab(tab).front());
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout(); l && !l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE_FALSE(ac->layout()->tabs.front().root.children.empty());

    // Reconcile: the local tab grows a second pane, still one tab.
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);
    CHECK(app.manager().model().window(win.id)->tabCount() == 1);

    ac->stop();
}

// A ratio only the CLIENT knows is a ratio the user loses: a re-attaching client rebuilds every tab
// from the daemon's LayoutState, so a divider the daemon never heard about comes back at the ratio
// the split was CREATED with. This is the whole point of proto::ResizeSplit — the drag has to reach
// the daemon's model, and the second attach has to read it back.
TEST_CASE("attach reports a moved divider, so a re-attaching client restores it", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    // The daemon's split ratio, or nullopt while its root is still a single leaf.
    auto daemonRatio = [&daemon]() -> std::optional<double> {
        return daemon.onDaemon([&daemon]() -> std::optional<double> {
            auto* root = daemon.host->model().window(daemon.host->windowId())->activeTab()->rootPane();
            return root->isLeaf() ? std::nullopt : std::optional { root->ratio() };
        });
    };

    {
        // The first client splits and then drags the divider well off centre.
        auto timeline = TestTimeline {};
        auto acOwned = makeNativeController(daemon.endpoint(), timeline);
        auto* ac = acOwned.get();
        contour::test::TestApp app { std::move(acOwned) };
        contour::test::ScopedController const win { app.manager() };

        auto* tab = attachAndRealize(*ac, app.manager(), win.id);
        REQUIRE(tab != nullptr);
        REQUIRE(app.manager().sessionsOfTab(tab).size() == 1);

        app.manager().splitActivePane(/*vertical=*/true, app.manager().sessionsOfTab(tab).front());
        // Wait for the split to reach THIS CLIENT's layout, not merely the daemon's model: the
        // daemon mutates its model before it pushes anything, so waiting on the daemon lets
        // applyRemoteLayout run against a LayoutState that predates the split — and it would then
        // find no new split to realize.
        REQUIRE(waitUntil([&] {
            auto const wire = ac->layout();
            return wire.has_value() && !wire->tabs.empty() && wire->tabs.front().root.isSplit();
        }));
        contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
        REQUIRE(tab->paneCount() == 2);

        // The drag itself. Both ratio mutators land on paneRatioChanged, so this is the same route a
        // ResizePane keybinding takes.
        app.manager().setPaneRatio(tab->id(), tab->rootPane()->id(), 0.75);
        CHECK(tab->rootPane()->ratio() == 0.75);
        CHECK(waitUntil([&] { return daemonRatio() == 0.75; }));

        ac->stop();
    }

    // A brand-new client, with nothing claimed: applyRemoteLayout realizes the WHOLE tab from the
    // daemon's layout — the path that used to hand back an even split.
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* tab = attachAndRealize(*ac, app.manager(), win.id);
    REQUIRE(tab != nullptr);
    REQUIRE(tab->paneCount() == 2);
    CHECK(tab->rootPane()->ratio() == 0.75);

    ac->stop();
}

// With the ratio living on the daemon, a divider one client moves has to reach the others: their
// tabs are already realized, so the additive/subtractive passes have nothing to do and would leave
// them showing a layout the daemon no longer describes.
namespace
{
/// One GUI attached to a daemon: its controller, its app, and the window the daemon's first window is
/// realized into.
struct AttachedGui
{
    TestTimeline timeline;
    contour::remote::NativeController* controller;
    contour::test::TestApp app;
    contour::test::ScopedController window;

    explicit AttachedGui(DaemonFixture const& daemon):
        AttachedGui { makeNativeController(daemon.endpoint(), timeline) }
    {
    }

    /// The window's one tab.
    [[nodiscard]] vtworkspace::Tab& tab() { return *app.manager().model().window(window.id)->tabAt(0); }

    /// Reconciles what the daemon last pushed, as the app does on every push.
    void reconcile() { contour::remote::applyRemoteLayout(app.manager(), window.id, *controller); }

    /// @return The daemon's id for the session of @p leaf.
    [[nodiscard]] std::optional<uint64_t> remoteOf(vtworkspace::Pane const& leaf)
    {
        auto* session = app.manager().sessionForId(leaf.session());
        return session != nullptr ? controller->sessionForPty(&session->terminal().device()) : std::nullopt;
    }

  private:
    explicit AttachedGui(std::unique_ptr<contour::remote::NativeController> owned):
        controller { owned.get() }, app { std::move(owned) }, window { app.manager() }
    {
    }
};

/// @return How many EditTab frames the daemon received, by its protocol trace in @p log.
[[nodiscard]] std::ptrdiff_t editTabsReceived(std::string const& log)
{
    static auto const received = std::regex { R"(recv #\d+ EditTab )" };
    return std::distance(std::sregex_iterator(log.begin(), log.end(), received), std::sregex_iterator());
}
} // namespace

TEST_CASE("a second client sees the first client's zoom, title and colour", "[attach][controller][sync-back]")
{
    // Declared first: it outlives every thread that logs into it.
    auto capture = SharedLogCapture {};
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("root");
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });

    auto a = AttachedGui { daemon };
    auto b = AttachedGui { daemon };
    for (auto* gui: { &a, &b })
    {
        requireConnected(*gui->controller);
        REQUIRE(waitUntil([&] {
            auto const layout = gui->controller->layout();
            return layout && layout->tabs.size() == 1 && layout->tabs.front().root.children.size() == 2;
        }));
        gui->reconcile();
        REQUIRE(gui->tab().paneCount() == 2);
    }

    // What B shows, once the daemon pushed it and B reconciled the push.
    auto const bShows = [&](auto const& predicate) {
        return waitUntil([&] {
            b.reconcile();
            return predicate(b.tab());
        });
    };

    // A zooms its active pane: B is zoomed on the same session.
    auto& tabA = a.tab();
    auto const zoomed = a.remoteOf(*tabA.activePane());
    REQUIRE(zoomed.has_value());
    a.app.manager().model().toggleActivePaneZoom(tabA.id());
    CHECK(bShows([&](vtworkspace::Tab const& tab) {
        return tab.isZoomed() && b.remoteOf(*tab.activePane()) == zoomed;
    }));

    // A renames the tab, then colours it.
    a.app.manager().model().setTabTitle(tabA.id(), "build");
    CHECK(bShows([](vtworkspace::Tab const& tab) { return tab.runtimeTitle() == "build"; }));
    auto constexpr Color = vtbackend::RGBColor { 0x336699 };
    a.app.manager().model().setTabColor(tabA.id(), vtworkspace::TabColorSource::User, Color);
    CHECK(bShows(
        [&](vtworkspace::Tab const& tab) { return tab.color(vtworkspace::TabColorSource::User) == Color; }));

    // Every change was sent once, by A; neither client answered what it applied with an edit of its own. A's
    // own pushes are reconciled too, and settle without one.
    a.reconcile();
    b.reconcile();
    std::this_thread::sleep_for(100ms);
    CHECK(editTabsReceived(capture.text()) == 3);

    a.controller->stop();
    b.controller->stop();
}

TEST_CASE("a divider moved on the daemon re-flows an already realized tab", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* tab = attachAndRealize(*ac, app.manager(), win.id);
    REQUIRE(tab != nullptr);
    app.manager().splitActivePane(/*vertical=*/true, app.manager().sessionsOfTab(tab).front());
    REQUIRE(waitUntil([&] {
        auto const layout = ac->layout();
        return layout && !layout->tabs.front().root.children.empty();
    }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(tab->paneCount() == 2);
    REQUIRE(tab->rootPane()->ratio() == 0.5);

    // What a SECOND client's ResizeSplit does to the daemon's model.
    auto const moved = daemon.onDaemon([&daemon] {
        auto* daemonTab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->model().setPaneRatio(daemonTab->id(), daemonTab->rootPane()->id(), 0.3);
        return true;
    });
    REQUIRE(moved);
    REQUIRE(waitUntil([&] {
        auto const layout = ac->layout();
        return layout && layout->tabs.front().root.ratio == 3000;
    }));

    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(tab->rootPane()->ratio() == 0.3);

    // ...and settles there. The pass writes through the model, which re-emits paneRatioChanged; if
    // that echoed back the daemon would keep re-pushing, and two clients would trade a divider
    // forever. Comparing in wire units is what stops the second write from happening at all.
    CHECK_FALSE(waitUntil(
        [&] {
            return daemon.onDaemon([&daemon] {
                auto* root = daemon.host->model().window(daemon.host->windowId())->activeTab()->rootPane();
                return root->ratio() != 0.3;
            });
        },
        /*attempts=*/20));

    ac->stop();
}

namespace
{
/// @return A daemon whose host resolves the profiles "main" (its default) and "ops", and whose handshake
///         offers Capability::ProfileByName besides what it offers by default -- what `contour daemon` serves
///         with profiles configured.
[[nodiscard]] std::unique_ptr<DaemonFixture> daemonWithProfiles()
{
    using vthost::testing::fakeProfile;
    auto services = vthost::NativeSessionServices {};
    services.handshake.capabilities.idents.push_back(
        std::to_underlying(vthost::proto::Capability::ProfileByName));
    return std::make_unique<DaemonFixture>(
        nullptr,
        std::move(services),
        1,
        std::make_shared<vthost::testing::FakeProfileCatalog>(
            std::map<std::string, vthost::ResolvedProfile> {
                { "main", fakeProfile("/bin/main-shell", vtbackend::LineCount(1000)) },
                { "ops", fakeProfile("/bin/ops-shell", vtbackend::LineCount(2000)) },
            },
            "main"));
}

} // namespace

TEST_CASE("a new tab with a profile asks the daemon for that profile", "[attach][controller][profiles]")
{
    auto const daemon = daemonWithProfiles();
    std::ignore = daemon->seedSession("first");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon->endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->layout().has_value(); }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabCount() == 1);

    // The CreateNewTab action's profile, through the manager as the keybinding takes it.
    app.manager().createNewTab(win.id, std::string { "ops" });

    auto const profiles = [&] {
        return daemon->onDaemon([&] {
            auto names = std::vector<std::string> {};
            daemon->host->model().forEachTab([&](vtworkspace::Window&, vtworkspace::Tab& tab) {
                names.push_back(daemon->host->hostedSession(tab.rootPane()->session())->profileName());
            });
            return names;
        });
    };
    REQUIRE(waitUntil([&] { return profiles().size() == 2; }));
    // Not the host's default ("main"): the name the tab asked for. The seeded tab names none.
    CHECK(profiles().back() == "ops");

    ac->stop();
}

// B3-Qt: the client authors a tab on the daemon; the daemon honors it and
// re-pushes its layout, which the incremental reconciler realizes as a new local
// tab — closing the create loop over a real attach connection.
TEST_CASE("attach authors a tab on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabCount() == 1);

    // A GUI "new tab" in attach mode routes to the daemon (requestRemoteTab ->
    // requestCreateTab) instead of creating a local tab; wait for the two-tab
    // layout to arrive.
    app.manager().createNewTab(win.id);
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout(); l && l->tabs.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(ac->layout()->tabs.size() == 2);

    // Reconcile: the daemon's new tab appears locally (the first is left untouched).
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabCount() == 2);

    ac->stop();
}

namespace
{
/// Writes a configuration file naming @p profile with @p terminalId, as a user saving it would.
void writeConfigWithTerminalId(std::filesystem::path const& file,
                               std::string const& profile,
                               std::string_view terminalId)
{
    contour::test::writeConfigFile(
        file,
        contour::test::profileConfigYaml(profile, {}, std::format("        terminal_id: {}\n", terminalId)));
}
} // namespace

// A session the client creates takes the client's profile from the configuration the GUI holds in
// memory -- what a reload updates, and what a local new tab is built from -- not from the file as saved.
// The sessions that already exist keep theirs.
TEST_CASE("a session the client creates gets the profile the GUI holds in memory", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto testApp = contour::test::ConfiguredTestApp {};
    auto& app = testApp.app();
    auto const profileName = app.profileName();
    auto const setInMemory = [&](vtbackend::VTType id) {
        REQUIRE(app.config().findProfile(profileName) != nullptr);
        app.config().findProfile(profileName)->terminalId.value() = id;
    };
    // The file says something else throughout, so a source that read it would be caught.
    writeConfigWithTerminalId(app.config().configFile, profileName, "VT320");
    setInMemory(vtbackend::VTType::VT420);

    auto timeline = TestTimeline {};
    auto const controllerOwned =
        makeNativeController(daemon.endpoint(), timeline, [&app] { return app.currentEmulationSettings(); });
    auto& controller = *controllerOwned;
    requireConnected(controller);

    auto const tabCount = [&] {
        return daemon.onDaemon(
            [&] { return daemon.host->model().window(daemon.host->windowId())->tabCount(); });
    };
    auto const terminalIdOfTab = [&](int index) {
        return daemon.onDaemon([&] {
            auto const* window = daemon.host->model().window(daemon.host->windowId());
            return daemon.host->terminal(window->tabAt(index)->rootPane()->session())->terminalId();
        });
    };

    controller.requestCreateTab(nullptr, {});
    REQUIRE(waitUntil([&] { return tabCount() == 2; }));
    CHECK(terminalIdOfTab(1) == vtbackend::VTType::VT420);

    // The configuration in memory changes, as a reload changes it; the file does not.
    setInMemory(vtbackend::VTType::VT220);
    controller.requestCreateTab(nullptr, {});
    REQUIRE(waitUntil([&] { return tabCount() == 3; }));
    CHECK(terminalIdOfTab(2) == vtbackend::VTType::VT220);

    // The file changes and nothing reloads it: the next tab still follows the configuration in memory.
    writeConfigWithTerminalId(app.config().configFile, profileName, "VT340");
    controller.requestCreateTab(nullptr, {});
    REQUIRE(waitUntil([&] { return tabCount() == 4; }));
    CHECK(terminalIdOfTab(3) == vtbackend::VTType::VT220);

    // The sessions that already existed keep what they were created with.
    CHECK(terminalIdOfTab(1) == vtbackend::VTType::VT420);

    controller.stop();
}

// B4: a second daemon window (authored via requestRemoteWindow) is reconciled into
// its OWN GUI window — the client maps one OS window per daemon window, each mirroring
// only that window's tabs, with no cross-window session bleed.
TEST_CASE("attach maps each daemon window onto its own GUI window", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("win one"); // the primary daemon window

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win1 { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->windowIds().empty(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->windowIds().size() == 1);

    // A GUI "new window" in attach mode authors it on the daemon (requestRemoteWindow
    // -> requestCreateWindow) rather than opening a stray local one; wait for the
    // daemon to grow and push the second window's layout.
    REQUIRE(ac->requestRemoteWindow({}));
    for (auto i = 0; i < 200 && ac->windowIds().size() < 2; ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->windowIds().size() == 2);
    auto const ids = ac->windowIds();

    // The two windows carry distinct leaf sessions — reconciling them into separate
    // GUI windows must not bind one session into both.
    auto const sessionOf = [&](uint64_t window) {
        return ac->layout(window)->tabs.front().root.session;
    };
    CHECK(sessionOf(ids[0]) != sessionOf(ids[1]));

    // Reconcile each daemon window into its own GUI window.
    contour::test::ScopedController const win2 { app.manager() };
    contour::remote::applyRemoteLayout(app.manager(), win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(app.manager(), win2.id, *ac, ids[1]);

    // Each GUI window mirrors exactly its daemon window's single tab...
    REQUIRE(app.manager().model().window(win1.id)->tabCount() == 1);
    REQUIRE(app.manager().model().window(win2.id)->tabCount() == 1);
    // ...bound to that window's own remote session (via the pane's pty).
    auto const boundSession = [&](vtworkspace::WindowId window) -> std::optional<uint64_t> {
        auto* tab = app.manager().model().window(window)->tabAt(0);
        auto const sessions = app.manager().sessionsOfTab(tab);
        if (sessions.empty())
            return std::nullopt;
        return ac->sessionForPty(&sessions.front()->terminal().device());
    };
    CHECK(boundSession(win1.id) == sessionOf(ids[0]));
    CHECK(boundSession(win2.id) == sessionOf(ids[1]));

    ac->stop();
}

// A "+" clicked in the SECOND attach-mode window must create its tab THERE. The request carried no
// window at all — SessionFactory::requestRemoteTab took no argument, proto::CreateTab had no field,
// and SessionHost::createTab used its own fixed window — so every window's "+" landed in the
// daemon's first one: the tab appeared in the wrong window and the window clicked in gained nothing.
TEST_CASE("attach authors a tab in the window the request came from", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("win one");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win1 { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->windowIds().empty(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->requestRemoteWindow({}));
    for (auto i = 0; i < 200 && ac->windowIds().size() < 2; ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->windowIds().size() == 2);
    auto const ids = ac->windowIds();

    contour::test::ScopedController const win2 { app.manager() };
    contour::remote::applyRemoteLayout(app.manager(), win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(app.manager(), win2.id, *ac, ids[1]);
    REQUIRE(app.manager().model().window(win1.id)->tabCount() == 1);
    REQUIRE(app.manager().model().window(win2.id)->tabCount() == 1);

    // The "+" of the SECOND GUI window.
    app.manager().createNewTab(win2.id);
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->layout(ids[1]); l && l->tabs.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }

    // The daemon grew the tab in the second window, and left the first alone.
    REQUIRE(ac->layout(ids[1]).has_value());
    CHECK(ac->layout(ids[1])->tabs.size() == 2);
    REQUIRE(ac->layout(ids[0]).has_value());
    CHECK(ac->layout(ids[0])->tabs.size() == 1);

    ac->stop();
}

// Regression: closing a mirrored tab must not resurrect it. Before the fix,
// unbind() only forgot the binding, so the still-live remote session's next
// delta re-registered it as pending and re-adopted a fresh tab indefinitely.
TEST_CASE("a closed mirrored tab does not resurrect on later remote output", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("first line");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(controller.pendingCount() == 1);

    // Bind the pending remote session to a local tab, then close that tab.
    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    auto pane = std::make_unique<MirrorPane>(route.router, std::move(pty));
    CHECK(pane->shows("first line"));
    CHECK(controller.pendingCount() == 0);
    pane.reset(); // the terminal destroyed its pty: the user closed the tab

    // The remote session lives on (nothing closed it) and keeps producing
    // output; a pre-fix controller would re-register it as pending here.
    daemon.onDaemon([&] {
        daemon.host->terminal(session)->writeToScreen("second line\r\n");
        return 0;
    });

    // Give the daemon's 20ms debounce ample time to push at least one more
    // delta, and confirm the closed session never came back as pending.
    auto resurrected = false;
    for ([[maybe_unused]] auto const iteration: std::views::iota(0, 50))
    {
        std::this_thread::sleep_for(10ms);
        if (controller.pendingCount() != 0)
        {
            resurrected = true;
            break;
        }
    }
    CHECK(!resurrected);
    CHECK(!controller.canCreateSession());

    controller.stop();
}

namespace
{

/// The connection states a controller went through, as connectionStateChanged() announced them.
struct StateLog
{
    explicit StateLog(contour::remote::NativeController& controller)
    {
        // Direct: it runs on the reactor, where the state changed, so no state is missed.
        QObject::connect(
            &controller, &contour::remote::NativeController::connectionStateChanged, [this, &controller] {
                auto const lock = std::lock_guard { mutex };
                states.push_back(controller.connectionState());
            });
        QObject::connect(&controller,
                         &contour::remote::NativeController::windowsLost,
                         [this](std::vector<uint64_t> const& daemonWindows) {
                             auto const lock = std::lock_guard { mutex };
                             lost.push_back(daemonWindows);
                         });
    }

    /// @return How many times @p state was entered.
    [[nodiscard]] std::size_t count(vthost::client::ConnectionState state) const
    {
        auto const lock = std::lock_guard { mutex };
        return static_cast<std::size_t>(std::ranges::count(states, state));
    }

    /// @return Every windowsLost() so far.
    [[nodiscard]] std::vector<std::vector<uint64_t>> windowsLost() const
    {
        auto const lock = std::lock_guard { mutex };
        return lost;
    }

    mutable std::mutex mutex;
    std::vector<vthost::client::ConnectionState> states;
    std::vector<std::vector<uint64_t>> lost;
};

} // namespace

TEST_CASE("a restarted listener re-snapshots the same sessions into the same panes",
          "[attach][controller][reconnect]")
{
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("before");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("before"));

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    CHECK(log.count(ConnectionState::Reconnecting) == 1);
    CHECK(controller.connectionState() == ConnectionState::Connected);
    CHECK(log.windowsLost().empty());
    CHECK(pane.shows("before"));

    // The same session reaches the same pane over the new connection.
    daemon.onDaemon([&] {
        daemon.host->terminal(session)->writeToScreen("\r\nafter");
        return 0;
    });
    CHECK(pane.shows("after"));
    CHECK_FALSE(pane.pty().isClosed());
    CHECK(controller.isBound(session.value));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("keys typed while reconnecting are dropped", "[attach][controller][reconnect]")
{
    // Review focus 5: never replayed into whatever shell answers after the reconnect.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("typing");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("typing"));

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Reconnecting; }));
    std::ignore = pane.pty().write("ls\r");
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }, 1000));

    // What is typed now arrives; what was typed while down never does, not even behind it.
    std::ignore = pane.pty().write("pwd\r");
    CHECK(waitUntil([&] { return daemon.ptyInput(session).contains("pwd\r"); }));
    CHECK(daemon.ptyInput(session) == "pwd\r");

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a pane closed while reconnecting is closed on the daemon once it answers again",
          "[attach][controller][reconnect]")
{
    // A close is an intent that outlives the gap, unlike a keystroke: dropped, the shell would keep running
    // on the daemon, tombstoned here and so never shown by this client again.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    std::ignore = daemon.seedSession("closed");
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.pendingCount() == 2; }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    auto const closed = controller.sessionForPty(&pane.pty());
    REQUIRE(closed.has_value());
    REQUIRE(liveSessions() == 2);

    daemon.refuseConnections();
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Reconnecting; }));
    route.router.requestRemoteClose(&pane.pty());
    // Long enough for the close to reach the reactor while no connection is up; nothing reached the daemon.
    std::this_thread::sleep_for(100ms);
    CHECK(controller.connectionState() == ConnectionState::Reconnecting);
    CHECK(liveSessions() == 2);

    daemon.serveAgain();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    CHECK(waitUntil([&] { return liveSessions() == 1; }));
    CHECK(daemon.onDaemon(
        [&] { return daemon.host->terminal(vtworkspace::SessionId { *closed }) == nullptr; }));

    controller.stop();
    pane.terminal.reset();
}

namespace contour::remote
{

/// Reaches the reactor-owned client of a controller (@see NativeController's friend declaration).
struct NativeControllerTestAccess
{
    /// Runs @p action on the reactor thread with the controller's current client, and waits for it.
    template <typename F>
    static void onReactor(contour::remote::NativeController& controller, F action)
    {
        auto done = std::promise<void> {};
        controller._reactor.post([&] {
            action(*controller._client);
            done.set_value();
        });
        done.get_future().wait();
    }

    /// Makes the reactor's run end with an exception, as a decode error escaping the root task would.
    static void killReactor(contour::remote::NativeController& controller)
    {
        controller._reactor.post([] { throw std::runtime_error("reactor died"); });
    }

    /// Ends the current client's connection and, in the same reactor turn, closes @p session's pane.
    static void closeWhileEnding(contour::remote::NativeController& controller, uint64_t session)
    {
        onReactor(controller, [&](auto& client) {
            client.detach();
            controller.closePaneOnReactor(session);
        });
    }
};

} // namespace contour::remote

TEST_CASE("a pane closed as its connection ends is closed once the same daemon answers again",
          "[attach][controller][reconnect]")
{
    // The close is issued with the client still current but its connection ending, so the client writes
    // nothing; the state change that would park it in _pendingCloses has not been delivered yet.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    std::ignore = daemon.seedSession("closed");
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.pendingCount() == 2; }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    auto const closed = controller.sessionForPty(&pane.pty());
    REQUIRE(closed.has_value());
    REQUIRE(liveSessions() == 2);

    // The close lands on the reactor in the same turn as the detach, ahead of the state change it provokes.
    contour::remote::NativeControllerTestAccess::closeWhileEnding(controller, *closed);

    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 2000));
    CHECK(waitUntil([&] { return liveSessions() == 1; }));
    CHECK(daemon.onDaemon(
        [&] { return daemon.host->terminal(vtworkspace::SessionId { *closed }) == nullptr; }));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("the daemon's echo report reaches the pane's device and its observer, with no output after it",
          "[attach][controller][inputecho]")
{
    // sudo may turn echo off after its prompt printed: nothing follows, so the daemon's idle check tells the
    // client (FlushPolicy::inputEchoCheck, short here: it is a setting, not a sleep), and the controller
    // routes the report to the pane's device and wakes its owner (vtpty::Pty::setInputEchoObserver).
    auto services = vthost::NativeSessionServices {};
    services.flush.inputEchoCheck = std::chrono::milliseconds { 2 };
    auto daemon = DaemonFixture { {}, services };
    auto const session = daemon.seedSession("prompt");
    auto const setEchoOnDaemon = [&](vtpty::InputEcho echo) {
        daemon.onDaemon([&] {
            auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
            dynamic_cast<vtpty::MockPty&>(tapped.inner()).setInputEcho(echo);
            return 0;
        });
    };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("prompt"));
    REQUIRE(controller.isBound(session.value));

    auto woken = std::atomic<int> { 0 };
    pane.pty().setInputEchoObserver([&woken] { ++woken; }); // runs on the reactor thread
    CHECK(pane.pty().inputEcho() == vtpty::InputEcho::Unknown);

    setEchoOnDaemon(vtpty::InputEcho::Hidden);
    REQUIRE(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Hidden; }));
    CHECK(woken.load() == 1);

    setEchoOnDaemon(vtpty::InputEcho::Echoed);
    REQUIRE(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Echoed; }));
    CHECK(woken.load() == 2);

    pane.pty().setInputEchoObserver({}); // before the counter goes
    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a pane's echo state is not trusted while the connection is down",
          "[attach][controller][inputecho][reconnect]")
{
    // Review M1: with typed input dropped and nothing reading, a stale Hidden would keep the lock and secure
    // input on. The pane reads Unknown while reconnecting; the reconnect's snapshot states the real one.
    auto services = vthost::NativeSessionServices {};
    services.flush.inputEchoCheck = std::chrono::milliseconds { 2 };
    auto daemon = DaemonFixture { {}, services };
    auto const session = daemon.seedSession("prompt");
    daemon.onDaemon([&] {
        auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
        dynamic_cast<vtpty::MockPty&>(tapped.inner()).setInputEcho(vtpty::InputEcho::Hidden);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("prompt"));
    REQUIRE(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Hidden; }));

    daemon.refuseConnections();
    CHECK(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Unknown; }));
    daemon.serveAgain();
    CHECK(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Hidden; }, 3000));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a restarted daemon's new pane does not inherit the old run's echo state",
          "[attach][controller][inputecho][reconnect]")
{
    // Review M1: session ids count from 1 in every run of a daemon, so a new pane with the old id must not
    // be handed the old pane's Hidden -- the daemon says nothing of a PTY that reads Unknown.
    auto services = vthost::NativeSessionServices {};
    services.flush.inputEchoCheck = std::chrono::milliseconds { 2 };
    auto daemon = DaemonFixture { {}, services };
    auto const session = daemon.seedSession("old run");
    daemon.onDaemon([&] {
        auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
        dynamic_cast<vtpty::MockPty&>(tapped.inner()).setInputEcho(vtpty::InputEcho::Hidden);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto old = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(old.shows("old run"));
    REQUIRE(waitUntil([&] { return old.pty().inputEcho() == vtpty::InputEcho::Hidden; }));

    daemon.restartHost();
    REQUIRE(waitUntil([&] { return controller.pendingCount() == 1; }, 3000));
    auto fresh = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    CHECK(fresh.pty().inputEcho() == vtpty::InputEcho::Unknown);

    controller.stop();
    fresh.terminal.reset();
    old.terminal.reset();
}

TEST_CASE("a reconnect takes the handshake of the daemon that answered", "[attach][controller][reconnect]")
{
    // The daemon may have been upgraded or downgraded meanwhile: what was agreed is the new connection's.
    using vthost::client::ConnectionState;
    using vthost::proto::Capability;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("upgraded");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(controller.negotiated().has_value());
    REQUIRE(controller.negotiated()->shared.contains(Capability::TabEdits));

    daemon.whileDisconnected([&] {
        daemon.services.handshake.capabilities =
            vthost::proto::CapabilitySet::of({ Capability::ExtensionPermissions,
                                               Capability::InitialClientArea,
                                               Capability::AttachIntent,
                                               Capability::Heartbeat });
    });
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    REQUIRE(controller.negotiated().has_value());
    CHECK_FALSE(controller.negotiated()->shared.contains(Capability::TabEdits));
    CHECK(controller.negotiated()->shared.contains(Capability::AttachIntent));

    controller.stop();
}

TEST_CASE("windows the daemon no longer has are reported lost", "[attach][controller][reconnect]")
{
    // Review focus 2: the daemon restarted, and none of the windows exists any more.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("old run");
    auto const window = daemon.onDaemon([&] { return daemon.host->windowId().value; });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("old run"));

    // Queued, as the GUI connects it: the lost windows must survive the trip to the GUI thread.
    auto receiver = QObject {};
    auto queued = std::vector<uint64_t> {};
    QObject::connect(
        &controller,
        &contour::remote::NativeController::windowsLost,
        &receiver,
        [&queued](std::vector<uint64_t> const& daemonWindows) { queued = daemonWindows; },
        Qt::QueuedConnection);

    daemon.restartHost();
    REQUIRE(waitUntil([&] { return !log.windowsLost().empty(); }, 1000));
    CHECK(log.windowsLost() == std::vector<std::vector<uint64_t>> { { window } });
    CHECK(waitUntil([&] { return !queued.empty(); }));
    CHECK(queued == std::vector<uint64_t> { window });
    // The Resume found nothing to resume, fell back to a launch, and the new run's first window arrived:
    // its session awaits a pane of its own, rather than the old pane that had the same id.
    CHECK(controller.connectionState() == ConnectionState::Connected);
    CHECK(waitUntil([&] { return controller.pendingCount() == 1; }));
    CHECK(controller.layout().has_value());

    // The old pane is detached: what it is sent goes nowhere, least of all to the new session of its id.
    std::ignore = pane.pty().write("stale\r");
    auto const fresh = daemon.onDaemon([&] {
        return daemon.host->model().window(daemon.host->windowId())->activeTab()->rootPane()->session();
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(daemon.ptyInput(fresh).contains("stale"));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a client following every window still gets new windows after a reconnect",
          "[attach][controller][reconnect]")
{
    // A reconnect must not narrow the view: AllWindows comes back as AllWindows, not as Resume of the windows
    // it had, so a window another client creates afterwards still reaches it.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline); // AllWindows
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.windowIds().size() == 1; }));

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    REQUIRE(waitUntil([&] { return controller.windowIds().size() == 1; }));

    // Claimed by another client: a client following only listed windows would never see it.
    auto const otherClient = vthost::SessionStreamEvents {};
    auto const created =
        daemon.onDaemon([&] { return daemon.host->createWindow({}, &otherClient)->id().value; });
    CHECK(waitUntil([&] { return std::ranges::contains(controller.windowIds(), created); }));
    CHECK(log.windowsLost().empty());

    controller.stop();
}

TEST_CASE("a window gone from the same daemon is reported lost, the others kept",
          "[attach][controller][reconnect]")
{
    // Same instance: the ids still mean what they meant, and the lost windows are the ones not followed
    // again.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto const kept = daemon.onDaemon([&] { return daemon.host->windowId().value; });
    auto const gone = daemon.onDaemon([&] { return daemon.host->createWindow()->id().value; });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.windowIds().size() == 2; }));

    // Closed on the daemon while this client was away: its only session ended.
    daemon.whileDisconnected([&] {
        auto* window = daemon.host->model().window(vtworkspace::WindowId { gone });
        daemon.host->handleSessionExit(window->tabAt(0)->rootPane()->session());
    });
    REQUIRE(waitUntil([&] { return !log.windowsLost().empty(); }, 1000));
    CHECK(log.windowsLost() == std::vector<std::vector<uint64_t>> { { gone } });
    CHECK(controller.connectionState() == ConnectionState::Connected);
    // Still followed (every window is), but with nothing in it: no OS window is opened for it.
    CHECK(controller.layout(kept).has_value());
    auto const emptied = controller.layout(gone);
    CHECK((!emptied || emptied->tabs.empty()));

    controller.stop();
}

TEST_CASE("a daemon that states no instance is judged by its window ids alone",
          "[attach][controller][reconnect]")
{
    // Instance 0 on either side is the old rule, mapped − followed: the restarted daemon's window 1 passes
    // for the one this client showed.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture { {}, {}, /*instance=*/0 };
    std::ignore = daemon.seedSession("unnamed run");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));

    daemon.restartHost();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    CHECK(waitUntil([&] { return controller.layout().has_value(); }));
    CHECK(log.windowsLost().empty());

    controller.stop();
}

TEST_CASE("stop() during a slow daemon start returns promptly", "[attach][controller][threading]")
{
    // The start runs on the transport's worker, not on the reactor stop() has to join: quitting while a
    // daemon is being started does not wait for it to serve.
    auto started = std::atomic<bool> { false };
    auto release = std::promise<void> {};
    auto const transport = std::make_shared<vthost::UnixTransportFactory>(
        "/nonexistent/contour/control", [&started, released = release.get_future().share()] {
            started = true;
            std::ignore = released.wait_for(10s);
            return std::expected<void, vthost::TransportError> {};
        });
    auto timeline = TestTimeline {};
    auto controller = contour::remote::NativeController {
        transport,
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Launch, .windows = {} },
        std::string {},
        std::nullopt,
        {},
        core::platform::defaultSteadyClock(),
        timeline.timeline
    };
    controller.startConnecting();
    REQUIRE(waitUntil([&] { return started.load(); }));

    auto const began = std::chrono::steady_clock::now();
    controller.stop();
    auto const stoppedIn = std::chrono::steady_clock::now() - began;
    release.set_value();

    CHECK(stoppedIn < 1s);
    CHECK(controller.connectionState() == vthost::client::ConnectionState::Stopped);
}

TEST_CASE("NativeController shows the refusal text", "[contour][remote]")
{
    struct Row
    {
        std::string_view name;
        vthost::HandshakePolicy policy;
        vthost::proto::VersionRange daemonVersions;
    };
    auto const rows = std::array {
        Row { .name = "a daemon of other versions",
              .policy = { .versions = { .min = 6, .max = 8 } },
              .daemonVersions = { .min = 6, .max = 8 } },
        // The ranges overlap: the daemon refused for the token, and the text says so.
        Row { .name = "a daemon wanting another token",
              .policy = { .acceptedTokens = { { .token = "right" } } },
              .daemonVersions = vthost::proto::SupportedVersions },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto daemon = DaemonFixture { {}, vthost::NativeSessionServices { .handshake = row.policy } };
        std::ignore = daemon.seedSession("refused");

        auto timeline = TestTimeline {};
        auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
        auto& controller = *controllerOwned;
        auto const connected = controller.connectAndWait(10s);
        REQUIRE_FALSE(connected.has_value());
        // The refusal, naming the daemon it came from.
        CHECK(connected.error()
              == std::format("{} (tls://127.0.0.1:{})",
                             vthost::proto::describe(
                                 { .ours = vthost::proto::SupportedVersions, .theirs = row.daemonVersions },
                                 vthost::proto::PeerRole::Daemon),
                             daemon.port));
        controller.stop();
    }
}

TEST_CASE("NativeController.reconnect.afterARefusal", "[attach][controller][reconnect][manual]")
{
    using contour::session::ReconnectError;
    using vthost::client::ConnectionState;
    // A daemon that wants another token refuses the attach for good; once it accepts, a reconnect from the
    // footer reaches it on a new supervised run.
    auto daemon = DaemonFixture {
        {}, vthost::NativeSessionServices { .handshake = { .acceptedTokens = { { .token = "right" } } } }
    };
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    CHECK(controller.reconnectSupport() == contour::session::ReconnectSupport::Supported);

    auto const refused = controller.connectAndWait(10s);
    REQUIRE_FALSE(refused.has_value());
    CHECK(controller.connectionState() == ConnectionState::Failed); // a first connect is not retried
    CHECK_FALSE(controller.failure().empty());

    // The reactor has recorded the failure, but the GUI has not yet handled connectionClosed: its stop() has
    // not run, and a reconnect now would start a run that the stale handler then stops.
    daemon.serveAgain([&] { daemon.services = {}; });
    auto const early = controller.reconnect();
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error() == ReconnectError::ConnectionEnding);
    CHECK(controller.connectionState() == ConnectionState::Failed);

    controller.stop(); // what the GUI's connectionClosed handler does
    auto const started = controller.reconnect();
    REQUIRE(started.has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    CHECK(controller.failure().empty());

    // While the run is up there is nothing to start.
    auto const again = controller.reconnect();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ReconnectError::AlreadyConnected);

    controller.stop();
}

TEST_CASE("NativeController.reconnect.afterTheReactorDied", "[attach][controller][reconnect][manual]")
{
    using vthost::client::ConnectionState;
    // A reactor that dies tells its owner the connection closed, so the owner stop()s the controller and
    // Reconnect is not refused for good.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto closed = std::atomic<int> { 0 };
    auto const connection = QObject::connect(
        &controller,
        &contour::remote::NativeController::connectionClosed,
        &controller,
        [&] { ++closed; },
        Qt::DirectConnection);

    contour::remote::NativeControllerTestAccess::killReactor(controller);
    REQUIRE(waitUntil([&] { return closed.load() == 1; }));
    CHECK(controller.connectionState() == ConnectionState::Failed);
    CHECK_FALSE(controller.failure().empty());

    controller.stop(); // what the GUI's connectionClosed handler does
    QObject::disconnect(connection);
    REQUIRE(controller.reconnect().has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    controller.stop();
}

TEST_CASE("NativeController.reconnect.restartedReactorDiesBeforeConnecting",
          "[attach][controller][reconnect][manual]")
{
    using vthost::client::ConnectionState;
    // A restarted run is Connecting from the moment Reconnect returns: a reactor that dies before it enters
    // Connecting itself must still tell its owner the connection closed, not hide behind the old Failed.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto closed = std::atomic<int> { 0 };
    auto const connection = QObject::connect(
        &controller,
        &contour::remote::NativeController::connectionClosed,
        &controller,
        [&] { ++closed; },
        Qt::DirectConnection);

    contour::remote::NativeControllerTestAccess::killReactor(controller);
    REQUIRE(waitUntil([&] { return closed.load() == 1; }));
    CHECK(controller.connectionState() == ConnectionState::Failed);
    controller.stop(); // what the GUI's connectionClosed handler does

    REQUIRE(controller.reconnect().has_value());
    CHECK(controller.connectionState() != ConnectionState::Failed);
    contour::remote::NativeControllerTestAccess::killReactor(controller);
    CHECK(waitUntil([&] { return closed.load() == 2; }));

    controller.stop();
    QObject::disconnect(connection);
    REQUIRE(controller.reconnect().has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    controller.stop();
}

TEST_CASE("NativeController.reconnect.afterAStop", "[attach][controller][reconnect][manual]")
{
    using vthost::client::ConnectionState;
    // The app stops the controller when its connection ends for good (closing every mirror pty); a
    // reconnect after that starts the run again on a fresh reactor.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    controller.stop();
    CHECK(controller.connectionState() == ConnectionState::Stopped);

    REQUIRE(controller.reconnect().has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    controller.stop();
}

TEST_CASE("NativeController.reconnect.neverAttached", "[attach][controller][reconnect][manual]")
{
    auto daemon = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto const result = controllerOwned->reconnect();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == contour::session::ReconnectError::NotAttached);
}

TEST_CASE("NativeController.reconnect.takesAPaneBackByItsRemoteSession",
          "[attach][controller][reconnect][manual]")
{
    using contour::session::ReconnectBinding;
    // The layout the daemon pushes names the session a lost pane mirrored: the next pty binds to it again,
    // keeping the id. A pane whose session the daemon no longer has is Gone.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    auto const session = controller.sessionForPty(&pane.pty());
    REQUIRE(session.has_value());

    CHECK(controller.bindForReconnect(&pane.pty()) == ReconnectBinding::Bound);
    // Bound for the next pty, which therefore may be made though nothing is pending.
    CHECK(controller.canCreateSession());
    auto replacement = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    CHECK(controller.sessionForPty(&replacement.pty()) == session);
    CHECK_FALSE(controller.canCreateSession());

    // A pty the controller never handed out names no session.
    auto stranger = vtpty::MockPty { cells(80, 24) };
    CHECK(controller.bindForReconnect(&stranger) == ReconnectBinding::Gone);

    controller.stop();
    replacement.terminal.reset();
    pane.terminal.reset();
}

TEST_CASE("NativeController.reconnect.cancelledBindingLeavesNothingForTheNextPty",
          "[attach][controller][reconnect][manual]")
{
    using contour::session::ReconnectBinding;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };

    REQUIRE(controller.bindForReconnect(&pane.pty()) == ReconnectBinding::Bound);
    REQUIRE(controller.canCreateSession());
    controller.cancelReconnectBinding();
    CHECK_FALSE(controller.canCreateSession());

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("attach controller reports an unreachable daemon", "[attach][controller]")
{
    // Port 1 on loopback has nothing listening: connect is refused before any TLS.
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} }, timeline);
    auto& controller = *controllerOwned;
    auto const connected = controller.connectAndWait(2s);
    REQUIRE(!connected.has_value());
    CHECK(!connected.error().empty());
}

namespace
{
/// A transport whose every attempt fails with one message.
class FailingTransportFactory final: public vthost::ITransportFactory
{
  public:
    FailingTransportFactory(std::string where, std::string message):
        _where { std::move(where) }, _message { std::move(message) }
    {
    }

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& /*loop*/) override
    {
        return fail(_message);
    }

    [[nodiscard]] std::string describe() const override { return _where; }

    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override
    {
        return vthost::PeerTrust::Verified;
    }

  private:
    [[nodiscard]] static core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    fail(std::string message)
    {
        co_return std::unexpected { vthost::TransportError { .kind = vthost::TransportErrorKind::Refused,
                                                             .message = std::move(message) } };
    }

    std::string _where;
    std::string _message;
};
} // namespace

TEST_CASE("NativeController names the transport of a failure once", "[attach][controller][hosts]")
{
    struct Row
    {
        std::string_view name;
        std::string message;
        std::string expected;
    };
    auto const rows = std::array {
        Row { .name = "a message that names no host gets it appended",
              .message = "connection refused",
              .expected = "connection refused (ssh://me@homepc)" },
        Row { .name = "an ssh message names its host already",
              .message = "ssh could not reach ssh://me@homepc: Connection refused",
              .expected = "ssh could not reach ssh://me@homepc: Connection refused" },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto timeline = TestTimeline {};
        auto controller = contour::remote::NativeController {
            std::make_shared<FailingTransportFactory>("ssh://me@homepc", row.message),
            vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows,
                                          .windows = {} },
            std::string {},
            std::nullopt,
            {},
            core::platform::defaultSteadyClock(),
            timeline.timeline
        };
        auto const connected = controller.connectAndWait(10s);
        REQUIRE_FALSE(connected.has_value());
        CHECK(connected.error() == row.expected);
        controller.stop();
    }
}

namespace
{
/// An ssh child that never answers: its stdout stays open and silent, like a remote stuck in its login.
class SilentStdioChild final: public vthost::IStdioChild
{
  public:
    explicit SilentStdioChild(std::unique_ptr<core::net::ISocket> remoteEnd):
        _remoteEnd { std::move(remoteEnd) }
    {
    }

    [[nodiscard]] std::string stderrTail() const override { return {}; }
    [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds /*wait*/) override
    {
        return std::nullopt;
    }
    void terminate() noexcept override { _remoteEnd.reset(); }

  private:
    std::unique_ptr<core::net::ISocket> _remoteEnd; ///< Held open, written never.
};

/// Spawns SilentStdioChild, on an in-process socket pair.
class SilentStdioSpawner final: public vthost::IStdioSpawner
{
  public:
    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& loop, std::span<std::string const> /*argv*/) override
    {
        auto pair = core::net::testing::makeSocketPair(loop);
        if (!pair)
            return std::unexpected { vthost::StdioSpawnError { .kind = vthost::SpawnFailure::SystemError,
                                                               .message = "no socket pair" } };
        return vthost::SpawnedStdioChild { .transport = std::move(pair->first),
                                           .process =
                                               std::make_unique<SilentStdioChild>(std::move(pair->second)) };
    }
};
} // namespace

TEST_CASE("a silent ssh remote is reported by the ssh transport rather than as an attach timeout",
          "[attach][controller][hosts]")
{
    // The attach's clock starts before the transport's: with the same bound for both, the attach would
    // time out first and say nothing about the host. Above MinimumConnectAwait, so the floor decides nothing.
    auto constexpr TransportBound = std::chrono::milliseconds { 2500 };
    static_assert(TransportBound > contour::remote::MinimumConnectAwait);
    auto timeline = TestTimeline {};
    auto controller = contour::remote::NativeController {
        std::make_shared<vthost::SshTransportFactory>(
            vthost::SshTarget { .user = "me", .host = "homepc", .port = std::nullopt },
            std::make_shared<SilentStdioSpawner>(),
            TransportBound),
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows, .windows = {} },
        std::string {},
        std::nullopt,
        {},
        core::platform::defaultSteadyClock(),
        timeline.timeline
    };
    auto const connected = controller.connectAndWait(contour::cli::sshAttachBudget(TransportBound));
    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error() == "ssh://me@homepc did not answer within 3 seconds");
    controller.stop();
}

// B4: the manager's attach-window seam (consumeAttachWindow) routes a freshly-spawned
// window to the binder ContourGuiApp installs — the QML-side hook that lets a spawned
// window adopt a daemon window instead of creating a fresh first tab. Without a binder
// it is inert (like an ordinary local window).
TEST_CASE("consumeAttachWindow routes a spawned window to the installed binder", "[attach][factory]")
{
    auto app = contour::test::TestApp { std::make_unique<RefusingFactory>() };
    auto controller = contour::test::ScopedController { app.manager() };

    // No binder installed (an ordinary local run): the seam is a no-op.
    CHECK_FALSE(app.manager().consumeAttachWindow(controller.controller));
    CHECK_FALSE(app.manager().consumeAttachWindow(nullptr));

    // An installed binder is consulted with the exact window being bootstrapped, and its
    // verdict is returned verbatim.
    contour::window::WindowController* seen = nullptr;
    auto verdict = false;
    app.manager().setAttachWindowBinder([&](contour::window::WindowController* c) {
        seen = c;
        return verdict;
    });
    CHECK_FALSE(app.manager().consumeAttachWindow(controller.controller));
    CHECK(seen == controller.controller);
    verdict = true;
    CHECK(app.manager().consumeAttachWindow(controller.controller));
    // A null controller never reaches the binder.
    seen = nullptr;
    CHECK_FALSE(app.manager().consumeAttachWindow(nullptr));
    CHECK(seen == nullptr);
}

TEST_CASE("a refusing session factory blocks every creation entry point", "[attach][factory]")
{
    auto app = contour::test::TestApp { std::make_unique<RefusingFactory>() };
    auto controller = contour::test::ScopedController { app.manager() };

    CHECK(app.manager().createSessionInBackground(controller.id) == nullptr);
    CHECK(app.manager().model().window(controller.id)->tabCount() == 0);
}

// Every SessionFactory verb has to survive the trip through RoutingSessionFactory, which the app
// installs permanently and re-routes when attach mode starts. A verb it forgets to forward is not a
// compile error and not a test failure anywhere else -- the delegate simply never hears about it.
//
// `bindTerminal` was exactly that: it reached the router and stopped, so a daemon-hosted pane in the
// real GUI never got its ScreenMirror and rendered nothing at all, while every headless test passed
// because they call NativeController::bindTerminal directly.
TEST_CASE("the routing session factory forwards every verb to its delegate", "[attach][factory]")
{
    struct RecordingFactory final: contour::session::SessionFactory
    {
        int ptys = 0;
        int tabs = 0;
        int splits = 0;
        int windows = 0;
        int closes = 0;
        int binds = 0;
        int ratios = 0;
        double lastRatio = 0.0;
        vtpty::Pty const* boundPty = nullptr;
        vtpty::Pty const* tabPty = nullptr; ///< The window the last tab request named.
        vtbackend::Terminal* boundTerminal = nullptr;

        [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string>,
                                                            std::optional<vtbackend::PageSize>,
                                                            std::optional<vtpty::Process::ExecInfo>,
                                                            std::optional<std::string>) override
        {
            ++ptys;
            return nullptr;
        }
        [[nodiscard]] bool canCreateSession() const noexcept override { return false; }
        [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
        {
            return contour::session::SessionHosting::Remote;
        }
        [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty,
                                            contour::session::RemoteSpawn const&) override
        {
            ++tabs;
            tabPty = actingPty;
            return true;
        }
        [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const*,
                                              bool,
                                              contour::session::RemoteSpawn const&) override
        {
            ++splits;
            return true;
        }
        void reportSplitRatio(vtpty::Pty const*, vtpty::Pty const*, double ratio) override
        {
            ++ratios;
            lastRatio = ratio;
        }
        [[nodiscard]] bool requestRemoteWindow(contour::session::RemoteSpawn const&) override
        {
            ++windows;
            return true;
        }
        void requestRemoteClose(vtpty::Pty const*) override { ++closes; }
        void bindTerminal(vtpty::Pty const* pty,
                          vtbackend::Terminal& terminal,
                          contour::session::InterruptionSink const& /*interruptions*/) override
        {
            ++binds;
            boundPty = pty;
            boundTerminal = &terminal;
        }
    };

    auto delegate = RecordingFactory {};
    auto router = contour::remote::RoutingSessionFactory { std::make_unique<RecordingFactory>() };
    router.setDelegate(&delegate);

    auto events = vtbackend::Terminal::NullEvents {};
    auto settings = vtbackend::Settings {};
    auto pty = std::make_unique<vtpty::MockPty>(settings.pageSize);
    auto* device = pty.get();
    auto terminal = vtbackend::Terminal { events,
                                          core::defaultEnvironment(),
                                          core::platform::NativeFileSystem::instance(),
                                          std::move(pty),
                                          std::move(settings),
                                          std::nullopt,
                                          vtbackend::TerminalClocks::system(),
                                          vtbackend::randomSemanticBlockTokens() };

    CHECK(router.createPty(std::nullopt) == nullptr);
    CHECK_FALSE(router.canCreateSession());
    CHECK(router.hosting() == contour::session::SessionHosting::Remote);
    CHECK(router.requestRemoteTab(device, {}));
    CHECK(router.requestRemoteSplit(device, true, {}));
    CHECK(router.requestRemoteWindow({}));
    router.requestRemoteClose(device);
    router.reportSplitRatio(device, device, 0.7);
    router.bindTerminal(device, terminal, {});

    CHECK(delegate.ptys == 1);
    CHECK(delegate.tabs == 1);
    CHECK(delegate.tabPty == device); // the acting pane names the target WINDOW; it must be routed
    CHECK(delegate.splits == 1);
    CHECK(delegate.windows == 1);
    CHECK(delegate.closes == 1);
    CHECK(delegate.ratios == 1);
    CHECK(delegate.lastRatio == 0.7);
    CHECK(delegate.binds == 1);
    CHECK(delegate.boundPty == device);
    CHECK(delegate.boundTerminal == &terminal);
}

namespace
{

/// @return A restore of two windows holding three and four single-pane tabs.
[[nodiscard]] vthost::WorkspaceRestore twoWindowsSevenTabs()
{
    auto snapshot = vthost::WorkspaceSnapshot {};
    for (auto const tabs: { 3, 4 })
    {
        auto window = vthost::WindowSnapshot {};
        for (auto const _: std::views::iota(0, tabs))
            window.tabs.push_back(vthost::TabSnapshot {});
        snapshot.windows.push_back(std::move(window));
    }
    return vthost::WorkspaceRestore { .snapshot = std::move(snapshot) };
}

} // namespace

TEST_CASE("a controller attached after a daemon restore is told what came back, once",
          "[attach][controller][restore]")
{
    auto daemon = DaemonFixture { {}, {}, 1, {}, twoWindowsSevenTabs() };
    auto timeline = TestTimeline {};

    auto const firstOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& first = *firstOwned;
    // The signals fire on the reactor thread; queued delivery keeps what they record on this thread, in
    // the order they were raised.
    auto const context = QObject {};
    auto restoredTold = std::vector<std::pair<quint32, quint32>> {};
    auto stateSignals = size_t { 0 };
    auto stateSeenByLastSignal = vthost::client::ConnectionState::Connecting;
    QObject::connect(
        &first,
        &contour::remote::NativeController::workspaceRestored,
        &context,
        [&](quint32 windows, quint32 tabs) { restoredTold.emplace_back(windows, tabs); },
        Qt::QueuedConnection);
    QObject::connect(
        &first,
        &contour::remote::NativeController::connectionStateChanged,
        &context,
        [&] {
            ++stateSignals;
            stateSeenByLastSignal = first.connectionState();
        },
        Qt::QueuedConnection);
    REQUIRE(first.connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return restoredTold.size() == 1; }));
    CHECK(restoredTold.front() == std::pair<quint32, quint32> { 2, 7 });

    // A second client of the same daemon run is told nothing.
    auto const secondOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& second = *secondOwned;
    auto untold = std::vector<std::pair<quint32, quint32>> {};
    QObject::connect(
        &second,
        &contour::remote::NativeController::workspaceRestored,
        &context,
        [&](quint32 windows, quint32 tabs) { untold.emplace_back(windows, tabs); },
        Qt::QueuedConnection);
    REQUIRE(second.connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return second.layout().has_value(); }));
    CHECK(untold.empty());

    // Nor does the first one's reconnect to it raise the summary again. The reactor raises the summary (if
    // it were to) just before the state signal that announces Connected, and queued signals arrive in
    // order, so once this thread has seen that state signal the summary has had its chance. A reconnect
    // raises at least two state signals: Reconnecting, then Connected.
    REQUIRE(waitUntil([&] {
        return first.connectionState() == vthost::client::ConnectionState::Connected
               && stateSeenByLastSignal == vthost::client::ConnectionState::Connected;
    }));
    auto const signalsBeforeDrop = stateSignals;
    daemon.dropConnections();
    REQUIRE(waitUntil([&] {
        return stateSignals >= signalsBeforeDrop + 2
               && first.connectionState() == vthost::client::ConnectionState::Connected
               && stateSeenByLastSignal == vthost::client::ConnectionState::Connected;
    }));
    CHECK(restoredTold.size() == 1);

    first.stop();
    second.stop();
}

TEST_CASE("a hello that restored nothing raises no summary", "[attach][controller][restore]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("plain");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const context = QObject {};
    auto told = std::vector<std::pair<quint32, quint32>> {};
    QObject::connect(
        &controller,
        &contour::remote::NativeController::workspaceRestored,
        &context,
        [&](quint32 windows, quint32 tabs) { told.emplace_back(windows, tabs); },
        Qt::QueuedConnection);
    REQUIRE(controller.connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    CHECK(told.empty());
    controller.stop();
}
