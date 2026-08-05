// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/NativeController.h>
#include <contour/remote/RoutingSessionFactory.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtpty/MockPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <contour/remote/RemoteLayout.h>

#include <cstdint>

#include <coro/Cancellation.hpp>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <net/Tls.h>
#include <vthost/ConnectionAcceptor.h>
#include <vthost/NativeSession.h>
#include <vthost/SessionHost.h>
#include <vthost/SocketPath.h>
#include <vthost/TappingPty.h>
#include <vthost/client/LayoutReconstruction.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/SessionModel.h>
#include <vtworkspace/Tab.h>

using namespace std::chrono_literals;

namespace
{

/// An in-process `contour daemon` (native protocol) on its own thread, serving
/// TLS over a LOOPBACK TCP socket on an EPHEMERAL port. Using TCP (the same
/// transport `client --connect-tcp` uses) rather than AF_UNIX lets these
/// end-to-end tests run on every platform, Windows included.
struct DaemonFixture
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    std::unique_ptr<vthost::SessionHost> host;
    std::unique_ptr<vthost::ConnectionAcceptor> server;
    std::uint16_t port = 0; ///< The OS-assigned loopback port the daemon listens on.
    std::thread thread;
    bool cancelled = false; ///< Whether teardown unwound the accept loop.

    DaemonFixture()
    {
        host = std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
            vtbackend::Settings {},
            crispy::defaultEnvironment(),
            /*startPumps=*/false);
        auto listener = net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        port = (*listener)->localPort();
        // Encrypt each accepted socket (server-side TLS, self-signed) before the
        // native protocol runs over it — the daemon's real TCP path. This exercises
        // the two-reactor TLS handshake (client + daemon on independent reactors),
        // which NativeClient's concurrent read+write drives.
        auto tls = net::makeSelfSignedServerContext();
        REQUIRE(tls.has_value());
        auto native = vthost::makeNativeHandler(loop, *host);
        auto const& context = *tls;
        auto handler = [context, native](vthost::ConnectionId id, std::unique_ptr<net::ISocket> socket) {
            return native(std::move(id), context->wrap(std::move(socket)));
        };
        server = std::make_unique<vthost::ConnectionAcceptor>(loop, "test", std::move(*listener), handler);
        thread = std::thread { [this] {
            try
            {
                loop.blockOn(server->serve());
            }
            catch (coro::OperationCancelled const&)
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
    contour::RoutingSessionFactory router { std::make_unique<UnusedDefaultFactory>() };

    explicit AttachRoute(contour::NativeController& controller) { router.setDelegate(&controller); }

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
    MirrorPane(contour::session::SessionFactory& factory, std::unique_ptr<vtpty::Pty> pty)
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pty->pageSize();
        auto* device = pty.get();
        terminal = std::make_unique<vtbackend::Terminal>(events,
                                                         crispy::defaultEnvironment(),
                                                         std::move(pty),
                                                         std::move(settings),
                                                         std::chrono::steady_clock::now());
        factory.bindTerminal(device, *terminal);
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
void requireConnected(contour::RemoteController& controller, std::chrono::milliseconds timeout = 10s)
{
    auto const connected = controller.connectAndWait(timeout);
    INFO("connectAndWait: " << (connected ? std::string { "ok" } : connected.error()));
    REQUIRE(connected.has_value());
}

/// Connects @p controller, waits for the daemon's first layout, and realizes it into @p window.
/// @return The realized tab, or nullptr if no layout ever arrived.
[[nodiscard]] vtworkspace::Tab* attachAndRealize(contour::NativeController& controller,
                                                 contour::session::TerminalSessionManager& manager,
                                                 vtworkspace::WindowId window)
{
    requireConnected(controller);
    if (!waitUntil([&] { return controller.layout().has_value(); }))
        return nullptr;
    contour::applyRemoteLayout(manager, window, controller);
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

    auto controller = contour::NativeController { daemon.endpoint(), std::nullopt };
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());
    REQUIRE(controller.pendingCount() == 1);
    REQUIRE(controller.canCreateSession());

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

    auto controller = contour::NativeController { daemon.endpoint(), std::nullopt };
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
    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);

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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    auto controller = contour::NativeController { daemon.endpoint(), std::nullopt };
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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return ac->layout().has_value(); }));
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
        auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
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
        contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
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
TEST_CASE("a divider moved on the daemon re-flows an already realized tab", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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

// B3-Qt: the client authors a tab on the daemon; the daemon honors it and
// re-pushes its layout, which the incremental reconciler realizes as a new local
// tab — closing the create loop over a real attach connection.
TEST_CASE("attach authors a tab on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
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
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabCount() == 2);

    ac->stop();
}

