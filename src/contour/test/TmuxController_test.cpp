// SPDX-License-Identifier: Apache-2.0
#include <contour/mux/TmuxController.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtpty/ChannelPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

#include <muxserver/tmux/LayoutString.h>
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

// F6: the pause-resume command the mirror sends on %pause must be exactly what the server's
// refresh-client parser consumes (`%N:continue`), so pin the wire string here. The server side of
// the same contract is covered in ControlSession_test ("refresh-client -A pauses and resumes ...").
TEST_CASE("tmux resume-pane command matches the server's refresh-client format (B5)", "[attach][tmux]")
{
    CHECK(contour::tmuxResumePaneCommand(1) == "refresh-client -A %1:continue");
    CHECK(contour::tmuxResumePaneCommand(42) == "refresh-client -A %42:continue");
}

// B5: the split/kill commands must match the server's split-window/kill-pane handlers
// (ControlSession::commandSplitWindow maps -h -> Vertical; default -> Horizontal).
TEST_CASE("tmux split/kill commands match the server's command format (B5)", "[attach][tmux]")
{
    CHECK(contour::tmuxSplitWindowCommand(3, /*vertical=*/true) == "split-window -h -t %3");
    CHECK(contour::tmuxSplitWindowCommand(3, /*vertical=*/false) == "split-window -t %3");
    CHECK(contour::tmuxKillPaneCommand(7) == "kill-pane -t %7");
}

// B5: a GUI split of a mirrored pane is authored on the tmux server (requestRemoteSplit returns
// true so the manager does NOT split locally); a pty not bound to a tmux pane is not routed.
TEST_CASE("a GUI split in tmux mode is authored on the tmux server (B5)", "[attach][tmux]")
{
    auto ctrlOwned = std::make_unique<contour::TmuxController>(std::string {});
    auto* ctrl = ctrlOwned.get();
    contour::test::TestApp app { std::move(ctrlOwned) };
    contour::test::ScopedController const win { app.manager() };

    ctrl->paneAdded(/*window=*/1, /*pane=*/10, 80, 24);
    ctrl->adoptPendingPanes(app.manager(), win.id);
    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    auto* session = app.manager().sessionForId(tab->activePane()->session());
    REQUIRE(session != nullptr);

    // The pane's pty is bound to tmux pane %10 → a GUI split routes to the server.
    CHECK(ctrl->requestRemoteSplit(&session->terminal().device(), /*vertical=*/true));

    // A pty bound to no tmux pane is not routed (the manager would split it locally).
    auto foreign =
        vtpty::MockPty { vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) } };
    CHECK_FALSE(ctrl->requestRemoteSplit(&foreign, /*vertical=*/true));

    ctrl->stop();
}

// F3: the pure converter turns a tmux BinaryLayout into a realizable vtmux::Layout that preserves the
// split orientation, the first-child ratio, and each leaf's tmux pane id.
TEST_CASE("tmux binary layout converts to a ratio-bearing vtmux layout (F3)", "[attach][tmux]")
{
    auto const wireOf = [](std::string const& body) {
        return std::format("{:04x},{}", muxserver::tmux::layoutChecksum(body), body);
    };

    // A 2-pane side-by-side window split 70 | 29 (+1 divider) — an asymmetric split.
    auto const parsed2 = muxserver::tmux::parseLayout(wireOf("100x50,0,0{70x50,0,0,1,29x50,71,0,2}"));
    REQUIRE(parsed2.has_value());
    auto const tree2 = muxserver::tmux::collapseToBinary(*parsed2);
    REQUIRE(tree2.leafCount() == 2);

    auto const conv2 = contour::tmuxLayoutToWindowLayout(tree2);
    REQUIRE(conv2.layout.tabs.size() == 1);
    auto const& root2 = conv2.layout.tabs.front().root;
    REQUIRE_FALSE(root2.isLeaf());
    CHECK(root2.orientation == vtmux::SplitState::Vertical); // side-by-side
    REQUIRE(root2.children.size() == 2);
    REQUIRE(root2.children[0].ratio.has_value());
    CHECK(*root2.children[0].ratio == tree2.ratio); // the exact tmux ratio, copied faithfully
    CHECK(*root2.children[0].ratio > 0.6);          // and it is the real asymmetric one (~0.71), not 0.5
    CHECK(conv2.leafPane.at(&root2.children[0]) == 1);
    CHECK(conv2.leafPane.at(&root2.children[1]) == 2);

    // A 3-pane right-leaning chain: head leaf + a nested tail split.
    auto const parsed3 =
        muxserver::tmux::parseLayout(wireOf("160x50,0,0{53x50,0,0,1,52x50,54,0,2,53x50,107,0,3}"));
    REQUIRE(parsed3.has_value());
    auto const tree3 = muxserver::tmux::collapseToBinary(*parsed3);
    auto const conv3 = contour::tmuxLayoutToWindowLayout(tree3);
    auto const& root3 = conv3.layout.tabs.front().root;
    REQUIRE(root3.children.size() == 2);
    CHECK(conv3.leafPane.at(&root3.children[0]) == 1); // head leaf
    REQUIRE_FALSE(root3.children[1].isLeaf());         // right-leaning tail is itself a split
    REQUIRE(root3.children[1].children.size() == 2);
    CHECK(conv3.leafPane.at(&root3.children[1].children[0]) == 2);
    CHECK(conv3.leafPane.at(&root3.children[1].children[1]) == 3);
}

// F3: realizing a whole tmux window tree reproduces the split's orientation AND its non-even ratio in
// the GUI's own pane model — driving exactly the converter + setNextBindPane + applyLayoutToWindow path
// that realizeWindowLayout uses.
TEST_CASE("tmux whole-tree realize reproduces the split ratio and shape (F3)", "[attach][tmux]")
{
    auto ctrlOwned = std::make_unique<contour::TmuxController>(std::string {});
    auto* ctrl = ctrlOwned.get();
    contour::test::TestApp app { std::move(ctrlOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto const body = std::string { "100x50,0,0{70x50,0,0,1,29x50,71,0,2}" };
    auto const parsed =
        muxserver::tmux::parseLayout(std::format("{:04x},{}", muxserver::tmux::layoutChecksum(body), body));
    REQUIRE(parsed.has_value());
    auto const tree = muxserver::tmux::collapseToBinary(*parsed);
    auto const converted = contour::tmuxLayoutToWindowLayout(tree);

    // Both leaf panes discovered, so createPty can size them.
    ctrl->paneAdded(/*window=*/1, /*pane=*/1, 70, 50);
    ctrl->paneAdded(/*window=*/1, /*pane=*/2, 29, 50);

    // Realize the whole tree, binding each leaf to its tmux pane (via the public setNextBindPane seam).
    app.manager().applyLayoutToWindow(win.id, converted.layout, std::nullopt, [&](auto const& leaf) {
        if (auto const it = converted.leafPane.find(&leaf); it != converted.leafPane.end())
            ctrl->setNextBindPane(it->second);
    });

    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    CHECK(tab->paneCount() == 2); // both panes realized as one split tab
    REQUIRE_FALSE(tab->rootPane()->isLeaf());
    CHECK(tab->rootPane()->splitState() == vtmux::SplitState::Vertical); // faithful orientation
    CHECK(tab->rootPane()->ratio() > 0.6);                               // faithful ~0.71 ratio, not 0.5

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
