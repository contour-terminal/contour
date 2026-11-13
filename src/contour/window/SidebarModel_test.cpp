// SPDX-License-Identifier: Apache-2.0
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/test/SidebarFakes.hpp>
#include <contour/window/ChromeKeys.hpp>
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

using FakeSource = contour::test::FakeDirectorySource;
using FakeHost = contour::test::FakeSidebarHost;
using contour::test::directoryPaneOf;
using contour::test::directoryWorkspaceOf;

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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
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
    auto t =
        SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10), directoryPaneOf(11) }),
                             directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) } };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    auto resets = QSignalSpy { &t.sidebar(), &QAbstractItemModel::modelReset };
    auto inserts = QSignalSpy { &t.sidebar(), &QAbstractItemModel::rowsInserted };
    auto removals = QSignalSpy { &t.sidebar(), &QAbstractItemModel::rowsRemoved };

    t.source.listed.workspaces.push_back(directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }));
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "a", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(2, "b", { directoryPaneOf(20) }) } };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Working) };
    t.sidebar().refreshNow();
    CHECK_FALSE(t.sidebar().isTimerActive());

    t.sidebar().setPresence(SidebarPresence::Shown);
    CHECK(t.sidebar().isTimerActive());

    t.sidebar().setPresence(SidebarPresence::Hidden);
    CHECK_FALSE(t.sidebar().isTimerActive());
}

TEST_CASE("SidebarModel: a tick changes only the running times and the card that shows them",
          "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Working, 30s) };
    t.sidebar().refreshNow();
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };

    t.clock.advance(5s);
    t.sidebar().refreshNow();

    REQUIRE(changed.count() == 1);
    auto const roles = changed.front().at(2).value<QList<int>>();
    CHECK(roles == QList<int> { SidebarModel::HoverDetailRole, SidebarModel::TimeTextRole });
}

TEST_CASE("SidebarModel: a title change names every role the title is served in", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(
                                      2, "docs", { directoryPaneOf(20), directoryPaneOf(21) }) } };

    SECTION("a workspace row selects the workspace")
    {
        t.sidebar().activate(2);
        // Through the window, which drops what waited for an earlier look: the user went elsewhere.
        CHECK(t.host.shownWorkspaces == std::vector<uint64_t> { 2 });
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) } };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.sidebar().setActivity(WindowActivity::Active);

    // Another daemon, already holding a pane that needs the user, in a workspace this window does not show.
    auto other = FakeSource {};
    other.listed.workspaces = { directoryWorkspaceOf(7, "ops", { directoryPaneOf(70) }),
                                directoryWorkspaceOf(8, "docs", { directoryPaneOf(80) }) };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };

    t.sidebar().rename(1, "ignored: a tab"); // not a workspace row
    CHECK(t.host.renamed.empty());

    t.sidebar().rename(0, "invoices");
    REQUIRE(t.host.renamed.size() == 1);
    CHECK(t.host.renamed.front() == std::pair { uint64_t { 1 }, std::string { "invoices" } });
}

namespace
{
/// Workspace 1: tab 10 of panes 10 and 11, and tab 12. Workspace 2: tab 20.
[[nodiscard]] std::vector<DirectoryWorkspace> moveSample()
{
    auto first = directoryWorkspaceOf(1, "a", { directoryPaneOf(10), directoryPaneOf(12) });
    first.tabs[0].panes.push_back(directoryPaneOf(11));
    return { std::move(first), directoryWorkspaceOf(2, "b", { directoryPaneOf(20) }) };
}

/// @return The JSON of a drag of @p payload from @p host.
[[nodiscard]] QString dragOf(HostIdentity const& host,
                             DropPayload payload,
                             std::vector<uint64_t> sessions,
                             uint64_t workspace = 0)
{
    auto drag = NodeDrag {};
    drag.payload = payload;
    drag.host = host;
    drag.sessions = std::move(sessions);
    drag.workspace = workspace;
    return writeNodeDrag(drag);
}
} // namespace

