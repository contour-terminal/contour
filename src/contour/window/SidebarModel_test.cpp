// SPDX-License-Identifier: Apache-2.0
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/test/SidebarFakes.hpp>
#include <contour/window/ChromeKeys.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/SidebarDropPolicy.hpp>
#include <contour/window/SidebarModel.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QColor>

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

/// A model over a fake window, source, daemons, clock and announcer.
struct SidebarUnderTest
{
    FakeSource source;
    FakeHost host;
    contour::test::FakeDaemonSections sections;
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
        model.emplace(host, sections, attention, clock, announcer);
    }

    /// A window on @p own, its workspaces @p workspaces, the presence listing @p others besides it.
    SidebarUnderTest(OwnDaemon own,
                     std::vector<DirectoryWorkspace> workspaces,
                     std::vector<DaemonSection> others)
    {
        source.listed.workspaces = std::move(workspaces);
        if (!source.listed.workspaces.empty())
            source.listed.selected = source.listed.workspaces.front().id;
        host.source = &source;
        host.own = std::move(own);
        sections.listed = std::move(others);
        model.emplace(host, sections, attention, clock, announcer);
    }

    SidebarModel& sidebar() { return *model; }
};

DaemonRef const Home = localDaemonRef("default");
DaemonRef const Starfive = hostDaemonRef("starfive");
constexpr auto HomeRun = DaemonInstance { 1 };
constexpr auto StarfiveRun = DaemonInstance { 9 };

/// The window's own daemon, connected.
OwnDaemon ownHome()
{
    return OwnDaemon { .daemon = Home,
                       .instance = HomeRun,
                       .name = "default",
                       .locality = DaemonLocality::Local,
                       .state = DaemonLinkState::Connected };
}

/// A section the presence lists.
DaemonSection sectionOf(DaemonRef daemon,
                        DaemonInstance instance,
                        std::string name,
                        DaemonLinkState state,
                        std::vector<DirectoryWorkspace> workspaces,
                        DaemonLocality locality = DaemonLocality::Remote)
{
    auto section = DaemonSection {};
    section.daemon = std::move(daemon);
    section.instance = instance;
    section.name = std::move(name);
    section.state = state;
    section.locality = locality;
    section.directory.workspaces = std::move(workspaces);
    section.feed = SectionFeed::Observer;
    return section;
}

/// Starfive in @p state, with its workspace 5 holding tab 50 (`build`).
DaemonSection starfiveIn(DaemonLinkState state)
{
    return sectionOf(Starfive,
                     StarfiveRun,
                     "starfive",
                     state,
                     { directoryWorkspaceOf(5, "ci", { directoryPaneOf(50, "build") }) });
}

/// The key of a row of @p daemon's.
SidebarRowKey keyOf(SidebarRowKind kind,
                    DaemonRef daemon,
                    DaemonInstance instance,
                    uint64_t workspace = 0,
                    uint64_t tab = 0,
                    PaneKey pane = {})
{
    return SidebarRowKey { .kind = kind,
                           .daemon = std::move(daemon),
                           .instance = instance,
                           .workspace = workspace,
                           .tab = tab,
                           .pane = pane };
}

/// @return How many rows of @p kind the model lists.
long rowsOfKind(SidebarModel const& model, SidebarRowKind kind)
{
    return std::ranges::count(model.rows(), kind, [](SidebarRow const& row) { return row.key.kind; });
}

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

    t.source.listed.workspaces[1].name = "documentation";
    t.sidebar().noteChanged();
    letEventLoopRun();

    CHECK(t.sidebar().rowCount() == 3);
    CHECK(t.sidebar().currentIndex() == 2);
    CHECK(t.sidebar().index(0).data(SidebarModel::ExpansionRole).toInt()
          == static_cast<int>(RowExpansion::Collapsed));
    CHECK(t.sidebar().index(1).data(SidebarModel::TitleRole).toString() == "documentation");
}

TEST_CASE("SidebarModel: a tab the window holds shows its icon and hover lines", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(
        1, "billing", { directoryPaneOf(10), directoryPaneOf(11) }) } };
    t.host.icons[10] = TabIcon { .kind = TabIconKind::Monogram, .initial = "V", .image = {} };
    auto const held = SidebarRowKey { .kind = SidebarRowKind::Tab, .workspace = 1, .tab = 10, .pane = {} };
    t.host.lines[held] = { "Running: vim" };
    t.sidebar().refreshNow();
    REQUIRE(t.sidebar().rowCount() == 3); // billing, its two tabs

    auto const icon = t.sidebar().index(1).data(SidebarModel::IconRole).toMap();
    CHECK(icon.value(QStringLiteral("kind")).toInt() == static_cast<int>(TabIconKind::Monogram));
    CHECK(icon.value(QStringLiteral("text")).toString() == QStringLiteral("V"));
    CHECK(t.sidebar().hoverLines(1) == QStringList { QStringLiteral("Running: vim") });

    // A tab the window does not hold, and the workspace row, have neither.
    CHECK(t.sidebar().index(2).data(SidebarModel::IconRole).toMap().isEmpty());
    CHECK(t.sidebar().hoverLines(2).isEmpty());
    CHECK(t.sidebar().index(0).data(SidebarModel::IconRole).toMap().isEmpty());
    CHECK(t.sidebar().hoverLines(0).isEmpty());

    // A program starting in the other tab changes its row's icon, and only that role.
    auto changed = QList<int> {};
    QObject::connect(&t.sidebar(),
                     &QAbstractItemModel::dataChanged,
                     [&](QModelIndex const& from, QModelIndex const&, QList<int> const& roles) {
                         if (from.row() == 2)
                             changed = roles;
                     });
    t.host.icons[11] = TabIcon { .kind = TabIconKind::Monogram, .initial = "M", .image = {} };
    t.sidebar().noteChanged();
    letEventLoopRun();
    CHECK(changed == QList<int> { SidebarModel::IconRole });
    CHECK(t.sidebar().index(2).data(SidebarModel::IconRole).toMap().value(QStringLiteral("text")).toString()
          == QStringLiteral("M"));
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

    t.source.listed.workspaces[0].name = "invoices";
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

