// SPDX-License-Identifier: Apache-2.0
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/SidebarDropPolicy.hpp>
#include <contour/window/SidebarModel.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QAbstractItemModelTester>
#include <QtTest/QSignalSpy>

using namespace contour::window;
using namespace std::chrono_literals;
using vtbackend::ProgramState;
using vtbackend::ProgramStatusRecord;

namespace
{

constexpr auto Epoch = std::chrono::system_clock::time_point { std::chrono::seconds { 1'000'000 } };

/// A source that lists what the test sets and records what it is asked to select.
struct FakeSource final: IWorkspaceDirectorySource
{
    WorkspaceDirectory listed;
    std::vector<uint64_t> selected;
    mutable int reads = 0; ///< How often the directory was asked for.

    [[nodiscard]] WorkspaceDirectory directory() const override
    {
        ++reads;
        return listed;
    }
    SelectResult select(uint64_t workspace) override
    {
        selected.push_back(workspace);
        return SelectResult::Requested;
    }
};

/// A window that records what the sidebar asks of it.
struct FakeHost final: ISidebarHost
{
    FakeSource* source = nullptr;
    std::optional<TabRef> displayed;
    std::vector<PaneKey> shown;
    std::vector<PaneKey> seen;
    std::vector<std::pair<uint64_t, std::string>> renamed;
    std::vector<std::pair<uint64_t, std::size_t>> movedWorkspaces;
    std::vector<std::pair<TabRef, std::pair<uint64_t, std::size_t>>> movedTabs;
    std::vector<SidebarRowKey> menus;

    [[nodiscard]] IWorkspaceDirectorySource* directorySource() const override { return source; }
    [[nodiscard]] std::optional<TabRef> displayedTab() const override { return displayed; }
    void showPane(PaneKey pane) override { shown.push_back(pane); }
    void markSeen(PaneKey pane) override { seen.push_back(pane); }
    void renameWorkspace(uint64_t workspace, std::string const& name) override
    {
        renamed.emplace_back(workspace, name);
    }
    void moveWorkspace(uint64_t workspace, std::size_t to) override
    {
        movedWorkspaces.emplace_back(workspace, to);
    }
    void moveTab(TabRef tab, uint64_t toWorkspace, std::size_t toPosition) override
    {
        movedTabs.push_back({ tab, { toWorkspace, toPosition } });
    }
    void openContextMenu(SidebarRow const& row, QPointF /*position*/) override { menus.push_back(row.key); }
};

DirectoryPane paneOf(uint64_t key, std::string title = "shell")
{
    auto pane = DirectoryPane {};
    pane.key = PaneKey { key };
    pane.title = std::move(title);
    return pane;
}

DirectoryWorkspace workspaceOf(uint64_t id, std::string label, std::vector<DirectoryPane> panes)
{
    auto workspace = DirectoryWorkspace {};
    workspace.id = id;
    workspace.label = std::move(label);
    // One tab per pane, its id the pane's.
    for (auto& pane: panes)
    {
        auto tab = DirectoryTab {};
        tab.id = pane.key.value;
        tab.label = pane.title;
        tab.panes.push_back(std::move(pane));
        workspace.tabs.push_back(std::move(tab));
    }
    return workspace;
}

void letEventLoopRun()
{
    QCoreApplication::processEvents();
}

ProgramStatusRecord recordOf(ProgramState state, std::chrono::seconds age = 30s)
{
    auto record = ProgramStatusRecord {};
    record.state = state;
    record.app = "app";
    record.since = Epoch - age;
    record.generation = 1;
    return record;
}

/// A model over a fake window, source, clock and announcer.
struct SidebarUnderTest
{
    FakeSource source;
    FakeHost host;
    TabAttention attention;
    core::platform::ManualWallClock clock { Epoch };
    contour::test::RecordingAnnouncer announcer;
    std::optional<SidebarModel> model;

    explicit SidebarUnderTest(std::vector<DirectoryWorkspace> workspaces)
    {
        source.listed.workspaces = std::move(workspaces);
        if (!source.listed.workspaces.empty())
            source.listed.selected = source.listed.workspaces.front().id;
        host.source = &source;
        model.emplace(host, attention, clock, announcer);
    }