TEST_CASE("SidebarModel: a drop resolves to the window's move operations", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { moveSample() };
    auto const host = t.host.identity;
    auto const rowOf = [&](SidebarRowKind kind, uint64_t workspace, uint64_t tab, uint64_t pane = 0) {
        auto const index = t.sidebar().rowIndexOf(
            SidebarRowKey { .kind = kind, .workspace = workspace, .tab = tab, .pane = PaneKey { pane } });
        REQUIRE(index >= 0);
        return index;
    };
    auto const moved = [&]() -> std::pair<SidebarNode, NodeDestination> const& {
        REQUIRE(t.host.movedNodes.size() == 1);
        return t.host.movedNodes.front();
    };

    SECTION("a workspace dropped on the lower half of another goes after it")
    {
        t.sidebar().drop(
            rowOf(SidebarRowKind::Workspace, 2, 0), 0.8, dragOf(host, DropPayload::Workspace, {}, 1));
        REQUIRE(t.host.movedWorkspaces.size() == 1);
        CHECK(t.host.movedWorkspaces.front() == std::pair { uint64_t { 1 }, std::size_t { 2 } });
    }
    SECTION("a one-pane tab's pane dropped into the middle of a workspace row is a tab move")
    {
        t.sidebar().drop(
            rowOf(SidebarRowKind::Workspace, 2, 0), 0.5, dragOf(host, DropPayload::Pane, { 12 }));
        auto const& [node, destination] = moved();
        CHECK(node == SidebarNode { TabRef { .workspace = 1, .tab = 12 } });
        REQUIRE(std::holds_alternative<MoveTab>(destination));
        CHECK(std::get<MoveTab>(destination).toWorkspace == 2);
        CHECK(std::get<MoveTab>(destination).toPosition == AppendTab);
    }
    SECTION("a pane of several dropped on a workspace row becomes a tab of it")
    {
        t.sidebar().drop(
            rowOf(SidebarRowKind::Workspace, 2, 0), 0.5, dragOf(host, DropPayload::Pane, { 11 }));
        auto const& [node, destination] = moved();
        CHECK(node == SidebarNode { PaneKey { 11 } });
        REQUIRE(std::holds_alternative<MoveTab>(destination));
        CHECK(std::get<MoveTab>(destination).toWorkspace == 2);
    }
    SECTION("a pane dropped on the middle of another tab's row joins that tab")
    {
        t.sidebar().drop(rowOf(SidebarRowKind::Tab, 2, 20), 0.5, dragOf(host, DropPayload::Pane, { 11 }));
        auto const& [node, destination] = moved();
        CHECK(node == SidebarNode { PaneKey { 11 } });
        REQUIRE(std::holds_alternative<JoinTab>(destination));
        CHECK(std::get<JoinTab>(destination).tab == TabRef { .workspace = 2, .tab = 20 });
    }
    SECTION("a pane dropped on its own tab, or on a pane row, is refused, and shows it")
    {
        auto const pane = dragOf(host, DropPayload::Pane, { 11 });
        auto const own = rowOf(SidebarRowKind::Tab, 1, 10);
        auto const paneRow = rowOf(SidebarRowKind::Pane, 1, 10, 10);
        t.sidebar().drop(own, 0.5, pane);
        t.sidebar().drop(paneRow, 0.5, pane);
        CHECK(t.host.movedNodes.empty());
        CHECK(t.sidebar().dropIndicator(own, 0.5, pane) == std::to_underlying(DropIndicator::Refused));
        CHECK(t.sidebar().dropIndicator(paneRow, 0.5, pane) == std::to_underlying(DropIndicator::Refused));
        CHECK(t.sidebar().dropIndicator(own, 0.1, pane) == std::to_underlying(DropIndicator::LineAbove));
        CHECK(t.sidebar().dropIndicator(rowOf(SidebarRowKind::Tab, 2, 20), 0.5, pane)
              == std::to_underlying(DropIndicator::Highlight));
    }
    SECTION("a whole tab dropped on another tab's middle band merges into it")
    {
        t.sidebar().drop(rowOf(SidebarRowKind::Tab, 2, 20), 0.5, dragOf(host, DropPayload::Tab, { 10, 11 }));
        auto const& [node, destination] = moved();
        CHECK(node == SidebarNode { TabRef { .workspace = 1, .tab = 10 } });
        CHECK(std::holds_alternative<JoinTab>(destination));
    }
    SECTION("a workspace of one tab joins a tab as that tab; one of several is refused there")
    {
        t.sidebar().drop(rowOf(SidebarRowKind::Tab, 1, 12), 0.5, dragOf(host, DropPayload::Workspace, {}, 2));
        auto const& [node, destination] = moved();
        CHECK(node == SidebarNode { TabRef { .workspace = 2, .tab = 20 } });
        CHECK(std::get<JoinTab>(destination).tab == TabRef { .workspace = 1, .tab = 12 });

        t.host.movedNodes.clear();
        t.sidebar().drop(rowOf(SidebarRowKind::Tab, 2, 20), 0.5, dragOf(host, DropPayload::Workspace, {}, 1));
        CHECK(t.host.movedNodes.empty());
        CHECK(t.host.movedWorkspaces.empty());
    }
    SECTION("the new workspace area takes a pane or a tab that is not all its workspace holds")
    {
        auto const pane = dragOf(host, DropPayload::Pane, { 11 });
        CHECK(t.sidebar().takesNewWorkspaceDrop(pane));
        t.sidebar().dropOnNewWorkspace(pane);
        auto const& [node, destination] = moved();
        CHECK(node == SidebarNode { PaneKey { 11 } });
        CHECK(std::holds_alternative<IntoNewWorkspace>(destination));

        // Tab 20 is all workspace 2 holds, and a workspace is not a node.
        auto const sole = dragOf(host, DropPayload::Pane, { 20 });
        auto const workspace = dragOf(host, DropPayload::Workspace, {}, 1);
        CHECK_FALSE(t.sidebar().takesNewWorkspaceDrop(sole));
        CHECK_FALSE(t.sidebar().takesNewWorkspaceDrop(workspace));
        t.host.movedNodes.clear();
        t.sidebar().dropOnNewWorkspace(sole);
        t.sidebar().dropOnNewWorkspace(workspace);
        CHECK(t.host.movedNodes.empty());
    }
    SECTION("garbage is refused")
    {
        t.sidebar().drop(2, 0.5, "not json");
        t.sidebar().dropOnNewWorkspace("not json");
        CHECK(t.host.movedWorkspaces.empty());
        CHECK(t.host.movedNodes.empty());
        CHECK(t.sidebar().dropIndicator(2, 0.5, "not json") == std::to_underlying(DropIndicator::Refused));
    }
    SECTION("a drag from another host is refused, though its ids name a tab and a workspace here")
    {
        auto const elsewhere = std::array {
            HostIdentity { .kind = HostKind::Process, .id = "another-process" },
            HostIdentity { .kind = HostKind::Daemon, .id = "42" },
        };
        for (auto const& other: elsewhere)
        {
            t.sidebar().drop(
                rowOf(SidebarRowKind::Workspace, 2, 0), 0.5, dragOf(other, DropPayload::Pane, { 10 }));
            t.sidebar().drop(
                rowOf(SidebarRowKind::Workspace, 2, 0), 0.8, dragOf(other, DropPayload::Workspace, {}, 1));
            t.sidebar().dropOnNewWorkspace(dragOf(other, DropPayload::Pane, { 11 }));
        }
        CHECK(t.host.movedWorkspaces.empty());
        CHECK(t.host.movedNodes.empty());
    }
}