TEST_CASE("SidebarModel: a colour changes neither a row's accessible name nor its description",
          "[contour][sidebar]")
{
    auto const coloured = [](SidebarRow row) {
        row.tabColor = vtbackend::RGBColor { 0x29, 0x80, 0xB9 };
        row.workspaceColor = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
        return row;
    };
    auto workspace = SidebarRow {};
    workspace.key.kind = SidebarRowKind::Workspace;
    workspace.title = "contour";
    workspace.expansion = RowExpansion::Expanded;
    auto tab = SidebarRow {};
    tab.key.kind = SidebarRowKind::Tab;
    tab.depth = 1;
    tab.title = "claude";
    tab.urgency = Urgency::Failed;
    for (auto const& row: { workspace, tab })
    {
        CAPTURE(row.title);
        CHECK(accessibleNameOf(coloured(row)) == accessibleNameOf(row));
        CHECK(accessibleNameOf(coloured(row)) == QString::fromStdString(row.title));
        CHECK(accessibleDescriptionOf(coloured(row)) == accessibleDescriptionOf(row));
    }
    CHECK(accessibleDescriptionOf(coloured(workspace)) == "Workspace, expanded");
    CHECK(accessibleDescriptionOf(coloured(tab)) == "Tab, failed");
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

TEST_CASE("Needs me narrows and restores", "[contour][sidebar]")
{
    auto t =
        SidebarUnderTest { ownHome(),
                           { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10), directoryPaneOf(11) }),
                             directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }),
                             directoryWorkspaceOf(3, "notes", { directoryPaneOf(30) }) },
                           { starfiveIn(DaemonLinkState::Connected) } };
    t.source.listed.workspaces[1].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.host.displayed = TabRef { .workspace = 1, .tab = 10 };
    // A fold of the user's that the way back must keep.
    t.sidebar().refreshNow();
    auto const notes = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Home, HomeRun, 3));
    REQUIRE(notes >= 0);
    t.sidebar().collapse(notes);
    t.sidebar().refreshNow();
    auto const all = t.sidebar().rows();
    CHECK(t.sidebar().emptyState() == SidebarEmptyState::None);

    auto scopeChanges = QSignalSpy { &t.sidebar(), &SidebarModel::scopeChanged };
    t.sidebar().setScope(contour::config::SidebarScope::NeedsMe);
    CHECK(scopeChanges.count() == 1);
    CHECK(t.sidebar().scopeValue() == static_cast<int>(contour::config::SidebarScope::NeedsMe));
    // The daemon, the path to the blocked pane and the displayed tab's path; nothing else.
    auto const keys = [&] {
        auto listed = std::vector<SidebarRowKey> {};
        for (auto const& row: t.sidebar().rows())
            listed.push_back(row.key);
        return listed;
    };
    CHECK(keys()
          == std::vector<SidebarRowKey> { keyOf(SidebarRowKind::Daemon, Home, HomeRun),
                                          keyOf(SidebarRowKind::Workspace, Home, HomeRun, 1),
                                          keyOf(SidebarRowKind::Tab, Home, HomeRun, 1, 10),
                                          keyOf(SidebarRowKind::Workspace, Home, HomeRun, 2),
                                          keyOf(SidebarRowKind::Tab, Home, HomeRun, 2, 20) });
    CHECK(t.sidebar().emptyState() == SidebarEmptyState::None);

    // Once nothing needs the user, the tree says so, the displayed tab's path still listed above it.
    auto emptyChanges = QSignalSpy { &t.sidebar(), &SidebarModel::emptyStateChanged };
    t.source.listed.workspaces[1].tabs[0].panes[0].status = { recordOf(ProgramState::Idle) };
    t.sidebar().refreshNow();
    CHECK(t.sidebar().emptyState() == SidebarEmptyState::NothingNeedsYou);
    CHECK(emptyChanges.count() == 1);
    CHECK(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Tab, Home, HomeRun, 1, 10)) >= 0);

    // The way back lists what was there, folds and all.
    t.source.listed.workspaces[1].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.sidebar().setScope(static_cast<int>(contour::config::SidebarScope::All));
    CHECK(t.sidebar().rows() == all);
    CHECK(t.sidebar().emptyState() == SidebarEmptyState::None);
    CHECK(scopeChanges.count() == 2);
    // A value QML cannot mean is every row.
    t.sidebar().setScope(static_cast<int>(contour::config::SidebarScopeCount));
    CHECK(t.sidebar().scope() == contour::config::SidebarScope::All);
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
                        auto const workspace = WorkspaceKey { .daemon = shown.key.daemon,
                                                              .instance = shown.key.instance,
                                                              .workspace = shown.key.workspace };
                        auto const tab = TabKey { .workspace = workspace, .tab = shown.key.tab };
                        // The window's one daemonless section starts unfolded: a fold is a toggle.
                        if (shown.key.kind == SidebarRowKind::Workspace)
                        {
                            if (collapsed)
                                expectedState.toggled.erase(workspace);
                            else
                                expectedState.toggled.insert(workspace);
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

TEST_CASE("SidebarModel: an idle tab row's second line is its folder", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.source.listed.workspaces[0].tabs[0].panes[0].cwd = "/srv/app";
    t.sidebar().refreshNow();

    CHECK(t.sidebar().roleNames().value(SidebarModel::SecondLineRole) == "secondLine");
    CHECK(t.sidebar().roleNames().value(SidebarModel::IdleLineRole) == "idleLine");
    auto const tab = t.sidebar().index(1);
    CHECK(tab.data(SidebarModel::SecondLineRole).toInt() == static_cast<int>(sidebarrows::SecondLine::Idle));
    CHECK(tab.data(SidebarModel::IdleLineRole).toString() == "/srv/app");
    auto const workspace = t.sidebar().index(0);
    CHECK(workspace.data(SidebarModel::SecondLineRole).toInt()
          == static_cast<int>(sidebarrows::SecondLine::None));
    CHECK(workspace.data(SidebarModel::IdleLineRole).toString().isEmpty());

    // A tab that knows neither its folder nor its program has nothing for its second line.
    t.source.listed.workspaces[0].tabs[0].panes[0].cwd.clear();
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(1).data(SidebarModel::SecondLineRole).toInt()
          == static_cast<int>(sidebarrows::SecondLine::None));
}

