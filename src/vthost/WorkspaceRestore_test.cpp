// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <vthost/HistorySeed.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshotCodec.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;
using namespace vthost;
using vtworkspace::SplitState;

namespace
{
/// A stand-in for an attached window that shows the workspaces it is given.
class ShowingWindow final: public vthost::SessionStreamEvents
{
  public:
    [[nodiscard]] bool follows(vtworkspace::WorkspaceId workspace) const noexcept override
    {
        return shown.contains(workspace.value);
    }

    std::set<uint64_t> shown {}; ///< The workspaces it shows.
};

constexpr auto DividerOf =
    RestoreDivider { .sessionEndedAt = std::chrono::local_days { std::chrono::year { 2026 } / 10 / 25 }
                                       + std::chrono::hours { 14 } + std::chrono::minutes { 2 },
                     .today = std::chrono::local_days { std::chrono::year { 2026 } / 10 / 25 } };

constexpr auto Now = std::chrono::sys_seconds { std::chrono::seconds { 1'761'000'000 } };

/// What a PTY factory was asked for, in order; it fails every spawn while @ref failing is set, and the first
/// @ref failFirst ones.
struct RecordingFactory
{
    std::vector<std::optional<vtpty::Process::ExecInfo>> overrides;
    bool failing = false;
    std::size_t failFirst = 0;

    [[nodiscard]] PtyFactory factory()
    {
        return [this](vtbackend::PageSize size,
                      std::optional<vtpty::Process::ExecInfo> const& shell) -> std::unique_ptr<vtpty::Pty> {
            overrides.push_back(shell);
            if (failing || overrides.size() <= failFirst)
                return nullptr;
            return std::make_unique<vtpty::MockPty>(size);
        };
    }
};

/// A loop on a manual clock and the file system the restored hosts look directories up in.
struct RestoreHarness
{
    core::platform::ManualClock clock {};
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend, clock };
    core::platform::testing::InMemoryFileSystem files {};
    RecordingFactory recorded {};

    [[nodiscard]] std::unique_ptr<SessionHost> makeHost(SessionHostConfig config)
    {
        config.pumping = SessionPumping::Manual;
        config.fileSystem = files;
        return std::make_unique<SessionHost>(
            loop, recorded.factory(), vtbackend::Settings {}, core::defaultEnvironment(), std::move(config));
    }

    [[nodiscard]] std::unique_ptr<SessionHost> restore(WorkspaceRestore restore)
    {
        return makeHost(SessionHostConfig { .restore = std::move(restore) });
    }
};

/// @return The terminal of @p host's active pane of its @p tabIndex-th tab in its own workspace.
[[nodiscard]] vtbackend::Terminal& terminalOf(SessionHost& host, int tabIndex)
{
    auto const* const workspace = host.model().workspace(host.workspaceId());
    REQUIRE(workspace != nullptr);
    auto const* const tab = workspace->tabAt(tabIndex);
    REQUIRE(tab != nullptr);
    auto* const terminal = host.terminal(tab->activePane()->session());
    REQUIRE(terminal != nullptr);
    return *terminal;
}

[[nodiscard]] vtpty::MockPty& mockOf(vtbackend::Terminal& terminal)
{
    return dynamic_cast<vtpty::MockPty&>(dynamic_cast<TappingPty&>(terminal.device()).inner());
}

/// Has @p terminal's PTY yield @p output and parses it, as one turn of a pump does.
void pump(vtbackend::Terminal& terminal, std::string_view output)
{
    mockOf(terminal).appendStdOutBuffer(output);
    std::ignore = terminal.processInputOnce();
}

[[nodiscard]] PaneNode leaf(PaneSnapshot pane = {})
{
    return PaneNode { .pane = std::move(pane) };
}

/// @return A snapshot of one workspace holding one tab per pane given.
[[nodiscard]] WorkspaceSnapshot workspaceOf(std::vector<PaneSnapshot> panes)
{
    auto workspace = WorkspaceLayoutSnapshot {};
    for (auto& pane: panes)
        workspace.tabs.push_back(TabSnapshot { .root = leaf(std::move(pane)) });
    return WorkspaceSnapshot { .takenAt = Now, .workspaces = { std::move(workspace) } };
}

void checkSameShape(PaneNode const& restored, PaneNode const& saved)
{
    CHECK(restored.split == saved.split);
    REQUIRE(restored.children.size() == saved.children.size());
    if (saved.children.empty())
        return;
    CHECK(restored.ratio == Catch::Approx(saved.ratio));
    checkSameShape(restored.children[0], saved.children[0]);
    checkSameShape(restored.children[1], saved.children[1]);
}

} // namespace

TEST_CASE("a captured workspace comes back with the same shape", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto source = h.makeHost({});
    auto* first = source->createTab();
    auto* second = source->createTab();
    REQUIRE((first != nullptr && second != nullptr));
    source->splitActivePane(second->id(), SplitState::Vertical, 0.3);
    source->splitActivePane(second->id(), SplitState::Horizontal, 0.5);
    source->model().toggleActivePaneZoom(second->id());
    source->model().setTabTitle(second->id(), "build");
    source->model().setTabColor(
        second->id(), vtworkspace::TabColorSource::User, vtbackend::RGBColor { 0x336699U });
    REQUIRE(source->createWorkspace() != nullptr);
    source->model().activateTab(source->workspaceId(), second->id());

    auto const captured = captureWorkspace(*source, { .scrollback = ScrollbackPersistence::Off }, Now);
    auto const bytes = encodeSnapshot(captured);
    auto decoded = decodeSnapshot(bytes);
    REQUIRE(decoded.has_value());
    auto const snap = decoded->snapshot;

    auto const restored = h.restore(WorkspaceRestore { .snapshot = snap });
    CHECK(restored->restoreOutcome() == RestoreOutcome::Realized);
    CHECK(restored->sessionCount() == source->sessionCount());
    auto const again = captureWorkspace(*restored, { .scrollback = ScrollbackPersistence::Off }, Now);
    REQUIRE(again.workspaces.size() == snap.workspaces.size());
    for (auto const workspaceIndex: std::views::iota(std::size_t { 0 }, snap.workspaces.size()))
    {
        auto const& want = snap.workspaces[workspaceIndex];
        auto const& got = again.workspaces[workspaceIndex];
        CHECK(got.activeTab == want.activeTab);
        REQUIRE(got.tabs.size() == want.tabs.size());
        for (auto const tabIndex: std::views::iota(std::size_t { 0 }, want.tabs.size()))
        {
            auto const& wantTab = want.tabs[tabIndex];
            auto const& gotTab = got.tabs[tabIndex];
            CHECK(gotTab.title == wantTab.title);
            CHECK(gotTab.color == wantTab.color);
            CHECK(gotTab.activeLeaf == wantTab.activeLeaf);
            CHECK(gotTab.zoom == wantTab.zoom);
            checkSameShape(gotTab.root, wantTab.root);
        }
    }
}

TEST_CASE("each restored pane starts in its directory with its profile", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    h.files.addDirectory("/work/a");
    auto profiles = std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, ResolvedProfile> {
            { "default", vthost::testing::fakeProfile("/bin/sh", vtbackend::LineCount(100)) },
            { "work", vthost::testing::fakeProfile("/bin/zsh", vtbackend::LineCount(100)) } },
        "default");
    auto const host = h.makeHost(SessionHostConfig {
        .profiles = profiles,
        .restore = WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot { .workingDirectory = "/work/a",
                                                                               .profile = "work" } }) } });

    REQUIRE(h.recorded.overrides.size() == 1);
    REQUIRE(h.recorded.overrides[0].has_value());
    CHECK(h.recorded.overrides[0]->workingDirectory == "/work/a");
    CHECK(h.recorded.overrides[0]->program == "/bin/zsh");
    auto const session = host->model().workspace(host->workspaceId())->tabAt(0)->activePane()->session();
    CHECK(host->paneRuntime(session).profile == "work");
}