    SidebarModel& sidebar() { return *model; }
};

} // namespace

TEST_CASE("SidebarModel: an output burst changes the model once", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    REQUIRE(t.sidebar().rowCount() == 2);
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };
    auto structural = QSignalSpy { &t.sidebar(), &QAbstractItemModel::modelReset };
    t.source.reads = 0;

    // Every notification comes with news of its own, so a rebuild per notification would show.
    auto& pane = t.source.listed.workspaces[0].tabs[0].panes[0];
    pane.activity = vthost::proto::OutputActivity::Output;
    for (auto const serial: std::views::iota(1, 101))
    {
        pane.activitySerial = static_cast<uint64_t>(serial);
        t.sidebar().noteChanged();
    }
    CHECK(t.sidebar().isRebuildPending());
    CHECK(t.source.reads == 0);
    letEventLoopRun();

    CHECK_FALSE(t.sidebar().isRebuildPending());
    CHECK(t.source.reads == 1);
    CHECK(changed.count() == 1);
    CHECK(structural.count() == 0);

    // The next turn's burst is one rebuild again.
    for ([[maybe_unused]] auto const _: std::views::iota(0, 10))
        t.sidebar().noteChanged();
    letEventLoopRun();
    CHECK(t.source.reads == 2);
}

TEST_CASE("SidebarModel: a directory change keeps the selection and the collapsed set", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10), paneOf(11) }),
                                  workspaceOf(2, "docs", { paneOf(20) }) } };
    t.host.displayed = TabRef { .workspace = 2, .tab = 20 };
    t.sidebar().refreshNow();
    REQUIRE(t.sidebar().currentIndex() == 4);

    t.sidebar().collapse(0); // billing
    REQUIRE(t.sidebar().rowCount() == 3);
    REQUIRE(t.sidebar().currentIndex() == 2);

    t.source.listed.workspaces[1].label = "documentation";
    t.sidebar().noteChanged();
    letEventLoopRun();

    CHECK(t.sidebar().rowCount() == 3);
    CHECK(t.sidebar().currentIndex() == 2);
    CHECK(t.sidebar().index(0).data(SidebarModel::ExpansionRole).toInt()
          == static_cast<int>(RowExpansion::Collapsed));
    CHECK(t.sidebar().index(1).data(SidebarModel::TitleRole).toString() == "documentation");
}

TEST_CASE("SidebarModel: a new workspace inserts rows without a reset", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    auto resets = QSignalSpy { &t.sidebar(), &QAbstractItemModel::modelReset };
    auto inserts = QSignalSpy { &t.sidebar(), &QAbstractItemModel::rowsInserted };
    auto removals = QSignalSpy { &t.sidebar(), &QAbstractItemModel::rowsRemoved };

    t.source.listed.workspaces.push_back(workspaceOf(2, "docs", { paneOf(20) }));
    t.sidebar().noteChanged();
    letEventLoopRun();

    CHECK(t.sidebar().rowCount() == 4);
    CHECK(resets.count() == 0);
    REQUIRE(inserts.count() == 1);
    CHECK(inserts.front().at(1).toInt() == 2);
    CHECK(inserts.front().at(2).toInt() == 3);

    // Taking the first one away is a removal, and what stays keeps its place in the model.
    t.source.listed.workspaces.erase(t.source.listed.workspaces.begin());
    t.sidebar().noteChanged();
    letEventLoopRun();
    CHECK(removals.count() == 1);
    CHECK(resets.count() == 0);
    CHECK(t.sidebar().index(0).data(SidebarModel::TitleRole).toString() == "docs");
}

TEST_CASE("SidebarModel: reordering workspaces moves rows without a reset", "[contour][sidebar]")
{
    auto t =
        SidebarUnderTest { { workspaceOf(1, "a", { paneOf(10) }), workspaceOf(2, "b", { paneOf(20) }) } };
    auto resets = QSignalSpy { &t.sidebar(), &QAbstractItemModel::modelReset };
    auto moves = QSignalSpy { &t.sidebar(), &QAbstractItemModel::rowsMoved };

    std::ranges::swap(t.source.listed.workspaces[0], t.source.listed.workspaces[1]);
    t.sidebar().refreshNow();

    CHECK(resets.count() == 0);
    CHECK(moves.count() >= 1);
    CHECK(t.sidebar().index(0).data(SidebarModel::TitleRole).toString() == "b");
    CHECK(t.sidebar().index(2).data(SidebarModel::TitleRole).toString() == "a");
}