TEST_CASE("a reason arriving changes the second line's role", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.sidebar().refreshNow();
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };

    auto record = recordOf(ProgramState::Done);
    record.message = "All tests pass";
    t.source.listed.workspaces[0].tabs[0].panes[0].status = { record };
    t.sidebar().refreshNow();

    auto const named = std::ranges::any_of(changed, [](QList<QVariant> const& signal) {
        return signal.at(2).value<QList<int>>().contains(SidebarModel::SecondLineRole);
    });
    CHECK(named);
    CHECK(t.sidebar().index(1).data(SidebarModel::SecondLineRole).toInt()
          == static_cast<int>(sidebarrows::SecondLine::Status));
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
    CHECK(footerCountsFor(120, 100, 80) == FooterCounts::Full);
    CHECK(footerCountsFor(100, 100, 80) == FooterCounts::Full);
    CHECK(footerCountsFor(99, 100, 80) == FooterCounts::Compact);
    CHECK(footerCountsFor(80, 100, 80) == FooterCounts::Compact);
    // Where no words fit, the rows' status glyphs say the counts.
    CHECK(footerCountsFor(79, 100, 80) == FooterCounts::Glyphs);

    auto footer = SidebarFooter { .needYou = 0, .working = 0 };
    CHECK(footerCompactTextOf(footer).isEmpty());
    footer = { .needYou = 1, .working = 1 };
    CHECK(footerCompactTextOf(footer) == "1 waiting · 1 busy");
    footer = { .needYou = 3, .working = 0 };
    CHECK(footerCompactTextOf(footer) == "3 waiting");
}

TEST_CASE("the footer's glyph counts are the status glyphs its rows draw", "[contour][sidebar][footer]")
{
    using contour::config::SidebarLook;
    // Every look, the last of them the enum's.
    for (auto const look:
         std::views::iota(0, std::to_underlying(SidebarLook::CellTree) + 1)
             | std::views::transform([](int value) { return static_cast<SidebarLook>(value); }))
    {
        INFO("look " << static_cast<int>(look));
        auto const blocked = std::string { tabBadgeGlyph(TabBadgeKind::Blocked, look) };
        auto const working = std::string { tabBadgeGlyph(TabBadgeKind::Working, look) };
        CHECK(sidebarFooterGlyphText({ .needYou = 0, .working = 0 }, look).empty());
        CHECK(sidebarFooterGlyphText({ .needYou = 1, .working = 2 }, look)
              == std::format("{} 1 · {} 2", blocked, working));
        CHECK(sidebarFooterGlyphText({ .needYou = 0, .working = 12 }, look) == std::format("{} 12", working));
        CHECK(sidebarFooterGlyphText({ .needYou = 3, .working = 0 }, look) == std::format("{} 3", blocked));
    }
    CHECK(sidebarFooterGlyphText({ .needYou = 1, .working = 2 }, SidebarLook::CellTree) == "! 1 · ◑ 2");
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

namespace
{
constexpr auto Orange = vtbackend::RGBColor { 0xE8, 0x85, 0x3A };

/// A workspace with one tab split in two panes, coloured @ref Orange: rows workspace, tab, pane, pane.
std::vector<DirectoryWorkspace> oneColouredSplitTab()
{
    auto workspace = directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) });
    workspace.tabs[0].panes.push_back(directoryPaneOf(11));
    workspace.tabs[0].color = Orange;
    return { std::move(workspace) };
}
} // namespace

TEST_CASE("SidebarModel: a tab's rows serve its colour, a workspace row none", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { oneColouredSplitTab() };
    REQUIRE(t.sidebar().rowCount() == 4);
    CHECK(t.sidebar().roleNames().value(SidebarModel::TabColorRole) == "tabColor");

    auto const orange = QColor { 0xE8, 0x85, 0x3A };
    for (auto const row: { 1, 2, 3 })
    {
        INFO("row " << row);
        CHECK(t.sidebar().index(row).data(SidebarModel::TabColorRole).value<QColor>() == orange);
    }
    CHECK(t.sidebar().index(0).data(SidebarModel::TabColorRole).value<QColor>().alpha() == 0);

    // Uncoloured, the tab's rows are transparent, the convention the tab strip's accentColor keeps.
    t.source.listed.workspaces[0].tabs[0].color.reset();
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(1).data(SidebarModel::TabColorRole).value<QColor>().alpha() == 0);
}

TEST_CASE("a tab colour change is reported for the row", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { oneColouredSplitTab() };
    t.sidebar().refreshNow();
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };

    t.source.listed.workspaces[0].tabs[0].color.reset();
    t.sidebar().refreshNow();

    auto const named = [&](int row) {
        return std::ranges::any_of(changed, [row](QList<QVariant> const& signal) {
            auto const first = signal.at(0).value<QModelIndex>().row();
            auto const last = signal.at(1).value<QModelIndex>().row();
            return first <= row && row <= last
                   && signal.at(2).value<QList<int>>().contains(SidebarModel::TabColorRole);
        });
    };
    CHECK_FALSE(named(0));
    CHECK(named(1));
    CHECK(named(2));
    CHECK(named(3));
}

TEST_CASE("SidebarModel: every row of a workspace serves its colour", "[contour][sidebar]")
{
    auto directory = oneColouredSplitTab();
    directory[0].color = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    auto t = SidebarUnderTest { std::move(directory) };
    REQUIRE(t.sidebar().rowCount() == 4);
    CHECK(t.sidebar().roleNames().value(SidebarModel::WorkspaceColorRole) == "workspaceColor");

    auto const magenta = QColor { 0xC2, 0x54, 0x8F };
    for (auto const row: { 0, 1, 2, 3 })
    {
        INFO("row " << row);
        CHECK(t.sidebar().index(row).data(SidebarModel::WorkspaceColorRole).value<QColor>() == magenta);
    }

    // Uncoloured, every row is transparent.
    t.source.listed.workspaces[0].color.reset();
    t.sidebar().refreshNow();
    for (auto const row: { 0, 1, 2, 3 })
    {
        INFO("row " << row);
        CHECK(t.sidebar().index(row).data(SidebarModel::WorkspaceColorRole).value<QColor>().alpha() == 0);
    }
}

TEST_CASE("a workspace colour change is reported for all of its rows", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { oneColouredSplitTab() };
    t.sidebar().refreshNow();
    auto changed = QSignalSpy { &t.sidebar(), &QAbstractItemModel::dataChanged };

    t.source.listed.workspaces[0].color = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    t.sidebar().refreshNow();

    auto const named = [&](int row) {
        return std::ranges::any_of(changed, [row](QList<QVariant> const& signal) {
            auto const first = signal.at(0).value<QModelIndex>().row();
            auto const last = signal.at(1).value<QModelIndex>().row();
            return first <= row && row <= last
                   && signal.at(2).value<QList<int>>().contains(SidebarModel::WorkspaceColorRole);
        });
    };
    for (auto const row: { 0, 1, 2, 3 })
    {
        INFO("row " << row);
        CHECK(named(row));
    }
}