TEST_CASE("a directory that is gone opens the pane in the default directory", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto capture = core::log::ScopedCapture {};
    auto const host = h.restore(
        WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot { .workingDirectory = "/gone" } }) });

    CHECK(host->sessionCount() == 1);
    REQUIRE(h.recorded.overrides.size() == 1);
    CHECK((!h.recorded.overrides[0] || h.recorded.overrides[0]->workingDirectory.empty()));
    CHECK(capture.contains("/gone no longer exists; the pane starts in the shell's default directory"));
}

TEST_CASE("restored history sits above the divider and the new prompt", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto source = h.makeHost({});
    REQUIRE(source->createTab() != nullptr);
    terminalOf(*source, 0).writeToScreen("old 1\r\nold 2\r\nold 3\r\n");
    auto const captured = captureWorkspace(*source, {}, Now);
    REQUIRE(captured.workspaces.at(0).tabs.at(0).root.pane.history.has_value());

    auto const host = h.restore(WorkspaceRestore { .snapshot = captured, .divider = DividerOf });

    auto& terminal = terminalOf(*host, 0);
    auto const& grid = terminal.primaryScreen().grid();
    CHECK(grid.lineText(vtbackend::LineOffset(-3)).starts_with("old 1"));
    CHECK(grid.lineText(vtbackend::LineOffset(-1)).starts_with("old 3"));
    CHECK(grid.lineText(vtbackend::LineOffset(0)).contains("Restored · session ended 14:02"));
}