TEST_CASE("SidebarModel: a row is named by its title and described by its kind and status",
          "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.sidebar().refreshNow();

    // The name is what the row shows; the kind, the fold and the status are words of the description.
    CHECK(t.sidebar().index(0).data(SidebarModel::AccessibleNameRole).toString() == "billing");
    CHECK(t.sidebar().index(0).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Workspace, expanded, 1 pane needs you");
    CHECK(t.sidebar().index(0).data(Qt::AccessibleDescriptionRole)
          == t.sidebar().index(0).data(SidebarModel::AccessibleDescriptionRole));
    CHECK(t.sidebar().footerNeedYou() == 1);
    CHECK(t.sidebar().footerWorking() == 0);
}

TEST_CASE("SidebarModel: counts take their plural form, and zeros are left out", "[contour][sidebar]")
{
    struct Case
    {
        SidebarFooter footer;
        QString text;
    };
    for (auto const& [footer, text]:
         { Case { .footer = { .needYou = 0, .working = 0 }, .text = "" },
           Case { .footer = { .needYou = 1, .working = 0 }, .text = "1 needs you" },
           Case { .footer = { .needYou = 2, .working = 0 }, .text = "2 need you" },
           Case { .footer = { .needYou = 0, .working = 3 }, .text = "3 working" },
           Case { .footer = { .needYou = 1, .working = 2 }, .text = "1 needs you · 2 working" } })
    {
        INFO(text.toStdString());
        CHECK(footerTextOf(footer) == text);
    }

    auto row = SidebarRow {};
    row.key.kind = SidebarRowKind::Tab;
    row.leaves = { .needYou = 0, .working = 2 };
    CHECK(accessibleDescriptionOf(row) == "Tab, 2 panes working");
    row.leaves = {};
    row.urgency = Urgency::Failed;
    CHECK(accessibleDescriptionOf(row) == "Tab, failed");
}

TEST_CASE("SidebarModel: a fold that hides the displayed tab selects its workspace", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) } };
    t.host.displayed = TabRef { .workspace = 2, .tab = 20 };
    t.sidebar().refreshNow();
    auto const displayed = t.sidebar().currentIndex();
    REQUIRE(displayed == 3);
    auto const workspace = parentRowOf(t.sidebar().rows(), displayed);
    REQUIRE(workspace == 2);

    t.sidebar().collapse(workspace);
    t.sidebar().refreshNow();
    CHECK(t.sidebar().currentIndex() == workspace);
    CHECK(t.sidebar().index(workspace).data(SidebarModel::SelectionRole).toInt()
          == static_cast<int>(RowSelection::ContainsSelected));
    CHECK(t.sidebar().index(workspace).data(SidebarModel::SelectedRole).toBool() == false);
    CHECK(t.sidebar().index(workspace).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Workspace, collapsed, holds the current tab");

    t.sidebar().expand(workspace);
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(workspace).data(SidebarModel::SelectionRole).toInt()
          == static_cast<int>(RowSelection::None));
}

