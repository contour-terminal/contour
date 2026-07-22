// SPDX-License-Identifier: Apache-2.0
#include <contour/mux/AttachController.h>
#include <contour/mux/RoutingSessionFactory.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtpty/MockPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <ranges>
#include <string>
#include <thread>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <coro/Cancellation.hpp>
#include <muxserver/MuxServer.h>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/SocketPath.h>
#include <muxserver/TappingPty.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using namespace std::chrono_literals;

namespace
{

#ifndef _WIN32

/// An in-process `contour daemon` (native protocol only) on its own thread,
/// serving a real AF_UNIX socket — what `attach --gui` talks to in production.
struct DaemonFixture
{
    // The socket's PARENT directory must be private (0700): listenUnix
    // hardens it exactly like tmux and refuses a world-writable /tmp.
    std::string socketDir = std::format("/tmp/contour-at-{}", ::getpid());
    // The CONTROL-socket path; the native endpoint the client dials is derived
    // beside it (see nativeSocketPath), exactly as in production.
    std::string socketPath = socketDir + "/ctl.sock";
    net::PollEventSource source;
    net::EventLoop loop { source };
    std::unique_ptr<muxserver::SessionHost> host;
    std::unique_ptr<muxserver::MuxServer> server;
    std::thread thread;
    bool cancelled = false; ///< Whether teardown unwound the accept loop.

    DaemonFixture()
    {
        std::filesystem::remove_all(socketDir);
        host = std::make_unique<muxserver::SessionHost>(
            loop,
            [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
            vtbackend::Settings {},
            /*startPumps=*/false);
        auto listener = net::listenUnix(loop, muxserver::nativeSocketPath(socketPath).string());
        REQUIRE(listener.has_value());
        server = std::make_unique<muxserver::MuxServer>(
            loop, std::move(*listener), muxserver::makeNativeHandler(loop, *host));
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
        std::filesystem::remove_all(socketDir);
    }

    DaemonFixture(DaemonFixture const&) = delete;
    DaemonFixture& operator=(DaemonFixture const&) = delete;
    DaemonFixture(DaemonFixture&&) = delete;
    DaemonFixture& operator=(DaemonFixture&&) = delete;

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

#endif // !_WIN32

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

#ifndef _WIN32

// The daemon fixture builds on POSIX /tmp + 0700-hardening semantics; the
// afunix Windows path is covered by the runtime-gated net unix-echo test.
TEST_CASE("attach controller mirrors a remote session over a real socket", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("hello attach");

    auto controller =
        contour::AttachController { muxserver::UnixEndpoint { .socketPath = daemon.socketPath } };
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

// Regression: closing a mirrored tab must not resurrect it. Before the fix,
// unbind() only forgot the binding, so the still-live remote session's next
// delta re-registered it as pending and re-adopted a fresh tab indefinitely.
TEST_CASE("a closed mirrored tab does not resurrect on later remote output", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("first line");

    auto controller =
        contour::AttachController { muxserver::UnixEndpoint { .socketPath = daemon.socketPath } };
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
    auto controller = contour::AttachController { muxserver::UnixEndpoint {
        .socketPath = "/tmp/contour-attach-test-nonexistent.sock" } };
    auto const connected = controller.connectAndWait(2s);
    REQUIRE(!connected.has_value());
    CHECK(!connected.error().empty());
}

#endif // !_WIN32

TEST_CASE("a refusing session factory blocks every creation entry point", "[attach][factory]")
{
    auto app = contour::test::TestApp { std::make_unique<RefusingFactory>() };
    auto controller = contour::test::ScopedController { app.manager() };

    CHECK(app.manager().createSessionInBackground(controller.id) == nullptr);
    CHECK(app.manager().model().window(controller.id)->tabCount() == 0);
}
