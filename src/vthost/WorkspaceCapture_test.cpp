// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace vthost;
using vtworkspace::SplitState;

namespace
{

/// A host over MockPty sessions with no pump threads: the test stands in for the loop thread.
struct CaptureHarness
{
    CaptureHarness(): host { loop, makePty, vtbackend::Settings {}, core::defaultEnvironment(), config() } {}

    [[nodiscard]] static std::unique_ptr<vtpty::Pty> makePty(
        vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const& /*shell*/)
    {
        return std::make_unique<vtpty::MockPty>(size);
    }

    [[nodiscard]] static SessionHostConfig config()
    {
        return SessionHostConfig { .pumping = SessionPumping::Manual,
                                   .startupLayout = vtworkspace::Layout {} };
    }

    /// @return The terminal of @p tab's active pane.
    [[nodiscard]] vtbackend::Terminal& activeTerminal(vtworkspace::Tab const& tab)
    {
        auto* const terminal = host.terminal(tab.activePane()->session());
        REQUIRE(terminal != nullptr);
        return *terminal;
    }

    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    SessionHost host;
};

constexpr auto Now = std::chrono::sys_seconds { std::chrono::seconds { 1'761'000'000 } };

[[nodiscard]] std::string textOf(proto::WireLine const& line)
{
    auto text = std::string {};
    for (auto const& cell: line.cells)
        text += cell.codepoint == 0 ? ' ' : static_cast<char>(cell.codepoint);
    return text;
}

} // namespace

TEST_CASE("captureWorkspace records the layout as the model holds it", "[vthost][snapshot][capture]")
{
    auto h = CaptureHarness {};
    auto* first = h.host.createTab();
    auto* second = h.host.createTab();
    REQUIRE((first != nullptr && second != nullptr));
    h.host.splitActivePane(second->id(), SplitState::Vertical, 0.3);
    h.host.splitActivePane(second->id(), SplitState::Horizontal, 0.5);
    REQUIRE(second->paneCount() == 3);
    h.host.model().toggleActivePaneZoom(second->id());
    h.host.model().setTabTitle(second->id(), "build");
    h.host.model().setTabColor(
        second->id(), vtworkspace::TabColorSource::User, vtbackend::RGBColor { 0x336699U });
    auto const* const other = h.host.createWindow();
    REQUIRE(other != nullptr);
    h.host.model().activateTab(h.host.windowId(), second->id());

    auto const snapshot = captureWorkspace(h.host, {}, Now);

    CHECK(snapshot.takenAt == Now);
    REQUIRE(snapshot.windows.size() == 2);
    CHECK(snapshot.windows[0].activeTab == 1);
    REQUIRE(snapshot.windows[0].tabs.size() == 2);
    CHECK(snapshot.windows[0].tabs[0].root.split == SplitState::None);
    auto const& tab = snapshot.windows[0].tabs[1];
    CHECK(tab.root.split == SplitState::Vertical);
    CHECK(tab.root.ratio == Catch::Approx(0.3));
    REQUIRE(tab.root.children.size() == 2);
    CHECK(tab.root.children[0].split == SplitState::None);
    CHECK(tab.root.children[1].split == SplitState::Horizontal);
    CHECK(tab.root.children[1].children.size() == 2);
    CHECK(tab.activeLeaf == 2);
    CHECK(tab.zoom == TabZoom::Zoomed);
    CHECK(tab.title == "build");
    CHECK(tab.color == vtbackend::RGBColor { 0x33, 0x66, 0x99 });
    CHECK(snapshot.windows[1].tabs.size() == 1);
}

TEST_CASE("captureWorkspace of an empty host is empty", "[vthost][snapshot][capture]")
{
    auto h = CaptureHarness {};
    CHECK(captureWorkspace(h.host, {}, Now).empty());
}

TEST_CASE("capturePaneHistory keeps the newest rows above the cursor", "[vthost][snapshot][capture]")
{
    auto h = CaptureHarness {};
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto& terminal = h.activeTerminal(*tab);
    for (auto const number: std::views::iota(1, 51))
        terminal.writeToScreen(std::format("line {}\r\n", number));
    terminal.writeToScreen("$ ");

    auto const history = capturePaneHistory(terminal, 10);
    REQUIRE(history.lines.size() == 10);
    CHECK(textOf(history.lines.front()).starts_with("line 41"));
    CHECK(textOf(history.lines.back()).starts_with("line 50"));
    CHECK(history.snapshot == 1);
}

TEST_CASE("capturePaneHistory drops the blank rows under the last printed one", "[vthost][snapshot][capture]")
{
    auto h = CaptureHarness {};
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto& terminal = h.activeTerminal(*tab);
    CHECK(capturePaneHistory(terminal, 100).lines.empty());
    terminal.writeToScreen("one\r\n\r\n\r\n");
    auto const history = capturePaneHistory(terminal, 100);
    REQUIRE(history.lines.size() == 1);
    CHECK(textOf(history.lines.front()).starts_with("one"));
}

TEST_CASE("capturePaneHistory closes a running block and drops a prompting one",
          "[vthost][snapshot][capture]")
{
    namespace test = vtbackend::test;
    auto h = CaptureHarness {};
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto& terminal = h.activeTerminal(*tab);

    SECTION("a finished and a running block")
    {
        terminal.writeToScreen(test::commandRun("make", { "out" }, 2));
        terminal.writeToScreen(test::commandStarted("sleep 9"));
        terminal.writeToScreen("tick\r\n");
        auto const history = capturePaneHistory(terminal, 100);
        REQUIRE(history.commandBlocks.size() == 2);
        CHECK(history.commandBlocks[0].commandLine == "make");
        CHECK(history.commandBlocks[0].exitCode == 2);
        CHECK(history.commandBlocks[0].state == std::to_underlying(vtbackend::CommandBlockState::Finished));
        CHECK(history.commandBlocks[1].commandLine == "sleep 9");
        CHECK(history.commandBlocks[1].state == std::to_underlying(vtbackend::CommandBlockState::Finished));
        CHECK(history.commandBlocks[1].end == std::to_underlying(vtbackend::CommandBlockEnd::Abandoned));
        CHECK((history.commandBlocks[1].present & proto::CommandBlockHasExitCode) == 0);
        // The store itself is untouched: the program still runs.
        CHECK(terminal.commandBlocks().current()->state == vtbackend::CommandBlockState::Running);
    }
    SECTION("a prompt nothing ran at")
    {
        terminal.writeToScreen(test::commandRun("make", { "out" }, 0));
        terminal.writeToScreen(std::string { test::PromptStart } + "\r\n");
        auto const history = capturePaneHistory(terminal, 100);
        CHECK(history.commandBlocks.size() == 1);
        // No row names a record that was left out.
        auto const kept = history.commandBlocks.front().id;
        CHECK(std::ranges::all_of(
            history.lines, [&](auto const& line) { return line.blockId == 0 || line.blockId == kept; }));
    }
}

TEST_CASE("capturePaneHistory carries the hyperlinks its rows use", "[vthost][snapshot][capture]")
{
    auto h = CaptureHarness {};
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto& terminal = h.activeTerminal(*tab);
    terminal.writeToScreen("plain\r\n");
    terminal.writeToScreen("\033]8;;https://contour-terminal.org\033\\link\033]8;;\033\\\r\n");
    terminal.writeToScreen("tail\r\n");

    auto const history = capturePaneHistory(terminal, 100);
    REQUIRE(history.hyperlinks.size() == 1);
    CHECK(history.hyperlinks.front().uri == "https://contour-terminal.org");
    CHECK(std::ranges::any_of(history.lines, [&](auto const& line) {
        return std::ranges::any_of(
            line.cells, [&](auto const& cell) { return cell.hyperlink == history.hyperlinks.front().id; });
    }));
}

TEST_CASE("captureWorkspace with scrollback off stores no history but keeps the directory",
          "[vthost][snapshot][capture]")
{
    auto h = CaptureHarness {};
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto& terminal = h.activeTerminal(*tab);
    terminal.writeToScreen("some output\r\n");
    terminal.writeToScreen("\033]7;file://localhost/tmp/work\033\\");

    auto const withTail = captureWorkspace(h.host, {}, Now);
    auto const without = captureWorkspace(h.host, { .scrollback = ScrollbackPersistence::Off }, Now);

    auto const& tailed = withTail.windows.at(0).tabs.at(0).root.pane;
    auto const& bare = without.windows.at(0).tabs.at(0).root.pane;
    CHECK(tailed.history.has_value());
    CHECK_FALSE(bare.history.has_value());
    CHECK(bare.workingDirectory == tailed.workingDirectory);
}

TEST_CASE("captureWorkspace records what the pane runs", "[vthost][snapshot][capture]")
{
    namespace test = vtbackend::test;
    auto h = CaptureHarness {};
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto& terminal = h.activeTerminal(*tab);

    CHECK_FALSE(captureWorkspace(h.host, {}, Now).windows.at(0).tabs.at(0).root.pane.runningCommandLine);

    terminal.writeToScreen(test::commandStarted("vim notes.md"));
    CHECK(captureWorkspace(h.host, {}, Now).windows.at(0).tabs.at(0).root.pane.runningCommandLine
          == "vim notes.md");

    terminal.writeToScreen(test::commandEnd(0));
    CHECK_FALSE(captureWorkspace(h.host, {}, Now).windows.at(0).tabs.at(0).root.pane.runningCommandLine);
}