TEST_CASE("SidebarModel: every row serves the cells of its workspace's rail", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { oneColouredSplitTab() };
    REQUIRE(t.sidebar().rowCount() == 4);
    CHECK(t.sidebar().roleNames().value(SidebarModel::RailTextRole) == "railText");
    CHECK(t.sidebar().roleNames().value(SidebarModel::RailTextContinuedRole) == "railTextContinued");
    for (auto const row: { 0, 1, 2, 3 })
    {
        INFO("row " << row);
        auto const cells = workspaceRailCellsOf(t.sidebar().rows()[static_cast<std::size_t>(row)]);
        auto const index = t.sidebar().index(row);
        CHECK(index.data(SidebarModel::RailTextRole).toString() == QString::fromStdString(cells.first));
        CHECK(index.data(SidebarModel::RailTextContinuedRole).toString()
              == QString::fromStdString(cells.second));
    }
    // The split tab is the workspace's last: its panes still carry the rail.
    CHECK(t.sidebar().index(2).data(SidebarModel::RailTextRole).toString() == QStringLiteral("│"));
}

TEST_CASE("SidebarModel: activating another daemon's tab asks the host to jump there", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    // Another daemon's workspaces start folded: the user unfolds Starfive's.
    t.sidebar().expand(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Starfive, StarfiveRun, 5)));
    auto const tab = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Tab, Starfive, StarfiveRun, 5, 50));
    REQUIRE(tab >= 0);

    // The keyboard stays where the jump was made from.
    CHECK(t.sidebar().activateRow(tab) == sidebarrows::RowActivation::Nothing);
    REQUIRE(t.host.jumps.size() == 1);
    CHECK(t.host.jumps[0].first
          == SidebarTarget {
              .daemon = Starfive, .instance = StarfiveRun, .workspace = 5, .tab = 50, .pane = std::nullopt });
    CHECK(t.host.jumps[0].second == JumpMode::Here);
    CHECK(t.host.shown.empty());
    CHECK(t.host.shownWorkspaces.empty());

    // The window's own verbs do not reach another daemon's rows.
    t.sidebar().rename(tab, QStringLiteral("renamed"));
    t.sidebar().markSeen(tab);
    CHECK(t.host.renamedTabs.empty());
    CHECK(t.host.seen.empty());
}

TEST_CASE("SidebarModel: activating a connected daemon row folds it and keeps the keyboard",
          "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    REQUIRE(daemon >= 0);
    auto const before = t.sidebar().rowCount();
    REQUIRE(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Starfive, StarfiveRun, 5)) >= 0);

    CHECK(t.sidebar().activateRow(daemon) == sidebarrows::RowActivation::Nothing);
    CHECK(t.sidebar().rowCount() == before - 1);
    CHECK(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Starfive, StarfiveRun, 5)) == -1);
    CHECK(t.sidebar().index(daemon).data(SidebarModel::ExpansionRole).toInt()
          == static_cast<int>(RowExpansion::Collapsed));
    CHECK(t.sections.calls.empty());
    CHECK(t.host.jumps.empty());

    // And unfolds it again.
    CHECK(t.sidebar().activateRow(daemon) == sidebarrows::RowActivation::Nothing);
    CHECK(t.sidebar().rowCount() == before);
}

TEST_CASE("SidebarModel: activating a daemon that needs you connects it from this window",
          "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::NeedsInput) } };
    t.host.window = vtworkspace::WorkspaceId { 7 };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    REQUIRE(daemon >= 0);

    CHECK(t.sidebar().activateRow(daemon) == sidebarrows::RowActivation::Nothing);
    using Call = contour::test::DaemonSectionsCall;
    CHECK(
        t.sections.calls
        == std::vector { Call {
            .verb = Call::Connect { .daemon = Starfive, .askingWindow = vtworkspace::WorkspaceId { 7 } } } });
    // Its buttons say the same.
    CHECK(t.sidebar().index(daemon).data(SidebarModel::DaemonButtonsRole).toList()
          == QVariantList { static_cast<int>(sidebarrows::DaemonRowButton::Connect),
                            static_cast<int>(sidebarrows::DaemonRowButton::None) });
}

TEST_CASE("SidebarModel: activating a stopped daemon starts it", "[contour][sidebar]")
{
    auto const work = localDaemonRef("work");
    auto t = SidebarUnderTest {
        ownHome(),
        { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
        { sectionOf(
            work, DaemonInstance { 3 }, "work", DaemonLinkState::NotRunning, {}, DaemonLocality::Local) }
    };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, work, DaemonInstance { 3 }));
    REQUIRE(daemon >= 0);

    CHECK(t.sidebar().activateRow(daemon) == sidebarrows::RowActivation::Nothing);
    using Call = contour::test::DaemonSectionsCall;
    CHECK(t.sections.calls == std::vector { Call { .verb = Call::Start { .daemon = work } } });

    // Its Remove button unlists it; Cancel has nothing to end.
    t.sidebar().removeDaemon(daemon);
    t.sidebar().cancelPending(daemon);
    CHECK(t.sections.calls
          == std::vector { Call { .verb = Call::Start { .daemon = work } },
                           Call { .verb = Call::Remove { .daemon = work } } });
}

TEST_CASE("SidebarModel: own daemon listed once", "[contour][sidebar]")
{
    // The presence lists the window's own daemon too, as its observer last saw it.
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { sectionOf(Home,
                                            HomeRun,
                                            "default",
                                            DaemonLinkState::Reconnecting,
                                            { directoryWorkspaceOf(99, "stale", {}) }),
                                  starfiveIn(DaemonLinkState::Connected) } };

    CHECK(rowsOfKind(t.sidebar(), SidebarRowKind::Daemon) == 2);
    auto const home = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Home, HomeRun));
    REQUIRE(home == 0);
    // Its rows are the window's own, from the window's own link: live, not the presence's last word.
    CHECK(t.sidebar().index(home).data(SidebarModel::LinkStateRole).toInt()
          == static_cast<int>(DaemonLinkState::Connected));
    CHECK(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Home, HomeRun, 1)) >= 0);
    CHECK(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Home, HomeRun, 99)) == -1);
}