TEST_CASE("SidebarModel: the filter narrows the rows and leaves the footer", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) } };
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
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
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
                auto workspace = directoryWorkspaceOf(
                    id, std::format("ws{}", id), { directoryPaneOf(nextId++, std::format("p{}", nextId)) });
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
                                      directoryPaneOf(id, std::format("p{}", id)));
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

TEST_CASE("SidebarModel: the Collapse and Expand keys fold a row or move to its parent or child",
          "[contour][sidebar]")
{
    auto t =
        SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10), directoryPaneOf(11) }),
                             directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) } };
    REQUIRE(t.sidebar().rowCount() == 5);

    // On a tab: to its workspace, which then folds; folded, it unfolds; unfolded, the focus goes into it.
    CHECK(t.sidebar().collapseOrParent(2) == 0);
    CHECK(t.sidebar().collapseOrParent(0) == 0);
    CHECK(t.sidebar().rowCount() == 3);
    CHECK(t.sidebar().expandOrChild(0) == 0);
    CHECK(t.sidebar().rowCount() == 5);
    CHECK(t.sidebar().expandOrChild(0) == 1);
    // A row with nothing to fold stays.
    CHECK(t.sidebar().expandOrChild(1) == 1);
}

TEST_CASE("SidebarModel: the summary follows the shown workspace", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) } };
    CHECK(t.sidebar().currentWorkspaceTitle() == "billing");
    CHECK(t.sidebar().currentWorkspaceIndex() == 0);
    CHECK_FALSE(t.sidebar().workspaceChipMarked());

    auto summary = QSignalSpy { &t.sidebar(), &SidebarModel::summaryChanged };
    t.source.listed.selected = 2;
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.sidebar().refreshNow();
    CHECK(summary.count() == 1);
    CHECK(t.sidebar().currentWorkspaceTitle() == "docs");
    CHECK(t.sidebar().currentWorkspaceIndex() == 2);
    CHECK(t.sidebar().otherWorkspacesUrgency() == static_cast<int>(Urgency::NeedsYou));
    CHECK(t.sidebar().workspaceChipMarked());
    CHECK(t.sidebar().workspaceChipGlyph() != static_cast<int>(TabBadgeKind::None));

    // A filter that hides billing does not hide what it asks.
    t.sidebar().setFilter("docs");
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(0).data(SidebarModel::TitleRole).toString() == "docs");
    CHECK(t.sidebar().workspaceChipMarked());
}

