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
        auto context = *tls;
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

TEST_CASE("a refusing session factory blocks every creation entry point", "[attach][factory]")
{
    auto app = contour::test::TestApp { std::make_unique<RefusingFactory>() };
    auto controller = contour::test::ScopedController { app.manager() };

    CHECK(app.manager().createSessionInBackground(controller.id) == nullptr);
    CHECK(app.manager().model().window(controller.id)->tabCount() == 0);
}