TEST_CASE("a pane restored without history still has the divider on its first row",
          "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto const host =
        h.restore(WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot {} }), .divider = DividerOf });

    auto const& grid = terminalOf(*host, 0).primaryScreen().grid();
    CHECK(grid.lineText(vtbackend::LineOffset(0)).contains("Restored · session ended 14:02"));
    CHECK(grid.historyLineCount() == vtbackend::LineCount(0));
}

TEST_CASE("an allow-listed program is typed into its restored shell once it shows a prompt",
          "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto const host = h.restore(
        WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot { .foregroundProcess = "htop" },
                                                     PaneSnapshot { .foregroundProcess = "bash" } }),
                           .rerunPrograms = { "htop" } });
    auto& restored = terminalOf(*host, 0);
    auto& other = terminalOf(*host, 1);
    std::ignore = h.loop.runUntilIdle();
    CHECK(mockOf(restored).stdinBuffer().empty());

    pump(restored, "\033]133;A\033\\");
    pump(other, "\033]133;A\033\\");
    std::ignore = h.loop.runUntilIdle();
    CHECK(mockOf(restored).stdinBuffer() == "htop\r");

    pump(restored, "\033]133;A\033\\");
    std::ignore = h.loop.runUntilIdle();
    h.clock.advance(InitialInputFallback);
    std::ignore = h.loop.runUntilIdle();
    CHECK(mockOf(restored).stdinBuffer() == "htop\r");
    CHECK(mockOf(other).stdinBuffer().empty());
}