TEST_CASE("SidebarModel: activateFirstMatch goes to the first matching tab; NewWindow opens it beside",
          "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10, "shell") }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    auto const starfiveTab = SidebarTarget {
        .daemon = Starfive, .instance = StarfiveRun, .workspace = 5, .tab = 50, .pane = std::nullopt
    };

    t.sidebar().setFilter(QStringLiteral("build"));
    t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::Here));
    t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::NewWindow));
    CHECK(t.host.jumps
          == std::vector { std::pair { starfiveTab, JumpMode::Here },
                           std::pair { starfiveTab, JumpMode::NewWindow } });

    // The window's own tab is shown here, and opened beside as a jump.
    t.sidebar().setFilter(QStringLiteral("shell"));
    t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::Here));
    CHECK(t.host.shown == std::vector { PaneKey { 10 } });
    t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::NewWindow));
    REQUIRE(t.host.jumps.size() == 3);
    CHECK(t.host.jumps[2]
          == std::pair {
              SidebarTarget {
                  .daemon = Home, .instance = HomeRun, .workspace = 1, .tab = 10, .pane = std::nullopt },
              JumpMode::NewWindow });

    // A filter that leaves only workspace rows goes to the first of them.
    t.sidebar().setFilter(QStringLiteral("ci"));
    t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::Here));
    REQUIRE(t.host.jumps.size() == 4);
    CHECK(t.host.jumps[3].first
          == SidebarTarget { .daemon = Starfive,
                             .instance = StarfiveRun,
                             .workspace = 5,
                             .tab = std::nullopt,
                             .pane = std::nullopt });
}

TEST_CASE("SidebarModel: a presence change rebuilds once", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    auto const reads = t.sections.reads;
    t.sections.listed = { starfiveIn(DaemonLinkState::Reconnecting) };
    t.sections.changed();
    t.sections.changed();
    t.sections.changed();
    CHECK(t.sidebar().isRebuildPending());
    letEventLoopRun();

    CHECK(t.sections.reads == reads + 1);
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    CHECK(t.sidebar().index(daemon).data(SidebarModel::LinkStateRole).toInt()
          == static_cast<int>(DaemonLinkState::Reconnecting));
}

TEST_CASE("SidebarModel: after a jump the selected row is reported for scrolling into view",
          "[contour][sidebar]")
{
    auto t = SidebarUnderTest {
        ownHome(),
        { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
        // The presence lists the window's own daemon too, in tree order.
        { sectionOf(Home, HomeRun, "default", DaemonLinkState::Connected, {}, DaemonLocality::Local),
          starfiveIn(DaemonLinkState::Connected) }
    };
    t.host.displayed = TabRef { .workspace = 1, .tab = 10 };
    t.sidebar().refreshNow();
    t.sidebar().expand(t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Starfive, StarfiveRun, 5)));
    REQUIRE(t.sidebar().currentIndex()
            == t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Tab, Home, HomeRun, 1, 10)));
    auto moved = QSignalSpy { &t.sidebar(), &SidebarModel::currentIndexChanged };

    // The jump lands: the window is on Starfive now, showing its tab; its old daemon is observed.
    t.host.own = OwnDaemon { .daemon = Starfive,
                             .instance = StarfiveRun,
                             .name = "starfive",
                             .locality = DaemonLocality::Remote,
                             .state = DaemonLinkState::Connected };
    t.source.listed.workspaces = { directoryWorkspaceOf(5, "ci", { directoryPaneOf(50, "build") }) };
    t.source.listed.selected = 5;
    t.host.displayed = TabRef { .workspace = 5, .tab = 50 };
    t.sections.listed = { sectionOf(Home,
                                    HomeRun,
                                    "default",
                                    DaemonLinkState::Connected,
                                    { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                    DaemonLocality::Local),
                          starfiveIn(DaemonLinkState::Connected) };
    t.sections.changed();
    letEventLoopRun();

    CHECK(moved.count() == 1);
    CHECK(t.sidebar().currentIndex()
          == t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Tab, Starfive, StarfiveRun, 5, 50)));
    CHECK(t.sidebar().currentIndex() >= 0);
}

TEST_CASE("SidebarModel: a selection that moves to another row at the same index is reported",
          "[contour][sidebar]")
{
    auto t =
        SidebarUnderTest { ownHome(), { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) }, {} };
    t.host.displayed = TabRef { .workspace = 1, .tab = 10 };
    t.sidebar().refreshNow();
    auto const before = t.sidebar().currentIndex();
    REQUIRE(before >= 0);
    auto moved = QSignalSpy { &t.sidebar(), &SidebarModel::currentIndexChanged };

    // Another workspace takes billing's place: its tab's row has billing's tab's index.
    t.source.listed.workspaces = { directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) };
    t.source.listed.selected = 2;
    t.host.displayed = TabRef { .workspace = 2, .tab = 20 };
    t.sidebar().refreshNow();

    REQUIRE(t.sidebar().currentIndex() == before);
    CHECK(t.sidebar().currentIndex()
          == t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Tab, Home, HomeRun, 2, 20)));
    CHECK(moved.count() == 1);
}

TEST_CASE("SidebarModel: Escape's precedence reads the pending jump, and cancelJump ends it",
          "[contour][sidebar]")
{
    auto t =
        SidebarUnderTest { ownHome(), { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) }, {} };
    t.sidebar().refreshNow();
    CHECK_FALSE(t.sidebar().isJumpPending());
    t.host.pending = SidebarTarget {
        .daemon = Starfive, .instance = StarfiveRun, .workspace = 5, .tab = std::nullopt, .pane = {}
    };
    CHECK(t.sidebar().isJumpPending());
    t.sidebar().cancelJump();
    CHECK(t.host.cancels == 1);
}

TEST_CASE("SidebarModel: a window without a daemon lists no daemon rows", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) } };
    t.sections.listed = { sectionOf(Home,
                                    HomeRun,
                                    "default",
                                    DaemonLinkState::Connected,
                                    { directoryWorkspaceOf(7, "elsewhere", { directoryPaneOf(70) }) }),
                          starfiveIn(DaemonLinkState::Connected) };
    t.sections.changed();
    letEventLoopRun();

    CHECK(rowsOfKind(t.sidebar(), SidebarRowKind::Daemon) == 0);
    CHECK(t.sidebar().rowCount() == 2); // billing and its tab
    CHECK(t.sidebar().rows().front().title == "billing");
}

TEST_CASE("SidebarModel: the switcher's workspace rows stay the window's own daemon's", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }),
                                  directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };

    auto const workspaces = t.sidebar().workspaceRows();
    REQUIRE(workspaces.size() == 2);
    CHECK(std::ranges::all_of(workspaces, [](SidebarRow const& row) { return row.key.daemon == Home; }));
    CHECK(t.sidebar().workspaceRowIndex(2)
          == t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Home, HomeRun, 2)));
    CHECK(t.sidebar().workspaceRowIndex(2) >= 0);
    // Starfive's workspace 5 is not the window's: no row of its own named by that id.
    CHECK(t.sidebar().workspaceRowIndex(5) == -1);
}

