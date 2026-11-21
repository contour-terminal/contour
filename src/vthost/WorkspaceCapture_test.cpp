// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/OwnedPanes.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TabEdits.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/testing/ServedTesting.hpp>
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
    std::ignore = h.host.splitActivePane(second->id(), SplitState::Vertical, 0.3);
    std::ignore = h.host.splitActivePane(second->id(), SplitState::Horizontal, 0.5);
    REQUIRE(second->paneCount() == 3);
    h.host.model().toggleActivePaneZoom(second->id());
    vthost::RegistryTabNames { h.host.objects() }.setName(second->id(), "build");
    h.host.model().setTabColor(
        second->id(), vtworkspace::TabColorSource::User, vtbackend::RGBColor { 0x336699U });
    auto const* const other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    h.host.model().activateTab(h.host.workspaceId(), second->id());

    auto const snapshot = captureWorkspace(h.host, {}, Now);

    CHECK(snapshot.takenAt == Now);
    REQUIRE(snapshot.workspaces.size() == 2);
    CHECK(snapshot.workspaces[0].activeTab == 1);
    REQUIRE(snapshot.workspaces[0].tabs.size() == 2);
    CHECK(snapshot.workspaces[0].tabs[0].root.split == SplitState::None);
    auto const& tab = snapshot.workspaces[0].tabs[1];
    CHECK(tab.root.split == SplitState::Vertical);
    CHECK(tab.root.ratio == Catch::Approx(0.3));
    REQUIRE(tab.root.children.size() == 2);
    CHECK(tab.root.children[0].split == SplitState::None);
    CHECK(tab.root.children[1].split == SplitState::Horizontal);
    CHECK(tab.root.children[1].children.size() == 2);
    CHECK(tab.activeLeaf == 2);
    CHECK(tab.zoom == TabZoom::Zoomed);
    REQUIRE(tab.identity.has_value());
    CHECK(tab.identity->name == "build");
    CHECK_FALSE(tab.title.has_value()); // no runtime title is written any more
    CHECK(tab.color == vtbackend::RGBColor { 0x33, 0x66, 0x99 });
    CHECK(snapshot.workspaces[1].tabs.size() == 1);
}

TEST_CASE("captureWorkspace records each workspace's colour, and none where it has none",
          "[vthost][snapshot][capture][workspace-color]")
{
    auto h = CaptureHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    auto const* const other = h.host.createWorkspace();
    REQUIRE(other != nullptr);
    h.host.model().setWorkspaceColor(other->id(), vtbackend::RGBColor { 0x7A3E9DU });

    auto const snapshot = captureWorkspace(h.host, {}, Now);

    REQUIRE(snapshot.workspaces.size() == 2);
    CHECK_FALSE(snapshot.workspaces[0].color.has_value());
    CHECK(snapshot.workspaces[1].color == vtbackend::RGBColor { 0x7A3E9DU });
}

namespace
{

/// @return The tab of a served tab @p host opened, as the fake extension opens it.
[[nodiscard]] vtworkspace::Tab& openServed(SessionHost& host)
{
    auto const session = vthost::testing::openServedTab(host);
    REQUIRE(session.has_value());
    auto const [workspace, tab, leaf] = host.model().findSessionLeaf(*session);
    REQUIRE(tab != nullptr);
    return *tab;
}

/// @return The UUID @p snapshot names @p tab by.
[[nodiscard]] std::optional<vthost::ObjectUuid> uuidOf(TabSnapshot const& tab)
{
    return tab.identity ? std::optional { tab.identity->uuid } : std::nullopt;
}

} // namespace

TEST_CASE("a snapshot leaves served tabs out", "[vthost][snapshot][capture][served]")
{
    auto h = CaptureHarness {};
    auto* const shell = h.host.createTab();
    REQUIRE(shell != nullptr);
    std::ignore = openServed(h.host);
    // A served tab stays whole (the host refuses to split one), so each served tab is one leaf.
    auto& refused = openServed(h.host);
    std::ignore = h.host.splitActivePane(refused.id(), SplitState::Vertical, 0.5);
    REQUIRE(refused.paneCount() == 1);

    auto const snapshot = captureWorkspace(h.host, {}, Now);
    REQUIRE(snapshot.workspaces.size() == 1);
    REQUIRE(snapshot.workspaces[0].tabs.size() == 1);
    CHECK(uuidOf(snapshot.workspaces[0].tabs[0]) == h.host.objects().uuidOf(shell->id())->bytes);
    CHECK(snapshot.workspaces[0].activeTab == 0);

    // A workspace whose only tab is a served one has nothing to restore.
    auto lone = CaptureHarness {};
    std::ignore = openServed(lone.host);
    CHECK(captureWorkspace(lone.host, {}, Now).workspaces.empty());
}