TEST_CASE("SidebarModel: the hover card's reason is the whole one, the row's is cut", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    auto record = recordOf(ProgramState::Working);
    record.message = std::string(MaxRowTextLength * 3, 'r');
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { record };
    t.sidebar().refreshNow();

    CHECK(t.sidebar().roleNames().value(SidebarModel::FullReasonRole) == "fullReason");
    auto const tab = t.sidebar().index(1);
    CHECK(tab.data(SidebarModel::ReasonRole).toString().size() == static_cast<qsizetype>(MaxRowTextLength));
    CHECK(tab.data(SidebarModel::FullReasonRole).toString() == QString::fromStdString(record.message));
}

TEST_CASE("SidebarModel: the reason is delivered as its words and its command", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    auto record = recordOf(ProgramState::Blocked);
    record.kind = vtbackend::BlockedKind::Permission;
    record.message = "Bash: psql -h db";
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { record };
    t.sidebar().refreshNow();

    CHECK(t.sidebar().roleNames().value(SidebarModel::ReasonLabelRole) == "reasonLabel");
    CHECK(t.sidebar().roleNames().value(SidebarModel::ReasonCommandRole) == "reasonCommand");
    auto const tab = t.sidebar().index(1);
    CHECK(tab.data(SidebarModel::ReasonLabelRole).toString() == "Permission · Bash: ");
    CHECK(tab.data(SidebarModel::ReasonCommandRole).toString() == "psql -h db");
    CHECK(tab.data(SidebarModel::ReasonRole).toString() == "Permission · Bash: psql -h db");
    CHECK(tab.data(SidebarModel::FullReasonCommandRole).toString() == "psql -h db");

    // A plain reason has no command.
    auto done = recordOf(ProgramState::Done);
    done.message = "All tests pass";
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { done };
    t.sidebar().refreshNow();
    auto const plain = t.sidebar().index(1);
    CHECK(plain.data(SidebarModel::ReasonLabelRole).toString() == "All tests pass");
    CHECK(plain.data(SidebarModel::ReasonCommandRole).toString().isEmpty());

    // The sidebar always draws a command, so a failed command's separator is drawn with its words.
    t.source.listed.workspaces[0].tabs[0].panes[0].status = {};
    t.source.listed.workspaces[0].tabs[0].panes[0].finished =
        vthost::proto::FinishedCommand { .exitCode = 1, .durationMs = std::nullopt, .commandLine = "make" };
    t.source.listed.workspaces[0].tabs[0].panes[0].finishedSerial = 1;
    t.sidebar().refreshNow();
    auto const failed = t.sidebar().index(1);
    CHECK(failed.data(SidebarModel::ReasonLabelRole).toString() == "exit 1 · ");
    CHECK(failed.data(SidebarModel::ReasonCommandRole).toString() == "make");
}