TEST_CASE("SidebarModel: the timer stops when no row has a live time", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    t.sidebar().setPresence(SidebarPresence::Shown);
    CHECK_FALSE(t.sidebar().isTimerActive());

    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Working) };
    t.sidebar().refreshNow();
    REQUIRE(t.sidebar().index(1).data(SidebarModel::TimeKindRole).toInt()
            == static_cast<int>(TimeKind::Live));
    CHECK(t.sidebar().isTimerActive());

    t.source.listed.workspaces[0].tabs[0].panes[0].status.clear();
    t.sidebar().refreshNow();
    CHECK_FALSE(t.sidebar().isTimerActive());
}

TEST_CASE("SidebarModel: the timer stops while the window is hidden", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Working) };
    t.sidebar().refreshNow();
    CHECK_FALSE(t.sidebar().isTimerActive());

    t.sidebar().setPresence(SidebarPresence::Shown);
    CHECK(t.sidebar().isTimerActive());

    t.sidebar().setPresence(SidebarPresence::Hidden);
    CHECK_FALSE(t.sidebar().isTimerActive());
}

TEST_CASE("SidebarModel: a tick changes only the running times", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Working, 30s) };
    t.sidebar().refreshNow();
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };

    t.clock.advance(5s);
    t.sidebar().refreshNow();

    REQUIRE(changed.count() == 1);
    auto const roles = changed.front().at(2).value<QList<int>>();
    CHECK(roles == QList<int> { SidebarModel::TimeTextRole });
}

TEST_CASE("SidebarModel: a title change names every role the title is served in", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };

    t.source.listed.workspaces[0].label = "invoices";
    t.sidebar().refreshNow();

    REQUIRE(changed.count() == 1);
    auto const roles = changed.front().at(2).value<QList<int>>();
    CHECK(roles.contains(SidebarModel::TitleRole));
    CHECK(roles.contains(Qt::DisplayRole));
    CHECK(roles.contains(Qt::AccessibleTextRole));
    CHECK(roles.contains(SidebarModel::AccessibleNameRole));
    CHECK(t.sidebar().index(0).data(Qt::DisplayRole).toString() == "invoices");
}

TEST_CASE("SidebarModel: activate selects, activates and marks seen", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }),
                                  workspaceOf(2, "docs", { paneOf(20), paneOf(21) }) } };

    SECTION("a workspace row selects the workspace")
    {
        t.sidebar().activate(2);
        CHECK(t.source.selected == std::vector<uint64_t> { 2 });
        CHECK(t.host.shown.empty());
    }
    SECTION("a tab row shows the tab's first pane")
    {
        t.sidebar().activate(3);
        CHECK(t.host.shown == std::vector<PaneKey> { PaneKey { 20 } });
    }
    SECTION("an out-of-range row does nothing")
    {
        t.sidebar().activate(99);
        CHECK(t.host.shown.empty());
        CHECK(t.source.selected.empty());
    }
    SECTION("markSeen counts every pane under the row")
    {
        t.sidebar().markSeen(2);
        CHECK(t.host.seen == std::vector<PaneKey> { PaneKey { 20 }, PaneKey { 21 } });
    }
}

TEST_CASE("SidebarModel: a background needs-you is announced politely once", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }),
                                  workspaceOf(2, "docs", { paneOf(20) }) } };
    t.sidebar().setActivity(WindowActivity::Active);
    REQUIRE(t.announcer.said.empty());

    auto const blocked = [&](uint64_t workspace) {
        t.source.listed.workspaces[workspace].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    };

    SECTION("in the workspace the window shows, nothing is said")
    {
        blocked(0);
        t.sidebar().refreshNow();
        CHECK(t.announcer.said.empty());
    }
    SECTION("in another, it is said once")
    {
        blocked(1);
        t.sidebar().refreshNow();
        t.sidebar().refreshNow();
        REQUIRE(t.announcer.said.size() == 1);
        CHECK(t.announcer.said.front().politeness == QAccessible::AnnouncementPoliteness::Polite);
        CHECK(t.announcer.said.front().message.contains("docs"));
    }
    SECTION("while the window is not active, nothing is said")
    {
        t.sidebar().setActivity(WindowActivity::Inactive);
        blocked(1);
        t.sidebar().refreshNow();
        CHECK(t.announcer.said.empty());
    }
}