// B4: a second daemon window (authored via requestRemoteWindow) is reconciled into
// its OWN GUI window — the client maps one OS window per daemon window, each mirroring
// only that window's tabs, with no cross-window session bleed.
TEST_CASE("attach maps each daemon window onto its own GUI window", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("win one"); // the primary daemon window

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
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
    REQUIRE(ac->requestRemoteWindow());
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
    contour::applyRemoteLayout(app.manager(), win1.id, *ac, ids[0]);
    contour::applyRemoteLayout(app.manager(), win2.id, *ac, ids[1]);

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

    auto acOwned = std::make_unique<contour::NativeController>(daemon.endpoint(), std::nullopt);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win1 { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->windowIds().empty(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->requestRemoteWindow());
    for (auto i = 0; i < 200 && ac->windowIds().size() < 2; ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->windowIds().size() == 2);
    auto const ids = ac->windowIds();

    contour::test::ScopedController const win2 { app.manager() };
    contour::applyRemoteLayout(app.manager(), win1.id, *ac, ids[0]);
    contour::applyRemoteLayout(app.manager(), win2.id, *ac, ids[1]);
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

    auto controller = contour::NativeController { daemon.endpoint(), std::nullopt };
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

TEST_CASE("attach controller reports an unreachable daemon", "[attach][controller]")
{
    // Port 1 on loopback has nothing listening: connect is refused before any TLS.
    auto controller = contour::NativeController {
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} }, std::nullopt
    };
    auto const connected = controller.connectAndWait(2s);
    REQUIRE(!connected.has_value());
    CHECK(!connected.error().empty());
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
    contour::WindowController* seen = nullptr;
    auto verdict = false;
    app.manager().setAttachWindowBinder([&](contour::WindowController* c) {
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
        [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty) override
        {
            ++tabs;
            tabPty = actingPty;
            return true;
        }
        [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const*, bool) override
        {
            ++splits;
            return true;
        }
        void reportSplitRatio(vtpty::Pty const*, vtpty::Pty const*, double ratio) override
        {
            ++ratios;
            lastRatio = ratio;
        }
        [[nodiscard]] bool requestRemoteWindow() override
        {
            ++windows;
            return true;
        }
        void requestRemoteClose(vtpty::Pty const*) override { ++closes; }
        void bindTerminal(vtpty::Pty const* pty, vtbackend::Terminal& terminal) override
        {
            ++binds;
            boundPty = pty;
            boundTerminal = &terminal;
        }
    };

    auto delegate = RecordingFactory {};
    auto router = contour::RoutingSessionFactory { std::make_unique<RecordingFactory>() };
    router.setDelegate(&delegate);

    auto events = vtbackend::Terminal::NullEvents {};
    auto settings = vtbackend::Settings {};
    auto pty = std::make_unique<vtpty::MockPty>(settings.pageSize);
    auto* device = pty.get();
    auto terminal = vtbackend::Terminal { events,
                                          crispy::defaultEnvironment(),
                                          std::move(pty),
                                          std::move(settings),
                                          std::chrono::steady_clock::now() };

    CHECK(router.createPty(std::nullopt) == nullptr);
    CHECK_FALSE(router.canCreateSession());
    CHECK(router.requestRemoteTab(device));
    CHECK(router.requestRemoteSplit(device, true));
    CHECK(router.requestRemoteWindow());
    router.requestRemoteClose(device);
    router.reportSplitRatio(device, device, 0.7);
    router.bindTerminal(device, terminal);

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