TEST_CASE("a pane whose command was not re-run remembers it until its next command",
          "[vthost][snapshot][restore][interrupted]")
{
    auto h = RestoreHarness {};
    auto const host = h.restore(WorkspaceRestore {
        .snapshot = workspaceOf({ PaneSnapshot { .runningCommandLine = "make test" },
                                  PaneSnapshot { .foregroundProcess = "htop", .runningCommandLine = "htop" },
                                  PaneSnapshot {} }),
        .rerunPrograms = { "htop" } });
    auto& first = terminalOf(*host, 0);
    auto& second = terminalOf(*host, 1);
    auto const sessionOf = [&](int tab) {
        return host->model().workspace(host->workspaceId())->tabAt(tab)->activePane()->session();
    };
    std::ignore = h.loop.runUntilIdle();

    SECTION("only the pane whose command no program re-ran carries it")
    {
        CHECK(host->interruptedCommandOf(sessionOf(0)) == "make test");
        CHECK(host->interruptedCommandOf(sessionOf(1)).empty());
        CHECK(host->interruptedCommandOf(sessionOf(2)).empty());

        pump(second, "\033]133;A\033\\");
        std::ignore = h.loop.runUntilIdle();
        CHECK(mockOf(second).stdinBuffer() == "htop\r");
    }

    SECTION("the shell's prompt does not clear it; its next command does, and a later attach is told none")
    {
        pump(first, "\033]133;A\033\\");
        std::ignore = h.loop.runUntilIdle();
        CHECK(host->interruptedCommandOf(sessionOf(0)) == "make test");

        pump(first, "\033]133;B\033\\\033]133;C\033\\");
        std::ignore = h.loop.runUntilIdle();
        CHECK(host->interruptedCommandOf(sessionOf(0)).empty());
        CHECK(host->interruptedCommandOf(sessionOf(2)).empty());
    }
}

TEST_CASE("a shell without prompt marks still gets its re-run after the fallback",
          "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto const host = h.restore(
        WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot { .foregroundProcess = "htop" } }),
                           .rerunPrograms = { "htop" } });
    auto& restored = terminalOf(*host, 0);

    pump(restored, "$ ");
    h.clock.advance(InitialInputFallback - 1ms);
    std::ignore = h.loop.runUntilIdle();
    CHECK(mockOf(restored).stdinBuffer().empty());
    h.clock.advance(1ms);
    std::ignore = h.loop.runUntilIdle();
    CHECK(mockOf(restored).stdinBuffer() == "htop\r");
}

TEST_CASE("an empty or unrealizable restore falls back to the startup layout", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto const startup =
        vtworkspace::Layout { .tabs = { vtworkspace::LayoutTab {}, vtworkspace::LayoutTab {} } };

    SECTION("a restore of nothing")
    {
        auto const host =
            h.makeHost(SessionHostConfig { .startupLayout = startup, .restore = WorkspaceRestore {} });
        CHECK(host->model().workspace(host->workspaceId())->tabCount() == 2);
        CHECK(host->restoreOutcome() == RestoreOutcome::NothingRealized);
    }
    SECTION("a restore whose every spawn fails")
    {
        h.recorded.failing = true;
        auto const host = h.makeHost(SessionHostConfig {
            .startupLayout = startup, .restore = WorkspaceRestore { .snapshot = workspaceOf({ {}, {} }) } });
        CHECK(host->restoreOutcome() == RestoreOutcome::NothingRealized);
        // Both restored tabs were tried, then the startup layout's.
        CHECK(h.recorded.overrides.size() == 4);
        CHECK(host->model().workspaceCount() == 1);
    }
    SECTION("no restore at all")
    {
        auto const host = h.makeHost(SessionHostConfig { .startupLayout = startup });
        CHECK(host->restoreOutcome() == RestoreOutcome::NothingRealized);
        CHECK(host->sessionCount() == 2);
    }
}

TEST_CASE("a restore whose first workspace comes back empty fills the host's own workspace with the next",
          "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    h.recorded.failFirst = 1;
    auto workspace = workspaceOf({ {} });
    workspace.workspaces.push_back(WorkspaceLayoutSnapshot { .tabs = { TabSnapshot { .root = leaf() } } });

    auto const host = h.restore(WorkspaceRestore { .snapshot = std::move(workspace) });
    CHECK(host->restoreOutcome() == RestoreOutcome::Realized);
    CHECK(host->model().workspaceCount() == 1);
    CHECK(host->model().workspace(host->workspaceId())->tabCount() == 1);
}

