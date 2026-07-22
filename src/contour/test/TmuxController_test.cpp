// SPDX-License-Identifier: Apache-2.0
#include <contour/mux/TmuxController.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtpty/ChannelPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

// The tmux mirror's structural reactions are driven through the TmuxModelEvents overrides, which
// are pure C++ (no tmux process) — so this maps to a real GUI SessionModel headlessly, on every
// platform. The oracle tests below (real `tmux` binary) are POSIX-only.
TEST_CASE("tmux %window-renamed retitles the mirrored tab (B5)", "[attach][tmux]")
{
    auto ctrlOwned = std::make_unique<contour::TmuxController>(std::string {}); // not connected
    auto* ctrl = ctrlOwned.get();
    contour::test::TestApp app { std::move(ctrlOwned) };
    contour::test::ScopedController const win { app.manager() };

    // A pane in tmux window 1 appears and is realized as the first tab.
    ctrl->paneAdded(/*window=*/1, /*pane=*/10, 80, 24);
    ctrl->adoptPendingPanes(app.manager(), win.id);
    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    REQUIRE_FALSE(tab->runtimeTitle().has_value());

    // tmux renames the window AFTER it was realized → reflected onto the tab on the next drain.
    ctrl->windowRenamed(1, "editor");
    ctrl->applyPendingRenames(app.manager());
    REQUIRE(tab->runtimeTitle().has_value());
    CHECK(*tab->runtimeTitle() == "editor");

    // A rename that arrives BEFORE its window is realized is held, then applied when the pane is
    // adopted (adoptPendingPanes drains pending renames).
    ctrl->windowRenamed(2, "logs");
    ctrl->paneAdded(/*window=*/2, /*pane=*/20, 80, 24);
    ctrl->adoptPendingPanes(app.manager(), win.id);
    auto* second = app.manager().model().window(win.id)->tabAt(1);
    REQUIRE(second != nullptr);
    REQUIRE(second->runtimeTitle().has_value());
    CHECK(*second->runtimeTitle() == "logs");

    ctrl->stop();
}

#ifndef _WIN32

    #include <array>
    #include <chrono>
    #include <cstdio>
    #include <filesystem>
    #include <format>
    #include <string>
    #include <thread>
    #include <tuple>

    #include <unistd.h>

using namespace std::chrono_literals;

namespace
{

/// Runs a shell command via popen, returning its captured stdout (or "" on
/// spawn failure); ::system is concurrency-mt-unsafe.
std::string runShellCapture(std::string const& command)
{
    auto* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
        return {};
    auto output = std::string {};
    auto buffer = std::array<char, 256> {};
    while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        output += buffer.data();
    std::ignore = ::pclose(pipe);
    return output;
}

/// A private, detached tmux server the controller attaches to.
struct RealTmuxServer
{
    std::string socketPath;

    static std::unique_ptr<RealTmuxServer> launch()
    {
        if (!runShellCapture("command -v tmux && echo have-tmux").contains("have-tmux"))
            return nullptr;
        auto self = std::make_unique<RealTmuxServer>();
        self->socketPath =
            (std::filesystem::temp_directory_path() / std::format("ctmuxgui-{}.sock", ::getpid())).string();
        std::ignore = runShellCapture(std::format(
            "tmux -S {} -f /dev/null new-session -d -s mirror -x 80 -y 24 2>&1", self->socketPath));
        if (runShellCapture(std::format("tmux -S {} list-sessions 2>/dev/null", self->socketPath)).empty())
            return nullptr;
        return self;
    }

    ~RealTmuxServer()
    {
        if (socketPath.empty())
            return;
        std::ignore = runShellCapture(std::format("tmux -S {} kill-server 2>&1", socketPath));
        std::error_code ec;
        std::filesystem::remove(socketPath, ec);
    }

    RealTmuxServer() = default;
    RealTmuxServer(RealTmuxServer const&) = delete;
    RealTmuxServer& operator=(RealTmuxServer const&) = delete;
    RealTmuxServer(RealTmuxServer&&) = delete;
    RealTmuxServer& operator=(RealTmuxServer&&) = delete;
};

/// Drains @p pty until @p needle shows up (bounded), returning what was read.
std::string drainUntil(vtpty::Pty& pty, std::string_view needle)
{
    auto pool = crispy::buffer_object_pool<char> { 65536 };
    auto collected = std::string {};
    for (auto i = 0; i < 300 && !collected.contains(needle); ++i)
    {
        auto const storage = pool.allocateBufferObject();
        if (auto const result = pty.read(*storage, 50ms, 65536); result && !result->data.empty())
            collected.append(result->data);
    }
    return collected;
}

} // namespace

TEST_CASE("tmux controller mirrors a real tmux session", "[attach][tmux][oracle]")
{
    auto server = RealTmuxServer::launch();
    if (!server)
    {
        SKIP("tmux not available");
    }
    // Seed identifiable content before attaching (it arrives via replay).
    std::ignore = runShellCapture(
        std::format("tmux -S {} send-keys -t mirror 'echo tmux-mirror-seed' Enter 2>&1", server->socketPath));

    auto controller = contour::TmuxController { server->socketPath };
    auto const connected = controller.connectAndWait(15s);
    REQUIRE(connected.has_value());
    REQUIRE(controller.canCreateSession());

    // The factory hands out a pty carrying the pane's capture-pane replay.
    auto pty = controller.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    CHECK(drainUntil(*pty, "tmux-mirror-seed").contains("tmux-mirror-seed"));

    // Input written by the (would-be) terminal reaches the remote pane
    // through send-keys -H, confirmed by the shell echoing it back.
    std::ignore = pty->write("echo tmux-hex-ok\r");
    auto echoed = false;
    for (auto i = 0; i < 100 && !echoed; ++i)
    {
        echoed = runShellCapture(std::format("tmux -S {} capture-pane -p -t mirror 2>&1", server->socketPath))
                     .contains("tmux-hex-ok");
        if (!echoed)
            std::this_thread::sleep_for(100ms);
    }
    CHECK(echoed);

    controller.stop();
    CHECK(pty->isClosed());
    pty.reset();
}

#endif // !_WIN32