TEST_CASE("SidebarModel: the command map names every tree command", "[contour][sidebar]")
{
    auto const commands = sidebarCommandMap();
    CHECK(commands.size() == static_cast<qsizetype>(SidebarCommandNames.size()));
    CHECK(commands.value("rename").toInt() == static_cast<int>(ChromeCommand::Rename));
    CHECK(commands.value("filter").toInt() == static_cast<int>(ChromeCommand::Filter));
}

TEST_CASE("the footer's counts outrank its chord, which is dropped first", "[contour][sidebar][footer]")
{
    using contour::window::sidebarrows::FooterChord;
    struct Case
    {
        FooterRoom room {};
        FooterChord expected = FooterChord::Dropped;
    };
    for (auto const& [room, expected]: {
             // Room for both, with the gap between them.
             Case { .room = { .available = 200, .countsWidth = 100, .chordWidth = 60, .gap = 8 },
                    .expected = FooterChord::Shown },
             // Exactly enough.
             Case { .room = { .available = 168, .countsWidth = 100, .chordWidth = 60, .gap = 8 },
                    .expected = FooterChord::Shown },
             // One pixel short: the counts keep their words, the chord goes.
             Case { .room = { .available = 167, .countsWidth = 100, .chordWidth = 60, .gap = 8 },
                    .expected = FooterChord::Dropped },
             // The counts alone fill it: the chord is dropped, the counts elide only as the last resort.
             Case { .room = { .available = 100, .countsWidth = 120, .chordWidth = 60, .gap = 8 },
                    .expected = FooterChord::Dropped },
             // No counts: the chord needs no gap.
             Case { .room = { .available = 60, .countsWidth = 0, .chordWidth = 60, .gap = 8 },
                    .expected = FooterChord::Shown },
             Case { .room = { .available = 59, .countsWidth = 0, .chordWidth = 60, .gap = 8 },
                    .expected = FooterChord::Dropped },
             // No chord bound: nothing to show.
             Case { .room = { .available = 500, .countsWidth = 100, .chordWidth = 0, .gap = 8 },
                    .expected = FooterChord::Dropped },
         })
    {
        INFO("available " << room.available << ", counts " << room.countsWidth << ", chord "
                          << room.chordWidth << ", gap " << room.gap);
        CHECK(footerChordFor(room) == expected);
    }
}

TEST_CASE("the footer's counts take their shorter wording only when the full one does not fit",
          "[contour][sidebar][footer]")
{
    using contour::window::sidebarrows::FooterCounts;
    CHECK(footerCountsFor(120, 100) == FooterCounts::Full);
    CHECK(footerCountsFor(100, 100) == FooterCounts::Full);
    CHECK(footerCountsFor(99, 100) == FooterCounts::Compact);

    auto footer = SidebarFooter { .needYou = 0, .working = 0 };
    CHECK(footerCompactTextOf(footer).isEmpty());
    footer = { .needYou = 1, .working = 1 };
    CHECK(footerCompactTextOf(footer) == "1 waiting · 1 busy");
    footer = { .needYou = 3, .working = 0 };
    CHECK(footerCompactTextOf(footer) == "3 waiting");
}

TEST_CASE("SidebarModel: the rows' times run while the sidebar or the switcher over the window shows them",
          "[contour][sidebar]")
{
    using contour::config::SidebarVisibility;
    // The chip's dropdown with the sidebar hidden shows the same running times: they must not stand still.
    CHECK(sidebarPresenceOf(SidebarVisibility::Hidden, WindowExposure::Exposed, SwitcherOverWindow::Yes)
          == SidebarPresence::Shown);
    CHECK(sidebarPresenceOf(SidebarVisibility::Hidden, WindowExposure::Exposed, SwitcherOverWindow::No)
          == SidebarPresence::Hidden);
    CHECK(sidebarPresenceOf(SidebarVisibility::Shown, WindowExposure::Exposed, SwitcherOverWindow::No)
          == SidebarPresence::Shown);
    CHECK(sidebarPresenceOf(SidebarVisibility::Shown, WindowExposure::Hidden, SwitcherOverWindow::Yes)
          == SidebarPresence::Hidden);
}