namespace
{

/// @return What @p host's registry names @p session's pane, tab and workspace by, as their references.
[[nodiscard]] std::vector<std::string> refsOf(SessionHost& host, vtworkspace::SessionId session)
{
    auto const [workspace, tab, leaf] = host.model().findSessionLeaf(session);
    REQUIRE(leaf != nullptr);
    auto const& objects = host.objects();
    return {
        vtcontrol::toString(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Pane, *objects.uuidOf(session) }),
        vtcontrol::toString(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Tab, *objects.uuidOf(tab->id()) }),
        vtcontrol::toString(
            vtcontrol::ObjectRef { vtcontrol::ObjectKind::Workspace, *objects.uuidOf(workspace->id()) }),
    };
}

/// @return The CONTOUR_PANE, CONTOUR_TAB and CONTOUR_WORKSPACE a spawn was given.
[[nodiscard]] std::vector<std::string> exportedRefsOf(std::optional<vtpty::Process::ExecInfo> const& spawn)
{
    REQUIRE(spawn.has_value());
    return { spawn->env.at("CONTOUR_PANE"),
             spawn->env.at("CONTOUR_TAB"),
             spawn->env.at("CONTOUR_WORKSPACE") };
}

} // namespace

TEST_CASE("a restored pane gets a UUID and exports it to its shell", "[vthost][snapshot][restore][ids]")
{
    auto h = RestoreHarness {};
    auto const host = h.restore(WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot {} }) });
    REQUIRE(host->restoreOutcome() == RestoreOutcome::Realized);
    REQUIRE(h.recorded.overrides.size() == 1);

    auto const session = host->model().workspace(host->workspaceId())->tabAt(0)->activePane()->session();
    CHECK(host->objects().uuidOf(session).has_value());
    CHECK(exportedRefsOf(h.recorded.overrides.front()) == refsOf(*host, session));
}

TEST_CASE("restored workspaces, tabs and panes keep the UUIDs and labels they were captured with",
          "[vthost][snapshot][restore][ids]")
{
    auto h = RestoreHarness {};
    auto source = h.makeHost({});
    auto* tab = source->createTab();
    REQUIRE(tab != nullptr);
    source->splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    REQUIRE(source->createWorkspace() != nullptr);
    auto& objects = source->objects();
    auto const firstPane = tab->rootPane()->first()->session();
    objects.setLabel({ vtcontrol::ObjectKind::Pane, *objects.uuidOf(firstPane) }, "logs");
    objects.setLabel({ vtcontrol::ObjectKind::Tab, *objects.uuidOf(tab->id()) }, "ops");
    objects.setLabel({ vtcontrol::ObjectKind::Workspace, *objects.uuidOf(source->workspaceId()) }, "main");

    auto const captured = captureWorkspace(*source, { .scrollback = ScrollbackPersistence::Off }, Now);
    auto decoded = decodeSnapshot(encodeSnapshot(captured));
    REQUIRE(decoded.has_value());
    auto const& snap = decoded->snapshot;
    REQUIRE(snap.workspaces.size() == 2);
    REQUIRE(snap.workspaces[0].identity.has_value());
    CHECK(snap.workspaces[0].identity->uuid == objects.uuidOf(source->workspaceId())->bytes);
    CHECK(snap.workspaces[0].identity->label == "main");
    REQUIRE(snap.workspaces[0].tabs.front().identity.has_value());
    CHECK(snap.workspaces[0].tabs.front().identity->label == "ops");
    REQUIRE(snap.workspaces[0].tabs.front().root.children.size() == 2);
    auto const& firstLeaf = snap.workspaces[0].tabs.front().root.children[0].pane;
    REQUIRE(firstLeaf.identity.has_value());
    CHECK(firstLeaf.identity->uuid == objects.uuidOf(firstPane)->bytes);
    CHECK(firstLeaf.identity->label == "logs");
    CHECK_FALSE(snap.workspaces[0].tabs.front().root.children[1].pane.identity->label.has_value());

    h.recorded.overrides.clear();
    auto const restored = h.restore(WorkspaceRestore { .snapshot = snap });
    REQUIRE(restored->restoreOutcome() == RestoreOutcome::Realized);
    auto const* const workspace = restored->model().workspace(restored->workspaceId());
    REQUIRE(workspace != nullptr);
    auto const* const restoredTab = workspace->tabAt(0);
    REQUIRE(restoredTab != nullptr);
    auto const restoredFirst = restoredTab->rootPane()->first()->session();
    auto const& now = restored->objects();
    CHECK(now.uuidOf(restoredFirst) == objects.uuidOf(firstPane));
    CHECK(now.uuidOf(restoredTab->id()) == objects.uuidOf(tab->id()));
    CHECK(now.uuidOf(restored->workspaceId()) == objects.uuidOf(source->workspaceId()));
    CHECK(now.labelOf({ vtcontrol::ObjectKind::Pane, *objects.uuidOf(firstPane) }) == "logs");
    CHECK(now.labelOf({ vtcontrol::ObjectKind::Tab, *objects.uuidOf(tab->id()) }) == "ops");
    CHECK(now.labelOf({ vtcontrol::ObjectKind::Workspace, *objects.uuidOf(source->workspaceId()) })
          == "main");
    // The first restored spawn is the first pane, and its shell is told the ids it came back with.
    REQUIRE(h.recorded.overrides.size() == source->sessionCount());
    CHECK(exportedRefsOf(h.recorded.overrides.front()) == refsOf(*restored, restoredFirst));
}

