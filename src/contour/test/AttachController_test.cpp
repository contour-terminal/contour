// SPDX-License-Identifier: Apache-2.0
#include <contour/mux/AttachController.h>
#include <contour/mux/RoutingSessionFactory.h>
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

#include <contour/mux/RemoteLayout.h>

#include <cstdint>

#include <coro/Cancellation.hpp>
#include <muxserver/MuxServer.h>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/SocketPath.h>
#include <muxserver/TappingPty.h>
#include <muxserver/client/LayoutReconstruction.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <net/Tls.h>
#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

using namespace std::chrono_literals;

namespace
{

/// An in-process `contour daemon` (native protocol) on its own thread, serving
/// TLS over a LOOPBACK TCP socket on an EPHEMERAL port. Using TCP (the same
/// transport `attach --gui --connect-tcp` uses) rather than AF_UNIX lets these
/// end-to-end tests run on every platform, Windows included.
struct DaemonFixture
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    std::unique_ptr<muxserver::SessionHost> host;
    std::unique_ptr<muxserver::MuxServer> server;
    std::uint16_t port = 0; ///< The OS-assigned loopback port the daemon listens on.
    std::thread thread;
    bool cancelled = false; ///< Whether teardown unwound the accept loop.

    DaemonFixture()
    {
        host = std::make_unique<muxserver::SessionHost>(
            loop,
            [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
            vtbackend::Settings {},
            /*startPumps=*/false);
        auto listener = net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        port = (*listener)->localPort();
        // Encrypt each accepted socket (server-side TLS, self-signed) before the
        // native protocol runs over it — the daemon's real TCP path. This exercises
        // the two-reactor TLS handshake (client + daemon on independent reactors),
        // which AttachClient's concurrent read+write drives.
        auto tls = net::makeSelfSignedServerContext();
        REQUIRE(tls.has_value());
        auto native = muxserver::makeNativeHandler(loop, *host);
        auto const& context = *tls;
        auto handler = [context, native](std::unique_ptr<net::ISocket> socket) {
            return native(context->wrap(std::move(socket)));
        };
        server = std::make_unique<muxserver::MuxServer>(loop, std::move(*listener), handler);
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
    [[nodiscard]] muxserver::TcpEndpoint endpoint() const
    {
        return muxserver::TcpEndpoint { .host = "127.0.0.1", .port = port, .token = {}, .caPem = {} };
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
    [[nodiscard]] vtmux::SessionId seedSession(std::string text)
    {
        return onDaemon([this, text = std::move(text)] {
            host->createTab();
            auto const id = host->model().window(host->windowId())->activeTab()->rootPane()->session();
            host->terminal(id)->writeToScreen(text);
            return id;
        });
    }
};

/// Drains @p pty until @p needle shows up (bounded), returning what was read.
std::string drainUntil(vtpty::Pty& pty, std::string_view needle)
{
    auto pool = crispy::buffer_object_pool<char> { 65536 };
    auto collected = std::string {};
    for (auto i = 0; i < 200 && !collected.contains(needle); ++i)
    {
        auto const storage = pool.allocateBufferObject();
        if (auto const result = pty.read(*storage, 50ms, 65536); result && !result->data.empty())
            collected.append(result->data);
    }
    return collected;
}

/// A factory that can never back a session (the attach guard's stand-in).
struct RefusingFactory final: contour::SessionFactory
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

    auto controller = contour::AttachController { daemon.endpoint() };
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());
    REQUIRE(controller.pendingCount() == 1);
    REQUIRE(controller.canCreateSession());

    // The factory hands out a pty bound to the remote session...
    auto pty = controller.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    CHECK(!controller.canCreateSession()); // the one pending session is consumed

    // ...whose feed carries the mirror's full replay of the remote screen.
    CHECK(drainUntil(*pty, "hello attach").contains("hello attach"));

    // Input written by the (would-be) terminal reaches the remote PTY.
    std::ignore = pty->write("ls\r");
    auto const stdinArrived = [&] {
        for (auto i = 0; i < 200; ++i)
        {
            auto const bytes = daemon.onDaemon([&] {
                auto& tapped = dynamic_cast<muxserver::TappingPty&>(daemon.host->terminal(session)->device());
                return dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer();
            });
            if (bytes == "ls\r")
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }();
    CHECK(stdinArrived);

    // A local resize proposes the client area upstream.
    pty->resizeScreen(vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(100) });
    auto const resized = [&] {
        for (auto i = 0; i < 200; ++i)
        {
            if (daemon.onDaemon([&] { return daemon.host->pageSize(); })
                == vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(100) })
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }();
    CHECK(resized);

    // Detaching closes the bound pty: its session would see a shell exit.
    controller.stop();
    CHECK(pty->isClosed());
    pty.reset(); // unbind (the controller outlives its ptys' registrations)
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
        daemon.host->splitActivePane(tab->id(), vtmux::SplitState::Vertical, 0.5);
        return 0;
    });

    auto controller = contour::AttachController { daemon.endpoint() };
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());

    // The layout is pushed leading the snapshot; poll until the controller has it.
    auto layout = std::optional<muxserver::proto::LayoutState> {};
    for (auto i = 0; i < 200 && !(layout = controller.layout()).has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(layout.has_value());
    REQUIRE(layout->tabs.size() == 1);

    // The tab's root is a vertical split with two distinct-session leaves.
    auto const& root = layout->tabs.front().root;
    CHECK(root.split == std::to_underlying(vtmux::SplitState::Vertical));
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
        daemon.host->splitActivePane(tab->id(), vtmux::SplitState::Vertical, 0.6);
        return 0;
    });

    // The AttachController is the manager's session factory (attach mode).
    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
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
    CHECK(tab->rootPane()->splitState() == vtmux::SplitState::Vertical);

    ac->stop();
}