TEST_CASE("SidebarModel: destroying the model unsubscribes", "[contour][sidebar]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    CHECK(t.sections.subscriberCount() == 1);
    t.model.reset();
    CHECK(t.sections.subscriberCount() == 0);
    t.sections.changed(); // reaches nothing
}

TEST_CASE("SidebarModel: descriptions name the daemon and its link state", "[contour][sidebar][a11y]")
{
    struct Case
    {
        DaemonLinkState state;
        std::string reason;
        QString description;
    };
    // Written out, not read from DaemonLinkLooks, so a changed look shows up here as a changed sentence.
    auto const cases = std::array {
        Case { .state = DaemonLinkState::NotConnected, .reason = {}, .description = "Daemon, not connected" },
        Case { .state = DaemonLinkState::Connecting, .reason = {}, .description = "Daemon, connecting" },
        Case { .state = DaemonLinkState::Connected, .reason = {}, .description = "Daemon, connected" },
        Case { .state = DaemonLinkState::Reconnecting, .reason = {}, .description = "Daemon, reconnecting" },
        Case { .state = DaemonLinkState::NeedsInput, .reason = {}, .description = "Daemon, needs you" },
        Case { .state = DaemonLinkState::NotRunning, .reason = {}, .description = "Daemon, not running" },
        Case { .state = DaemonLinkState::TooOld, .reason = {}, .description = "Daemon, daemon too old" },
        Case { .state = DaemonLinkState::Failed,
               .reason = "host key changed",
               .description = "Daemon, failed, host key changed" },
    };
    static_assert(cases.size() == DaemonLinkStateCount);
    for (auto const& [state, reason, description]: cases)
    {
        INFO(description.toStdString());
        auto section = sectionOf(Starfive, StarfiveRun, "starfive", state, {});
        section.reason = reason;
        auto t = SidebarUnderTest { ownHome(),
                                    { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                    { std::move(section) } };
        auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
        REQUIRE(daemon >= 0);
        // Named by its daemon; its state in words, never by its dot or its mark.
        CHECK(t.sidebar().index(daemon).data(SidebarModel::AccessibleNameRole).toString() == "starfive");
        CHECK(t.sidebar().index(daemon).data(SidebarModel::AccessibleDescriptionRole).toString()
              == description);
    }

    // A daemon with a tree says its fold, and counts what its panes ask, as a workspace does.
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    auto const home = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Home, HomeRun));
    auto const starfive = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    CHECK(t.sidebar().index(home).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Daemon, connected, expanded");
    t.sidebar().toggle(starfive);
    CHECK(t.sidebar().index(starfive).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Daemon, connected, collapsed");
    t.sections.listed[0].directory.workspaces[0].tabs[0].panes[0].status = { recordOf(
        ProgramState::Blocked) };
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(starfive).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Daemon, connected, collapsed, 1 pane needs you");

    // The daemons the user hid are one row, counted in their plural form.
    auto hidden = SidebarRow {};
    hidden.key.kind = SidebarRowKind::Hidden;
    hidden.hidden = { HiddenDaemon { .daemon = Starfive, .name = "starfive" } };
    CHECK(accessibleNameOf(hidden) == "Hidden daemons");
    CHECK(accessibleDescriptionOf(hidden) == "1 hidden daemon");
    hidden.hidden.push_back(HiddenDaemon { .daemon = hostDaemonRef("pi"), .name = "pi" });
    CHECK(accessibleDescriptionOf(hidden) == "2 hidden daemons");
}

TEST_CASE("SidebarModel: a last-known row says so", "[contour][sidebar][a11y]")
{
    auto section = starfiveIn(DaemonLinkState::Reconnecting);
    section.shownElsewhere = { 5 };
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { std::move(section) } };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    auto const workspace = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Starfive, StarfiveRun, 5));
    REQUIRE(daemon >= 0);
    REQUIRE(workspace >= 0);
    CHECK(t.sidebar().index(daemon).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Daemon, reconnecting, expanded, last known");
    CHECK(t.sidebar().index(workspace).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Workspace, collapsed, last known, shown in another window");

    // Back live, the rows say what they are now; a jump on its way to one says it is connecting.
    t.sections.listed[0].state = DaemonLinkState::Connected;
    t.host.pending = SidebarTarget {
        .daemon = Starfive, .instance = StarfiveRun, .workspace = 5, .tab = std::nullopt, .pane = {}
    };
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(workspace).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Workspace, collapsed, shown in another window, connecting");
    // A daemon row whose own link is connecting says so once; what it lists is what was last known.
    t.sections.listed[0].state = DaemonLinkState::Connecting;
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(daemon).data(SidebarModel::AccessibleDescriptionRole).toString()
          == "Daemon, connecting, expanded, last known");
}

TEST_CASE("SidebarModel: a new need on another daemon is announced with its name", "[contour][sidebar][a11y]")
{
    auto t =
        SidebarUnderTest { ownHome(), { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) }, {} };
    t.sidebar().setActivity(WindowActivity::Active);
    auto const blocked = [](DaemonSection& section, std::size_t tab) {
        section.directory.workspaces[0].tabs[tab].panes[0].status = { recordOf(ProgramState::Blocked) };
    };
    auto const starfiveRun = [](DaemonInstance run) {
        return sectionOf(
            Starfive,
            run,
            "starfive",
            DaemonLinkState::Connected,
            { directoryWorkspaceOf(5, "ci", { directoryPaneOf(50, "build"), directoryPaneOf(51, "test") }) });
    };

    // A daemon listed with a pane that already needs the user is not news.
    t.sections.listed = { starfiveRun(StarfiveRun) };
    blocked(t.sections.listed[0], 0);
    t.sidebar().refreshNow();
    CHECK(t.announcer.said.empty());

    // A pane on it that begins to need the user is, once, and says which daemon.
    blocked(t.sections.listed[0], 1);
    t.sidebar().refreshNow();
    t.sidebar().refreshNow();
    REQUIRE(t.announcer.said.size() == 1);
    CHECK(t.announcer.said.front().message == "test in ci on starfive needs you");
    CHECK(t.announcer.said.front().politeness == QAccessible::AnnouncementPoliteness::Polite);

    // Restarted, it is a new baseline: what its new run lists already is not news.
    t.sections.listed = { starfiveRun(DaemonInstance { 10 }) };
    blocked(t.sections.listed[0], 0);
    t.sidebar().refreshNow();
    CHECK(t.announcer.said.size() == 1);
    blocked(t.sections.listed[0], 1);
    t.sidebar().refreshNow();
    CHECK(t.announcer.said.size() == 2);

    // The window's own daemon's needs still say no daemon.
    t.source.listed.workspaces.push_back(directoryWorkspaceOf(2, "docs", { directoryPaneOf(20) }));
    t.sidebar().refreshNow();
    t.source.listed.workspaces[1].tabs[0].panes[0].status = { recordOf(ProgramState::Blocked) };
    t.sidebar().refreshNow();
    REQUIRE(t.announcer.said.size() == 3);
    CHECK(t.announcer.said.back().message == "shell in docs needs you");
}