TEST_CASE("the active tab is remapped past a served tab", "[vthost][snapshot][capture][served]")
{
    auto h = CaptureHarness {};
    auto* const first = h.host.createTab();
    REQUIRE(first != nullptr);
    auto& served = openServed(h.host);
    auto* const last = h.host.createTab();
    REQUIRE(last != nullptr);
    auto const workspace = h.host.workspaceId();
    auto const capturedActive = [&] {
        auto const snapshot = captureWorkspace(h.host, {}, Now);
        REQUIRE(snapshot.workspaces.size() == 1);
        REQUIRE(snapshot.workspaces[0].tabs.size() == 2);
        return snapshot.workspaces[0].activeTab;
    };

    SECTION("an active tab after a served one shifts down")
    {
        h.host.model().activateTab(workspace, last->id());
        CHECK(capturedActive() == 1);
    }
    SECTION("an active served tab maps to the tab after it")
    {
        h.host.model().activateTab(workspace, served.id());
        CHECK(capturedActive() == 1);
    }
    SECTION("an active tab before a served one stays")
    {
        h.host.model().activateTab(workspace, first->id());
        CHECK(capturedActive() == 0);
    }
    SECTION("an active served tab that is the last maps to the tab before it")
    {
        h.host.closeSession(last->rootPane()->session());
        h.host.model().activateTab(workspace, served.id());
        auto const snapshot = captureWorkspace(h.host, {}, Now);
        REQUIRE(snapshot.workspaces.size() == 1);
        REQUIRE(snapshot.workspaces[0].tabs.size() == 1);
        CHECK(snapshot.workspaces[0].activeTab == 0);
    }
}

namespace
{

/// Opens an agent pane of the orchestrator's in @p h's host, in a tab of its own, and turns the loop until it
/// exists.
/// @return Its tab.
[[nodiscard]] vtworkspace::Tab& openOwned(CaptureHarness& h, std::string program = "claude")
{
    auto const turn = [&] {
        h.loop.blockOn(core::net::testing::sleepFor(&h.loop, std::chrono::milliseconds { 0 }));
    };
    turn(); // The registry's reconciliation, which the constructor posted.
    auto const directory = std::filesystem::temp_directory_path().string();
    auto const pane =
        h.host.ownedPanes().open(OwnedPaneSpawn { .extension = "orchestrator",
                                                  .argv = { std::move(program) },
                                                  .directory = std::make_shared<vtpty::DirectoryHandle const>(
                                                      vtpty::DirectoryHandle::unbacked(directory)),
                                                  .cwd = directory,
                                                  .title = "run-1",
                                                  .tag = "run-1" });
    REQUIRE(pane.has_value());
    turn();
    for (auto const session: h.host.sessionIds())
        if (auto const uuid = h.host.objects().uuidOf(session); uuid && vtcontrol::toString(*uuid) == *pane)
        {
            auto const [workspace, tab, leaf] = h.host.model().findSessionLeaf(session);
            REQUIRE(tab != nullptr);
            return *const_cast<vtworkspace::Tab*>(tab);
        }
    FAIL("the agent pane was not created");
    throw std::logic_error { "unreachable" };
}

/// @return Every pane UUID @p node holds, depth first.
void collectPaneUuids(PaneNode const& node, std::vector<ObjectUuid>& uuids)
{
    if (node.children.empty())
    {
        if (node.pane.identity)
            uuids.push_back(node.pane.identity->uuid);
        return;
    }
    for (auto const& child: node.children)
        collectPaneUuids(child, uuids);
}

} // namespace