// B3-Qt: a split authored on the daemon AFTER attach reconciles into the already-
// shown tab as a second pane (intra-tab incremental reconciliation), rather than
// only whole new tabs.
TEST_CASE("attach reconciles a split authored on the daemon after attach", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    // Split the tab on the daemon (after the client attached) and wait for the
    // split to reach the client's layout.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtmux::SplitState::Horizontal, 0.5);
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

    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
    for (auto i = 0; i < 200 && !ac->layout().has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    contour::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    // A distinctly uneven split (0.7 to the first/acting child) authored on the daemon.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtmux::SplitState::Vertical, 0.7);
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
    auto const second = daemon.onDaemon([&daemon]() -> vtmux::SessionId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtmux::SplitState::Vertical, 0.5);
        return tab->activePane()->session(); // the new (active) pane's session
    });

    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    REQUIRE(ac->connectAndWait(10s).has_value());
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

    ac->stop();
}

// B3-Qt: a GUI split in attach mode is authored on the daemon (routed by
// requestRemoteSplit), which splits the right pane and re-pushes its layout; the
// reconciler realizes the new pane locally — the full split-authoring loop.
TEST_CASE("attach authors a split on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
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

// B3-Qt: the client authors a tab on the daemon; the daemon honors it and
// re-pushes its layout, which the incremental reconciler realizes as a new local
// tab — closing the create loop over a real attach connection.
TEST_CASE("attach authors a tab on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
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

    auto acOwned = std::make_unique<contour::AttachController>(daemon.endpoint());
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win1 { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
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
    auto const boundSession = [&](vtmux::WindowId window) -> std::optional<uint64_t> {
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

// Regression: closing a mirrored tab must not resurrect it. Before the fix,
// unbind() only forgot the binding, so the still-live remote session's next
// delta re-registered it as pending and re-adopted a fresh tab indefinitely.
TEST_CASE("a closed mirrored tab does not resurrect on later remote output", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("first line");

    auto controller = contour::AttachController { daemon.endpoint() };
    REQUIRE(controller.connectAndWait(10s).has_value());
    REQUIRE(controller.pendingCount() == 1);

    // Bind the pending remote session to a local tab, then close that tab.
    auto pty = controller.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    CHECK(drainUntil(*pty, "first line").contains("first line"));
    CHECK(controller.pendingCount() == 0);
    pty.reset(); // the terminal destroyed its pty: the user closed the tab

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
    auto controller = contour::AttachController { muxserver::TcpEndpoint {
        .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} } };
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