TEST_CASE("SidebarModel: swapping the window's source is a new baseline, not news", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    t.sidebar().setActivity(WindowActivity::Active);

    // Another daemon, already holding a pane that needs the user, in a workspace this window does not show.
    auto other = FakeSource {};
    other.listed.workspaces = { workspaceOf(7, "ops", { paneOf(70) }),
                                workspaceOf(8, "docs", { paneOf(80) }) };
    other.listed.selected = 7;
    other.listed.workspaces[1].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.host.source = &other;
    t.sidebar().refreshNow();
    CHECK(t.announcer.said.empty());
    CHECK(t.sidebar().rowCount() == 4);

    // Once baselined, a new need in it is news.
    other.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    other.listed.selected = 8;
    t.sidebar().refreshNow();
    CHECK(t.announcer.said.size() == 1);

    // Attaching a source to a window that had none is a baseline too.
    auto bare = SidebarUnderTest { {} };
    bare.sidebar().setActivity(WindowActivity::Active);
    bare.host.source = &other;
    bare.sidebar().refreshNow();
    CHECK(bare.announcer.said.empty());
}

TEST_CASE("SidebarModel: rename calls the window's rename", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };

    t.sidebar().rename(1, "ignored: a tab"); // not a workspace row
    CHECK(t.host.renamed.empty());

    t.sidebar().rename(0, "invoices");
    REQUIRE(t.host.renamed.size() == 1);
    CHECK(t.host.renamed.front() == std::pair { uint64_t { 1 }, std::string { "invoices" } });
}

TEST_CASE("SidebarModel: a drop resolves to the window's move operations", "[contour][sidebar]")
{
    auto t =
        SidebarUnderTest { { workspaceOf(1, "a", { paneOf(10) }), workspaceOf(2, "b", { paneOf(20) }) } };
    auto const host = HostIdentity { .kind = HostKind::Process, .id = "x" };

    SECTION("a workspace dropped on the lower half of another goes after it")
    {
        auto drag = NodeDrag {};
        drag.payload = DropPayload::Workspace;
        drag.host = host;
        drag.workspace = 1;
        t.sidebar().drop(2, 0.8, writeNodeDrag(drag));
        REQUIRE(t.host.movedWorkspaces.size() == 1);
        CHECK(t.host.movedWorkspaces.front() == std::pair { uint64_t { 1 }, std::size_t { 2 } });
    }
    SECTION("a one-pane tab's pane dropped into the middle of a workspace row is a tab move")
    {
        auto drag = NodeDrag {};
        drag.payload = DropPayload::Pane;
        drag.host = host;
        drag.sessions = { 10 };
        t.sidebar().drop(2, 0.5, writeNodeDrag(drag));
        REQUIRE(t.host.movedTabs.size() == 1);
        CHECK(t.host.movedTabs.front().first == TabRef { .workspace = 1, .tab = 10 });
        CHECK(t.host.movedTabs.front().second.first == 2);
        CHECK(t.host.movedTabs.front().second.second == AppendTab);
    }
    SECTION("garbage is refused")
    {
        t.sidebar().drop(2, 0.5, "not json");
        CHECK(t.host.movedWorkspaces.empty());
        CHECK(t.host.movedTabs.empty());
    }
}

TEST_CASE("SidebarModel: a row's accessible name carries its kind, state and status", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.sidebar().refreshNow();

    CHECK(t.sidebar().index(0).data(SidebarModel::AccessibleNameRole).toString()
          == "billing, workspace, expanded, 1 needs you");
    CHECK(t.sidebar().footerNeedYou() == 1);
    CHECK(t.sidebar().footerWorking() == 0);
}