TEST_CASE("SidebarModel: a jump's landing, cancel and failure are announced politely",
          "[contour][sidebar][a11y]")
{
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    auto const ci = SidebarTarget { .daemon = Starfive, .instance = StarfiveRun, .workspace = 5 };

    t.sidebar().noteJump(JumpEvent::Connecting, ci, {});
    t.sidebar().noteJump(JumpEvent::Landed, ci, {});
    t.sidebar().noteJump(JumpEvent::Cancelled, ci, {});
    t.sidebar().noteJump(JumpEvent::Failed, ci, "host key changed");
    // A daemon's default workspace is none the sidebar names; a daemon it does not list is called by its
    // reference; the window's own is found in its own directory.
    t.sidebar().noteJump(
        JumpEvent::Landed, SidebarTarget { .daemon = Starfive, .instance = StarfiveRun }, {});
    t.sidebar().noteJump(JumpEvent::Failed, SidebarTarget { .daemon = hostDaemonRef("pi") }, {});
    t.sidebar().noteJump(
        JumpEvent::Landed, SidebarTarget { .daemon = Home, .instance = HomeRun, .workspace = 1 }, {});

    auto const said = t.announcer.said
                      | std::views::transform([](auto const& entry) { return entry.message.toStdString(); });
    CHECK(std::ranges::equal(said,
                             std::array<std::string, 7> { "Connecting to starfive…",
                                                          "Now on starfive › ci",
                                                          "Jump cancelled",
                                                          "Could not reach starfive: host key changed",
                                                          "Now on starfive",
                                                          "Could not reach pi",
                                                          "Now on default › billing" }));
    CHECK(std::ranges::all_of(t.announcer.said, [](auto const& entry) {
        return entry.politeness == QAccessible::AnnouncementPoliteness::Polite;
    }));
}

TEST_CASE("SidebarModel: a host that is not running offers neither Start nor Remove", "[contour][sidebar]")
{
    // A host's relay reports its daemon absent: nothing here starts it, and Remove is this machine's verb.
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::NotRunning) } };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    REQUIRE(daemon >= 0);
    CHECK(t.sidebar().index(daemon).data(SidebarModel::DaemonButtonsRole).toList()
          == QVariantList { static_cast<int>(sidebarrows::DaemonRowButton::None),
                            static_cast<int>(sidebarrows::DaemonRowButton::None) });
    // Return folds what was last known of it, and starts nothing.
    std::ignore = t.sidebar().activateRow(daemon);
    CHECK(t.sections.calls.empty());
}

TEST_CASE("SidebarModel: Connect on the window's own section seeks its link again", "[contour][sidebar]")
{
    // The chip's Disconnected reads Failed on the window's own heading: its Connect is the chip's
    // Reconnect, for the daemon sections leave a link a window holds to that window.
    auto own = ownHome();
    own.state = DaemonLinkState::Failed;
    auto t = SidebarUnderTest { own, { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) }, {} };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Home, HomeRun));
    REQUIRE(daemon >= 0);
    CHECK(t.sidebar().index(daemon).data(SidebarModel::DaemonButtonsRole).toList()
          == QVariantList { static_cast<int>(sidebarrows::DaemonRowButton::Connect),
                            static_cast<int>(sidebarrows::DaemonRowButton::None) });
    t.sidebar().pressDaemonButton(daemon, static_cast<int>(sidebarrows::DaemonRowButton::Connect));
    CHECK(t.host.reconnects == 1);
    CHECK(t.sections.calls.empty());

    CHECK(t.host.promptRaises == 0);
}

TEST_CASE("SidebarModel: Connect on the window's own link that needs you raises its prompt",
          "[contour][sidebar]")
{
    // The link already waits for the user: seeking it again would only be refused, and say so in every
    // pane's footer ("the connection is already being sought"). Its Connect, and activating the row, bring
    // the prompt it waits on to the front instead.
    auto own = ownHome();
    own.state = DaemonLinkState::NeedsInput;
    auto t = SidebarUnderTest { own, { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) }, {} };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Home, HomeRun));
    REQUIRE(daemon >= 0);
    t.sidebar().pressDaemonButton(daemon, static_cast<int>(sidebarrows::DaemonRowButton::Connect));
    CHECK(t.host.promptRaises == 1);
    std::ignore = t.sidebar().activateRow(daemon);
    CHECK(t.host.promptRaises == 2);
    CHECK(t.host.reconnects == 0);
    CHECK(t.sections.calls.empty());
}

TEST_CASE("SidebarModel: a daemon another window is on offers no Connect", "[contour][sidebar]")
{
    // Another window of this process holds starfive's link: that window's chip seeks it again.
    auto starfive = starfiveIn(DaemonLinkState::Failed);
    starfive.feed = SectionFeed::WindowLink;
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfive } };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    REQUIRE(daemon >= 0);
    CHECK(t.sidebar().index(daemon).data(SidebarModel::DaemonButtonsRole).toList()
          == QVariantList { static_cast<int>(sidebarrows::DaemonRowButton::None),
                            static_cast<int>(sidebarrows::DaemonRowButton::None) });
    t.sidebar().connectDaemon(daemon);
    CHECK(t.sections.calls.empty());
    CHECK(t.host.reconnects == 0);
}

TEST_CASE("SidebarModel: a daemon row's buttons map to their verbs here", "[contour][sidebar]")
{
    auto const work = localDaemonRef("work");
    auto t = SidebarUnderTest {
        ownHome(),
        { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
        { sectionOf(
              work, DaemonInstance { 3 }, "work", DaemonLinkState::NotRunning, {}, DaemonLocality::Local),
          starfiveIn(DaemonLinkState::NotConnected) }
    };
    auto const stopped = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, work, DaemonInstance { 3 }));
    auto const host = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    using Button = sidebarrows::DaemonRowButton;
    t.sidebar().pressDaemonButton(stopped, static_cast<int>(Button::Start));
    t.sidebar().pressDaemonButton(stopped, static_cast<int>(Button::Remove));
    t.sidebar().pressDaemonButton(host, static_cast<int>(Button::Connect));
    // None, and a value out of range, do nothing.
    t.sidebar().pressDaemonButton(host, static_cast<int>(Button::None));
    t.sidebar().pressDaemonButton(host, 99);
    using Call = contour::test::DaemonSectionsCall;
    CHECK(t.sections.calls
          == std::vector {
              Call { .verb = Call::Start { .daemon = work } },
              Call { .verb = Call::Remove { .daemon = work } },
              Call { .verb = Call::Connect { .daemon = Starfive, .askingWindow = t.host.window } } });
    t.host.pending = SidebarTarget {
        .daemon = Starfive, .instance = StarfiveRun, .workspace = 5, .tab = std::nullopt, .pane = {}
    };
    t.sidebar().pressDaemonButton(host, static_cast<int>(Button::Cancel));
    CHECK(t.host.cancels == 1);
}