TEST_CASE("a snapshot that repeats a UUID restores the second object under a fresh one",
          "[vthost][snapshot][restore][ids]")
{
    auto const repeated = ObjectIdentityRecord { .uuid = { 0x01, 0x9a }, .label = "twin" };
    auto h = RestoreHarness {};
    auto const host =
        h.restore(WorkspaceRestore { .snapshot = workspaceOf({ PaneSnapshot { .identity = repeated },
                                                               PaneSnapshot { .identity = repeated } }) });
    auto const* const workspace = host->model().workspace(host->workspaceId());
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->tabCount() == 2);
    auto const first = host->objects().uuidOf(workspace->tabAt(0)->activePane()->session());
    auto const second = host->objects().uuidOf(workspace->tabAt(1)->activePane()->session());
    REQUIRE((first.has_value() && second.has_value()));
    CHECK(first->bytes == repeated.uuid);
    CHECK(*second != *first);
}

namespace
{

/// @return A snapshot of one workspace per count given, each holding that many single-pane tabs.
[[nodiscard]] WorkspaceSnapshot workspacesOf(std::vector<int> const& tabCounts)
{
    auto snapshot = WorkspaceSnapshot { .takenAt = Now };
    for (auto const tabs: tabCounts)
    {
        auto workspace = WorkspaceLayoutSnapshot {};
        std::ranges::generate_n(
            std::back_inserter(workspace.tabs), tabs, [] { return TabSnapshot { .root = leaf() }; });
        snapshot.workspaces.push_back(std::move(workspace));
    }
    return snapshot;
}

} // namespace

TEST_CASE("a restore announces the workspaces and tabs it brought back, once", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto const host =
        h.restore(WorkspaceRestore { .snapshot = workspacesOf({ 3, 4 }), .divider = DividerOf });
    auto const everything = [](uint64_t) {
        return true;
    };

    CHECK(host->takeRestoreAnnouncement(everything) == RestoredCounts { .workspaces = 2, .tabs = 7 });
    CHECK_FALSE(host->takeRestoreAnnouncement(everything).has_value());
}