TEST_CASE("SidebarModel: the filter narrows the rows and leaves the footer", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }),
                                  workspaceOf(2, "docs", { paneOf(20) }) } };
    t.source.listed.workspaces[1].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.sidebar().refreshNow();

    t.sidebar().setFilter("bill");
    CHECK(t.sidebar().rowCount() == 1);
    CHECK(t.sidebar().footerNeedYou() == 1);

    t.sidebar().setFilter({});
    CHECK(t.sidebar().rowCount() == 4);
}

TEST_CASE("SidebarModel: a context menu is the window's to open", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { workspaceOf(1, "billing", { paneOf(10) }) } };
    t.sidebar().openContextMenu(1, QPointF { 4, 5 });
    REQUIRE(t.host.menus.size() == 1);
    CHECK(t.host.menus.front().kind == SidebarRowKind::Tab);
}

namespace
{

/// A seeded random walk over what a window lists and what the user does to the sidebar.
struct Walk
{
    SidebarUnderTest t { {} };
    std::mt19937 random;
    uint64_t nextId = 100;
    SidebarState expectedState; ///< What the model's own state should be, kept by the test.

    explicit Walk(uint32_t seed): random { seed } {}

    [[nodiscard]] std::size_t pick(std::size_t bound)
    {
        return std::uniform_int_distribution<std::size_t> { 0, bound - 1 }(random);
    }

    [[nodiscard]] auto& workspaces() { return t.source.listed.workspaces; }

    /// A random tab of a random workspace, if any.
    [[nodiscard]] DirectoryTab* randomTab(DirectoryWorkspace** owner = nullptr)
    {
        auto holders = std::vector<DirectoryWorkspace*> {};
        for (auto& workspace: workspaces())
            if (!workspace.tabs.empty())
                holders.push_back(&workspace);
        if (holders.empty())
            return nullptr;
        auto* const holder = holders[pick(holders.size())];
        if (owner != nullptr)
            *owner = holder;
        return &holder->tabs[pick(holder->tabs.size())];
    }