TEST_CASE("a snapshot leaves owned panes out", "[vthost][snapshot][capture][panes]")
{
    auto h = CaptureHarness {};
    auto* const shell = h.host.createTab();
    REQUIRE(shell != nullptr);
    auto const workspace = h.host.workspaceId();

    SECTION("a tab of the agent alone is left out")
    {
        std::ignore = openOwned(h);
        auto const snapshot = captureWorkspace(h.host, {}, Now);
        REQUIRE(snapshot.workspaces.size() == 1);
        REQUIRE(snapshot.workspaces[0].tabs.size() == 1);
        CHECK(uuidOf(snapshot.workspaces[0].tabs[0]) == h.host.objects().uuidOf(shell->id())->bytes);
    }
    SECTION("an agent split beside a shell is pruned: the shell is the tab's one leaf, and active")
    {
        auto& agent = openOwned(h);
        auto const agentSession = agent.rootPane()->session();
        auto const shellSession = shell->rootPane()->session();
        REQUIRE(h.host
                    .mergeTabIntoSplit(proto::MergeTabIntoSplit {
                        .tabSessions = { agentSession.value },
                        .targetSession = shellSession.value,
                        .side = std::to_underlying(vtworkspace::SplitSide::Right) })
                    .has_value());
        REQUIRE(shell->paneCount() == 2);
        // The agent's pane is the active one of the merged tab.
        auto const [found, tab, leaf] = h.host.model().findSessionLeaf(agentSession);
        REQUIRE(leaf != nullptr);
        h.host.model().setActivePane(shell->id(), leaf->id());

        auto const snapshot = captureWorkspace(h.host, {}, Now);
        REQUIRE(snapshot.workspaces.size() == 1);
        REQUIRE(snapshot.workspaces[0].tabs.size() == 1);
        auto const& captured = snapshot.workspaces[0].tabs[0];
        CHECK(captured.root.children.empty());
        CHECK(captured.root.pane.identity->uuid == h.host.objects().uuidOf(shellSession)->bytes);
        CHECK(captured.activeLeaf == 0);
    }
    SECTION("an active agent tab is remapped as a served one is")
    {
        auto& agent = openOwned(h);
        auto* const last = h.host.createTab();
        REQUIRE(last != nullptr);
        h.host.model().activateTab(workspace, agent.id());
        auto const snapshot = captureWorkspace(h.host, {}, Now);
        REQUIRE(snapshot.workspaces.size() == 1);
        REQUIRE(snapshot.workspaces[0].tabs.size() == 2);
        CHECK(snapshot.workspaces[0].activeTab == 1);
    }
}

TEST_CASE("an owned pane is never re-run", "[vthost][snapshot][capture][panes]")
{
    // Captured, then restored cold with `claude` a program to re-run: no pane comes back running it, and none
    // with the agent's UUID.
    auto h = CaptureHarness {};
    std::ignore = h.host.createTab();
    auto& agent = openOwned(h);
    auto const agentUuid = h.host.objects().uuidOf(agent.rootPane()->session());
    REQUIRE(agentUuid.has_value());
    auto const snapshot = captureWorkspace(h.host, {}, Now);
    auto uuids = std::vector<ObjectUuid> {};
    for (auto const& workspace: snapshot.workspaces)
        for (auto const& tab: workspace.tabs)
            collectPaneUuids(tab.root, uuids);
    CHECK_FALSE(std::ranges::contains(uuids, agentUuid->bytes));

    auto spawned = std::vector<std::optional<vtpty::Process::ExecInfo>> {};
    auto restored =
        SessionHost { h.loop,
                      [&](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const& shell) {
                          spawned.push_back(shell);
                          return std::make_unique<vtpty::MockPty>(size);
                      },
                      vtbackend::Settings {},
                      core::defaultEnvironment(),
                      SessionHostConfig { .pumping = SessionPumping::Manual,
                                          .restore = WorkspaceRestore { .snapshot = snapshot,
                                                                        .rerunPrograms = { "claude" } } } };
    CHECK_FALSE(spawned.empty());
    for (auto const& shell: spawned)
        CHECK((!shell || shell->program != "claude"));
    // Nothing the restore spawned is the agent, nor knows its pane by its UUID: a pane the snapshot does not
    // hold has nothing for rerun_programs to re-run.
    for (auto const session: restored.sessionIds())
        CHECK(restored.objects().uuidOf(session) != agentUuid);
    CHECK(restored.sessionCount() == 1);
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

    auto const& tailed = withTail.workspaces.at(0).tabs.at(0).root.pane;
    auto const& bare = without.workspaces.at(0).tabs.at(0).root.pane;
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

    CHECK_FALSE(captureWorkspace(h.host, {}, Now).workspaces.at(0).tabs.at(0).root.pane.runningCommandLine);

    terminal.writeToScreen(test::commandStarted("vim notes.md"));
    CHECK(captureWorkspace(h.host, {}, Now).workspaces.at(0).tabs.at(0).root.pane.runningCommandLine
          == "vim notes.md");

    terminal.writeToScreen(test::commandEnd(0));
    CHECK_FALSE(captureWorkspace(h.host, {}, Now).workspaces.at(0).tabs.at(0).root.pane.runningCommandLine);
}