TEST_CASE("a workspace none of whose panes spawned is not announced", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    h.recorded.failFirst = 3; // every pane of the first workspace
    auto const host =
        h.restore(WorkspaceRestore { .snapshot = workspacesOf({ 3, 4 }), .divider = DividerOf });

    CHECK(host->takeRestoreAnnouncement([](uint64_t) { return true; })
          == RestoredCounts { .workspaces = 1, .tabs = 4 });
}

TEST_CASE("a client is told only of the workspaces it follows", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    auto const host =
        h.restore(WorkspaceRestore { .snapshot = workspacesOf({ 3, 4 }), .divider = DividerOf });
    auto const first = host->model().workspaceAt(0)->id().value;

    CHECK(host->takeRestoreAnnouncement([first](uint64_t workspace) { return workspace == first; })
          == RestoredCounts { .workspaces = 1, .tabs = 3 });
    // The question is asked of the first client only: nothing is left for another.
    CHECK_FALSE(host->takeRestoreAnnouncement([](uint64_t) { return true; }).has_value());
}

TEST_CASE("a host that restored nothing has nothing to announce", "[vthost][snapshot][restore]")
{
    auto h = RestoreHarness {};
    h.recorded.failing = true;
    auto const host = h.restore(WorkspaceRestore { .snapshot = workspacesOf({ 2 }), .divider = DividerOf });
    CHECK_FALSE(host->takeRestoreAnnouncement([](uint64_t) { return true; }).has_value());
    CHECK_FALSE(h.makeHost({})->takeRestoreAnnouncement([](uint64_t) { return true; }).has_value());
}

TEST_CASE("SessionHost.cellPixelSize.restored_from_a_snapshot", "[vthost][snapshot][restore][cell]")
{
    auto h = RestoreHarness {};
    auto snapshot = workspaceOf({ PaneSnapshot {}, PaneSnapshot {} });
    snapshot.cellPixelSize = vtpty::ImageSize { vtpty::Width(9), vtpty::Height(18) };
    auto const host = h.restore(WorkspaceRestore { .snapshot = std::move(snapshot) });

    CHECK(host->learnedCellPixelSize() == vtpty::ImageSize { vtpty::Width(9), vtpty::Height(18) });
    for (auto const tabIndex: std::views::iota(0, 2))
    {
        auto& terminal = terminalOf(*host, tabIndex);
        auto const _ = std::lock_guard { terminal };
        CHECK(terminal.cellPixelSize() == vtpty::ImageSize { vtpty::Width(9), vtpty::Height(18) });
    }
    // What a restart then writes down is what it learned.
    CHECK(captureWorkspace(*host, {}, Now).cellPixelSize == host->learnedCellPixelSize());
}

TEST_CASE("SessionHost.cellPixelSize.a_corrupt_snapshot_value_is_ignored",
          "[vthost][snapshot][restore][cell]")
{
    auto h = RestoreHarness {};
    auto snapshot = workspaceOf({ PaneSnapshot {} });
    snapshot.cellPixelSize = vtpty::ImageSize { vtpty::Width(0), vtpty::Height(18) };
    auto const host = h.restore(WorkspaceRestore { .snapshot = std::move(snapshot) });

    CHECK(host->restoreOutcome() == RestoreOutcome::Realized);
    CHECK_FALSE(host->learnedCellPixelSize().has_value());
    CHECK(host->cellPixelSize() == vtpty::ImageSize { vtpty::Width(10), vtpty::Height(20) });
}

TEST_CASE("a workspace put away stays Hidden across a restore, a shown one comes back Left",
          "[vthost][snapshot][restore][visibility]")
{
    auto h = RestoreHarness {};
    auto source = h.makeHost({});
    REQUIRE(source->createTab() != nullptr);
    auto const* const away = source->createWorkspace();
    REQUIRE(away != nullptr);
    source->noteWorkspaceReleased(away->id(), vthost::proto::WorkspaceRelease::SwitchedAway);
    REQUIRE(source->visibility(away->id()) == WorkspaceVisibility::Hidden);

    auto const captured = captureWorkspace(*source, { .scrollback = ScrollbackPersistence::Off }, Now);
    auto decoded = decodeSnapshot(encodeSnapshot(captured));
    REQUIRE(decoded.has_value());
    auto const restored = h.restore(WorkspaceRestore { .snapshot = decoded->snapshot });

    REQUIRE(restored->model().workspaceCount() == 2);
    CHECK(restored->visibility(restored->model().workspaceAt(0)->id()) == WorkspaceVisibility::Left);
    CHECK(restored->visibility(restored->model().workspaceAt(1)->id()) == WorkspaceVisibility::Hidden);
}