    void step()
    {
        switch (pick(11))
        {
            case 0: { // a workspace appears, anywhere in the list
                auto const id = nextId++;
                auto workspace = workspaceOf(
                    id, std::format("ws{}", id), { paneOf(nextId++, std::format("p{}", nextId)) });
                workspaces().insert(workspaces().begin()
                                        + static_cast<std::ptrdiff_t>(pick(workspaces().size() + 1)),
                                    std::move(workspace));
                break;
            }
            case 1: // a workspace goes
                if (!workspaces().empty())
                    workspaces().erase(workspaces().begin()
                                       + static_cast<std::ptrdiff_t>(pick(workspaces().size())));
                break;
            case 2: // two workspaces trade places
                if (workspaces().size() > 1)
                    std::ranges::swap(workspaces()[pick(workspaces().size())],
                                      workspaces()[pick(workspaces().size())]);
                break;
            case 3: // a workspace is moved to another place
                if (workspaces().size() > 1)
                {
                    auto const from = pick(workspaces().size());
                    auto moved = std::move(workspaces()[from]);
                    workspaces().erase(workspaces().begin() + static_cast<std::ptrdiff_t>(from));
                    workspaces().insert(workspaces().begin()
                                            + static_cast<std::ptrdiff_t>(pick(workspaces().size() + 1)),
                                        std::move(moved));
                }
                break;
            case 4: { // a tab moves to a workspace
                auto* holder = static_cast<DirectoryWorkspace*>(nullptr);
                auto* const tab = randomTab(&holder);
                if (tab == nullptr)
                    break;
                auto moved = std::move(*tab);
                auto const index = static_cast<std::ptrdiff_t>(tab - holder->tabs.data());
                holder->tabs.erase(holder->tabs.begin() + index);
                auto& target = workspaces()[pick(workspaces().size())];
                target.tabs.insert(target.tabs.begin()
                                       + static_cast<std::ptrdiff_t>(pick(target.tabs.size() + 1)),
                                   std::move(moved));
                break;
            }
            case 5: { // a pane is split off
                if (auto* const tab = randomTab())
                {
                    auto const id = nextId++;
                    tab->panes.insert(tab->panes.begin()
                                          + static_cast<std::ptrdiff_t>(pick(tab->panes.size() + 1)),
                                      paneOf(id, std::format("p{}", id)));
                }
                break;
            }
            case 6: { // a pane closes; a tab without panes goes with it
                auto* holder = static_cast<DirectoryWorkspace*>(nullptr);
                auto* const tab = randomTab(&holder);
                if (tab == nullptr)
                    break;
                tab->panes.erase(tab->panes.begin() + static_cast<std::ptrdiff_t>(pick(tab->panes.size())));
                if (tab->panes.empty())
                    holder->tabs.erase(holder->tabs.begin() + (tab - holder->tabs.data()));
                break;
            }
            case 7: // a pane starts to ask for something, or stops
                if (auto* const tab = randomTab())
                {
                    auto& pane = tab->panes[pick(tab->panes.size())];
                    auto const state =
                        std::array { ProgramState::Working, ProgramState::Blocked, ProgramState::Done };
                    if (pick(3) == 0)
                        pane.status.clear();
                    else
                        pane.status = { recordOf(state[pick(state.size())]) };
                }
                break;
            case 8: // a row is collapsed or expanded
                if (t.sidebar().rowCount() > 0)
                {
                    auto const row = static_cast<int>(pick(static_cast<std::size_t>(t.sidebar().rowCount())));
                    auto const& shown = t.sidebar().rows()[static_cast<std::size_t>(row)];
                    if (shown.expansion != RowExpansion::None)
                    {
                        auto const collapsed = shown.expansion == RowExpansion::Collapsed;
                        auto const tab = TabRef { .workspace = shown.key.workspace, .tab = shown.key.tab };
                        if (shown.key.kind == SidebarRowKind::Workspace)
                        {
                            if (collapsed)
                                expectedState.collapsedWorkspaces.erase(shown.key.workspace);
                            else
                                expectedState.collapsedWorkspaces.insert(shown.key.workspace);
                        }
                        else if (collapsed)
                            expectedState.collapsedTabs.erase(tab);
                        else
                            expectedState.collapsedTabs.insert(tab);
                        t.sidebar().toggle(row);
                    }
                }
                break;
            case 9: { // the filter is typed, or cleared
                auto const words = std::array<char const*, 4> { "", "ws", "p1", "zzz" };
                expectedState.filter = words[pick(words.size())];
                t.sidebar().setFilter(QString::fromStdString(expectedState.filter));
                break;
            }
            default: // the window shows another tab
                if (auto* const tab = randomTab())
                {
                    auto const holder = std::ranges::find_if(workspaces(), [&](DirectoryWorkspace const& w) {
                        return std::ranges::any_of(w.tabs,
                                                   [&](DirectoryTab const& each) { return &each == tab; });
                    });
                    t.source.listed.selected = holder->id;
                    t.host.displayed = TabRef { .workspace = holder->id, .tab = tab->id };
                }
                break;
        }
        // A tab id is unique only within its workspace, and the walk's are unique everywhere.
        t.sidebar().refreshNow();
    }

    /// The rows a freshly built sidebar of the directory would show.
    [[nodiscard]] std::vector<SidebarRow> fresh() const
    {
        return sidebarRows(t.source.listed, t.attention, expectedState, t.clock.now(), t.host.displayed);
    }
};

} // namespace

TEST_CASE("SidebarModel: a random walk keeps the model equal to a fresh build", "[contour][sidebar][fuzz]")
{
    for (auto const seed: std::views::iota(1u, 9u))
    {
        CAPTURE(seed);
        auto walk = Walk { seed };
        auto const tester =
            QAbstractItemModelTester { &walk.t.sidebar(),
                                       QAbstractItemModelTester::FailureReportingMode::Fatal };
        auto resets = QSignalSpy { &walk.t.sidebar(), &QAbstractItemModel::modelReset };
        walk.t.sidebar().refreshNow();

        for (auto const step: std::views::iota(0, 250))
        {
            CAPTURE(step);
            walk.step();
            REQUIRE(walk.t.sidebar().rows() == walk.fresh());
            REQUIRE(walk.t.sidebar().rowCount() == static_cast<int>(walk.fresh().size()));
        }
        CHECK(resets.count() == 0);
    }
}