TEST_CASE("SidebarModel: the selection is reported for scrolling only when it is another row",
          "[contour][sidebar]")
{
    // The window shows build on its own daemon, listed first; starfive's section below it grows.
    auto t = SidebarUnderTest {
        ownHome(),
        { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
        { sectionOf(Home, HomeRun, "default", DaemonLinkState::Connected, {}, DaemonLocality::Local),
          starfiveIn(DaemonLinkState::Connected) }
    };
    t.host.displayed = TabRef { .workspace = 1, .tab = 10 };
    t.sidebar().refreshNow();
    auto other = QSignalSpy { &t.sidebar(), &SidebarModel::currentRowChanged };

    // A row above the selection appears: its index moves, the row does not.
    t.source.listed.workspaces.insert(t.source.listed.workspaces.begin(),
                                      directoryWorkspaceOf(2, "api", { directoryPaneOf(20) }));
    t.sidebar().refreshNow();
    CHECK(other.count() == 0);

    // The window lands on another tab: that one is brought into view.
    t.host.displayed = TabRef { .workspace = 2, .tab = 20 };
    t.sidebar().refreshNow();
    CHECK(other.count() == 1);
}

TEST_CASE("SidebarModel: the filter's Return with nothing typed goes nowhere", "[contour][sidebar]")
{
    // With no filter and every row listed there is no match: nothing is activated, and the tree takes the
    // keyboard, as before rows on other daemons were listed.
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    CHECK(t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::Here))
          == sidebarrows::RowActivation::Unfiltered);
    CHECK(t.host.jumps.empty());
    CHECK(t.host.shown.empty());
    CHECK(t.host.shownWorkspaces.empty());
}

TEST_CASE("SidebarModel: a pane that matches beneath a tab that does not is where Return goes",
          "[contour][sidebar]")
{
    // One tab, "t", of two panes: the tab is listed as the ancestor of "logs", which it is not.
    auto workspace = directoryWorkspaceOf(1, "billing", { directoryPaneOf(10, "shell") });
    workspace.tabs.front().name = "t";
    workspace.tabs.front().panes.push_back(directoryPaneOf(11, "logs"));
    auto t = SidebarUnderTest { ownHome(), { workspace }, {} };
    t.sidebar().setFilter(QStringLiteral("logs"));
    CHECK(t.sidebar().activateFirstMatch(static_cast<int>(JumpMode::Here))
          == sidebarrows::RowActivation::Shown);
    CHECK(t.host.shown == std::vector { PaneKey { 11 } });
}

TEST_CASE("SidebarModel: a dropped sidebar queues no rebuild", "[contour][sidebar]")
{
    // A window that closes drops its sidebar's subscription: a notice after that rebuilds nothing.
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    REQUIRE(t.sidebar().sectionsFeed() == SectionsFeed::Followed);
    REQUIRE(t.sections.subscriberCount() == 1);
    t.sidebar().followSections(SectionsFeed::Dropped);
    CHECK(t.sidebar().sectionsFeed() == SectionsFeed::Dropped);
    CHECK(t.sections.subscriberCount() == 0);
    t.sections.changed();
    CHECK_FALSE(t.sidebar().isRebuildPending());

    // Following again catches up on what changed meanwhile.
    t.sidebar().followSections(SectionsFeed::Followed);
    CHECK(t.sections.subscriberCount() == 1);
    CHECK(t.sidebar().isRebuildPending());
}

TEST_CASE("SidebarModel: a pending jump marks its row and its daemon row Connecting with Cancel",
          "[contour][sidebar][jump]")
{
    // Spec §5.1: the row the jump goes to and its daemon's heading both show the pending mark, and the
    // heading offers Cancel alone, where Connect was; the rest of the tree is as it was.
    auto t = SidebarUnderTest { ownHome(),
                                { directoryWorkspaceOf(1, "billing", { directoryPaneOf(10) }) },
                                { starfiveIn(DaemonLinkState::Connected) } };
    auto const daemon = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Daemon, Starfive, StarfiveRun));
    auto const workspace = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Starfive, StarfiveRun, 5));
    REQUIRE(daemon >= 0);
    REQUIRE(workspace >= 0);
    CHECK(t.sidebar().index(workspace).data(SidebarModel::PendingRole).toInt()
          == static_cast<int>(RowPending::No));

    t.host.pending = SidebarTarget {
        .daemon = Starfive, .instance = StarfiveRun, .workspace = 5, .tab = std::nullopt, .pane = {}
    };
    t.sidebar().refreshNow();

    CHECK(t.sidebar().index(workspace).data(SidebarModel::PendingRole).toInt()
          == static_cast<int>(RowPending::Yes));
    CHECK(t.sidebar().index(daemon).data(SidebarModel::PendingRole).toInt()
          == static_cast<int>(RowPending::Yes));
    CHECK(t.sidebar().index(daemon).data(SidebarModel::DaemonButtonsRole).toList()
          == QVariantList { static_cast<int>(sidebarrows::DaemonRowButton::Cancel),
                            static_cast<int>(sidebarrows::DaemonRowButton::None) });
    CHECK(t.sidebar().isJumpPending());
    auto const own = t.sidebar().rowIndexOf(keyOf(SidebarRowKind::Workspace, Home, HomeRun, 1));
    REQUIRE(own >= 0);
    CHECK(t.sidebar().index(own).data(SidebarModel::PendingRole).toInt() == static_cast<int>(RowPending::No));

    // Landed: the marks go.
    t.host.pending.reset();
    t.sidebar().refreshNow();
    CHECK(t.sidebar().index(workspace).data(SidebarModel::PendingRole).toInt()
          == static_cast<int>(RowPending::No));
    CHECK_FALSE(t.sidebar().isJumpPending());
}