TEST_CASE("restored workspaces come back left by a client that went away, in the order they are restored",
          "[vthost][snapshot][restore][visibility]")
{
    // Recorded as a Closed release, so that the primary viewer a launch makes does not take up every one of
    // them beside the one the launch brings back; stamped, so that the launch brings back the last.
    auto h = RestoreHarness {};
    auto source = h.makeHost({});
    REQUIRE(source->createTab() != nullptr);
    REQUIRE(source->createWorkspace() != nullptr);
    REQUIRE(source->createWorkspace() != nullptr);

    auto const captured = captureWorkspace(*source, { .scrollback = ScrollbackPersistence::Off }, Now);
    auto decoded = decodeSnapshot(encodeSnapshot(captured));
    REQUIRE(decoded.has_value());
    auto const restored = h.restore(WorkspaceRestore { .snapshot = decoded->snapshot });

    REQUIRE(restored->model().workspaceCount() == 3);
    auto const idAt = [&](int index) {
        return restored->model().workspaceAt(index)->id();
    };
    for (auto const index: std::views::iota(0, 3))
    {
        CHECK(restored->visibility(idAt(index)) == WorkspaceVisibility::Left);
        CHECK(restored->lastRelease(idAt(index)) == vthost::proto::WorkspaceRelease::Closed);
    }
    CHECK(restored->leftAt(idAt(0)) < restored->leftAt(idAt(1)));
    CHECK(restored->leftAt(idAt(1)) < restored->leftAt(idAt(2)));
    for (auto const index: std::views::iota(0, 3))
        CHECK(restored->shownBeforeRestart(idAt(index)) == vthost::ShownBeforeRestart::No);
}

TEST_CASE("a restored workspace a window showed is the one left last, until a window shows it again",
          "[vthost][snapshot][restore][visibility]")
{
    auto h = RestoreHarness {};
    auto source = h.makeHost({});
    REQUIRE(source->createTab() != nullptr);
    auto const shownId = source->workspaceId();
    REQUIRE(source->createWorkspace() != nullptr);
    auto window = ShowingWindow {};
    window.shown.insert(shownId.value);
    source->noteClientAttached(&window);

    auto const captured = captureWorkspace(*source, { .scrollback = ScrollbackPersistence::Off }, Now);
    source->noteClientDetached(&window);
    auto decoded = decodeSnapshot(encodeSnapshot(captured));
    REQUIRE(decoded.has_value());
    auto const restored = h.restore(WorkspaceRestore { .snapshot = decoded->snapshot });

    REQUIRE(restored->model().workspaceCount() == 2);
    auto const shown = restored->model().workspaceAt(0)->id();
    auto const left = restored->model().workspaceAt(1)->id();
    CHECK(restored->shownBeforeRestart(shown) == vthost::ShownBeforeRestart::Yes);
    CHECK(restored->shownBeforeRestart(left) == vthost::ShownBeforeRestart::No);
    CHECK(restored->leftAt(shown) > restored->leftAt(left));

    auto again = ShowingWindow {};
    again.shown.insert(shown.value);
    restored->noteClientAttached(&again);
    restored->noteWorkspaceFollowed(shown, &again);
    CHECK(restored->shownBeforeRestart(shown) == vthost::ShownBeforeRestart::No);
    restored->noteClientDetached(&again);
}
