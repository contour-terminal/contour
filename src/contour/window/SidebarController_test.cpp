// SPDX-License-Identifier: Apache-2.0
//
// The controller's half of the workspace sidebar: what a window starts with (the configured defaults, or what
// the user last chose), what it declares as horizontal chrome, and what a change writes back to the GUI
// state.

#include <contour/config/GuiStateStore.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/geometry/SidebarGeometry.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QPointF>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QWindow>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <vector>

#include <QtTest/QSignalSpy>

namespace
{

using contour::config::SidebarPrefs;
using contour::config::SidebarVisibility;
using contour::config::TabPlacement;
using contour::window::BarCentre;

/// Gives @p app the sidebar the Native2026 edition ships: shown, with the tabs in it. The test app itself
/// runs the Classic defaults.
void useNativeSidebarDefaults(contour::test::TestApp& app)
{
    app.app().config().sidebar = contour::config::SidebarConfig { .visibility = SidebarVisibility::Shown };
    app.app().config().tabPlacement = TabPlacement::Sidebar;
}

/// @return A sidebar's visibility as the controller publishes it to QML.
[[nodiscard]] int asQml(SidebarVisibility visibility) noexcept
{
    return static_cast<int>(visibility);
}

using contour::window::DirectoryPane;
using contour::window::DirectoryTab;
using contour::window::DirectoryWorkspace;
using contour::window::IWorkspaceDirectorySource;
using contour::window::PaneKey;
using contour::window::SelectResult;
using contour::window::SidebarPresence;
using contour::window::SidebarRowKind;
using contour::window::WorkspaceDirectory;

/// A directory a test scripts, with ids of its own as a daemon's are, and answers it chooses.
struct ScriptedSource final: IWorkspaceDirectorySource
{
    WorkspaceDirectory listed;
    SelectResult answer = SelectResult::Requested;
    std::vector<uint64_t> selected;

    [[nodiscard]] WorkspaceDirectory directory() const override { return listed; }
    SelectResult select(uint64_t workspace) override
    {
        selected.push_back(workspace);
        return answer;
    }
};

[[nodiscard]] DirectoryTab tabHolding(uint64_t id, std::vector<uint64_t> const& panes)
{
    auto tab = DirectoryTab {};
    tab.id = id;
    tab.title = "t";
    tab.label = "t";
    for (auto const key: panes)
    {
        auto pane = DirectoryPane {};
        pane.key = PaneKey { key };
        pane.title = "shell";
        tab.panes.push_back(std::move(pane));
    }
    return tab;
}

/// The model row of the tab with @p tab among @p window's sidebar rows.
[[nodiscard]] int tabRowOf(contour::window::WindowController& window, uint64_t tab)
{
    window.sidebar().refreshNow();
    auto const& rows = window.sidebar().rows();
    auto const at = std::ranges::find_if(
        rows, [&](auto const& row) { return row.key.kind == SidebarRowKind::Tab && row.key.tab == tab; });
    REQUIRE(at != rows.end());
    return static_cast<int>(std::distance(rows.begin(), at));
}

[[nodiscard]] std::optional<int> actionIdOf(QVariantList const& rows, QString const& title)
{
    for (auto const& value: rows)
    {
        auto const row = value.toMap();
        if (row["kind"].toString() == "command" && row["title"].toString() == title)
            return row["actionId"].toInt();
        if (auto const nested = actionIdOf(row["children"].toList(), title))
            return nested;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a sidebar menu on a row that is not current acts on that row", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(app.manager(), *window, 3);
    REQUIRE(window->count() == 3);

    // The window shows the third tab; the menu is opened on the first.
    window->sidebar().openContextMenu(tabRowOf(*window, made.tabs[0].value), QPointF { 3, 4 });
    auto const closeOthers = actionIdOf(window->sidebarContextMenuModel(), "Close Other Tabs");
    REQUIRE(closeOthers.has_value());
    window->triggerSidebarContextMenuAction(*closeOthers);

    REQUIRE(window->count() == 1);
    CHECK(app.manager().model().findTab(made.tabs[0]) != nullptr);
    CHECK(app.manager().model().findTab(made.tabs[2]) == nullptr);
}

TEST_CASE("a sidebar action whose pane is not there waits and never runs on the active one",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    std::ignore = contour::test::createSessionTabs(app.manager(), *window, 2);

    // The directory lists two tabs whose panes this window has no session for.
    auto source = ScriptedSource {};
    auto const shown = window->directorySource()->directory().selected;
    REQUIRE(shown.has_value());
    source.listed.selected = shown;
    source.listed.workspaces = { DirectoryWorkspace {
        .id = *shown, .label = "w", .tabs = { tabHolding(901, { 9001 }), tabHolding(902, { 9002 }) } } };
    window->setDirectorySource(&source);

    window->sidebar().openContextMenu(tabRowOf(*window, 901), QPointF {});
    auto const closeOthers = actionIdOf(window->sidebarContextMenuModel(), "Close Other Tabs");
    REQUIRE(closeOthers.has_value());
    window->triggerSidebarContextMenuAction(*closeOthers);

    CHECK(window->count() == 2); // nothing ran against the active tab
    CHECK(window->rowActionPending());

    window->cancelPendingJump();
    CHECK_FALSE(window->rowActionPending());
}

TEST_CASE("a refused workspace selection drops the sidebar action that waited for it",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    std::ignore = contour::test::createSessionTabs(app.manager(), *window, 2);

    auto source = ScriptedSource {};
    auto const shown = window->directorySource()->directory().selected.value_or(1);
    source.listed.selected = shown;
    source.listed.workspaces = {
        DirectoryWorkspace { .id = shown, .label = "here", .tabs = { tabHolding(1, { 11 }) } },
        DirectoryWorkspace {
            .id = 777, .label = "there", .tabs = { tabHolding(2, { 22 }), tabHolding(3, { 33 }) } }
    };
    window->setDirectorySource(&source);

    window->sidebar().openContextMenu(tabRowOf(*window, 2), QPointF {});
    auto const closeOthers = actionIdOf(window->sidebarContextMenuModel(), "Close Other Tabs");
    REQUIRE(closeOthers.has_value());

    SECTION("refused: nothing waits")
    {
        source.answer = SelectResult::Refused;
        window->triggerSidebarContextMenuAction(*closeOthers);
        CHECK(source.selected == std::vector<uint64_t> { 777 });
        CHECK_FALSE(window->rowActionPending());
        CHECK(window->count() == 2);
    }
    SECTION("requested: it waits for the workspace, and a refusal later cancels it")
    {
        window->triggerSidebarContextMenuAction(*closeOthers);
        CHECK(source.selected == std::vector<uint64_t> { 777 });
        CHECK(window->rowActionPending());
        window->cancelPendingJump();
        CHECK_FALSE(window->rowActionPending());
        CHECK(window->count() == 2);
    }
}

TEST_CASE("a sidebar menu's Mark seen, Move and Rename act on a background row without showing it",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    std::ignore = contour::test::createSessionTabs(app.manager(), *window, 1);

    // A background workspace whose second tab's pane finished a command nobody has seen.
    auto source = ScriptedSource {};
    auto const shown = window->directorySource()->directory().selected.value_or(1);
    source.listed.selected = shown;
    auto there = DirectoryWorkspace { .id = 777,
                                      .label = "there",
                                      .tabs = { tabHolding(2, { 22 }), tabHolding(3, { 33 }) } };
    there.tabs[1].panes[0].finished = vthost::proto::FinishedCommand { .exitCode = 0 };
    there.tabs[1].panes[0].finishedSerial = 1;
    source.listed.workspaces = {
        DirectoryWorkspace { .id = shown, .label = "here", .tabs = { tabHolding(1, { 11 }) } },
        std::move(there),
    };
    window->setDirectorySource(&source);
    auto const workspaceRow = [&] {
        window->sidebar().refreshNow();
        auto const at = window->sidebar().rowIndexOf(
            { .kind = SidebarRowKind::Workspace, .workspace = 777, .tab = 0, .pane = {} });
        REQUIRE(at >= 0);
        return at;
    };
    auto const urgencyOfThere = [&] {
        return window->sidebar().rows().at(static_cast<std::size_t>(workspaceRow())).urgency;
    };
    REQUIRE(urgencyOfThere() == contour::window::Urgency::DoneUnseen);

    SECTION("Mark seen on the workspace row marks the pane in its background tab")
    {
        window->sidebar().openContextMenu(workspaceRow(), QPointF {});
        auto const markSeen = actionIdOf(window->sidebarContextMenuModel(), "Mark Seen");
        REQUIRE(markSeen.has_value());
        window->triggerSidebarContextMenuAction(*markSeen);

        CHECK(source.selected.empty()); // the window stayed where it was
        CHECK_FALSE(window->rowActionPending());
        CHECK(urgencyOfThere() == contour::window::Urgency::Idle);
    }
    SECTION("Rename… opens the editor on that row")
    {
        auto renames = QSignalSpy { &*window, &contour::window::WindowController::sidebarRowRenameRequested };
        auto const row = tabRowOf(*window, 3);
        window->sidebar().openContextMenu(row, QPointF {});
        auto const rename = actionIdOf(window->sidebarContextMenuModel(), "Rename…");
        REQUIRE(rename.has_value());
        window->triggerSidebarContextMenuAction(*rename);

        CHECK(source.selected.empty());
        CHECK_FALSE(window->rowActionPending());
        REQUIRE(renames.count() == 1);
        CHECK(renames.at(0).at(0).toInt() == row);
    }
    SECTION("Move Up asks for the move, and nothing is selected")
    {
        window->sidebar().openContextMenu(workspaceRow(), QPointF {});
        auto const moveUp = actionIdOf(window->sidebarContextMenuModel(), "Move Up");
        REQUIRE(moveUp.has_value());
        window->triggerSidebarContextMenuAction(*moveUp);

        CHECK(source.selected.empty());
        CHECK_FALSE(window->rowActionPending());
    }
}

TEST_CASE("the displayed tab is the one holding the active pane, whatever id the source gives it",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    std::ignore = contour::test::createSessionTabs(app.manager(), *window, 2);

    SECTION("in-process: the window's own source")
    {
        window->sidebar().refreshNow();
        REQUIRE(window->sidebar().currentIndex() >= 0);
        auto const& row =
            window->sidebar().rows()[static_cast<std::size_t>(window->sidebar().currentIndex())];
        CHECK(row.key.kind == SidebarRowKind::Tab);
        CHECK(row.key.tab
              == app.manager().model().workspace(window->displayedWorkspace())->activeTab()->id().value);
    }
    SECTION("a daemon's source: its tab ids are its own")
    {
        auto const active = app.manager().paneKeyOf(window->activeSession()->modelSessionId());
        auto source = ScriptedSource {};
        auto const shown = window->directorySource()->directory().selected.value_or(1);
        source.listed.selected = shown;
        source.listed.workspaces = { DirectoryWorkspace {
            .id = shown,
            .label = "d",
            .tabs = { tabHolding(5001, { 1 }), tabHolding(5002, { active.value }) } } };
        window->setDirectorySource(&source);
        window->sidebar().refreshNow();
        REQUIRE(window->sidebar().currentIndex() >= 0);
        auto const& row =
            window->sidebar().rows()[static_cast<std::size_t>(window->sidebar().currentIndex())];
        CHECK(row.key.tab == 5002);
    }
}

TEST_CASE("the sidebar is told whether anyone can see it", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto window = contour::test::ScopedControllerWithOsWindow { app.manager() };
    window->bindWindow(window.osWindow.get());
    REQUIRE(window->sidebarVisibility() == static_cast<int>(SidebarVisibility::Hidden));
    CHECK(window->sidebar().presence() == SidebarPresence::Hidden);

    // Shown in a window that is not on screen: nobody sees it.
    window->toggleSidebar();
    CHECK(window->sidebar().presence() == SidebarPresence::Hidden);

    window.osWindow->show();
    CHECK(window->sidebar().presence() == SidebarPresence::Shown);

    window.osWindow->showMinimized();
    CHECK(window->sidebar().presence() == SidebarPresence::Hidden);

    window.osWindow->showNormal();
    CHECK(window->sidebar().presence() == SidebarPresence::Shown);

    window->toggleSidebar();
    CHECK(window->sidebar().presence() == SidebarPresence::Hidden);
}

TEST_CASE("a window starts with the configured sidebar when nothing was chosen",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    auto const metrics = app.app().sidebarMetrics();

    SECTION("the Classic defaults hide it and keep the tabs on top")
    {
        auto window = contour::test::ScopedController { app.manager() };
        CHECK(window.controller->sidebarVisibility() == asQml(SidebarVisibility::Hidden));
        CHECK(window.controller->tabPlacement() == static_cast<int>(TabPlacement::Top));
        CHECK(window.controller->sidebarWidth() == 0);
        CHECK(window.controller->titleBand().centre == BarCentre::Tabs);
    }

    useNativeSidebarDefaults(app);
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;

    // The newest edition's defaults: the sidebar shown, the tabs in it.
    CHECK(controller.sidebarVisibility() == asQml(SidebarVisibility::Shown));
    CHECK(controller.tabPlacement() == static_cast<int>(TabPlacement::Sidebar));
    CHECK(controller.sidebarWidth() == metrics.tokens.width);
    CHECK(controller.titleBand().centre == BarCentre::Breadcrumb);
}

TEST_CASE("GuiState sidebar prefs win over the config default", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    useNativeSidebarDefaults(app);
    auto const metrics = app.app().sidebarMetrics();
    auto const remembered = metrics.tokens.minWidth + 11;
    app.guiState().state.sidebar = SidebarPrefs { .visibility = SidebarVisibility::Hidden,
                                                  .widthPx = remembered,
                                                  .widthCells = std::nullopt };
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;

    CHECK(controller.sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    // Placement is configuration only: what was remembered does not touch it.
    CHECK(controller.tabPlacement() == static_cast<int>(TabPlacement::Sidebar));
    // Hidden, the sidebar takes no width...
    CHECK(controller.sidebarWidth() == 0);
    // ...and shown again it takes the width the user left it at, not the row's own.
    controller.toggleSidebar();
    CHECK(controller.sidebarWidth() == remembered);
}

TEST_CASE("chrome width is the sidebar width when shown, 0 when hidden (window)",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    useNativeSidebarDefaults(app);
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;
    auto widthChanges = QSignalSpy { &controller, &contour::window::WindowController::sidebarWidthChanged };
    auto const shown = controller.sidebarWidth();
    REQUIRE(shown > 0);

    controller.toggleSidebar();
    CHECK(controller.sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    CHECK(controller.sidebarWidth() == 0);
    CHECK(controller.titleBand().centre == BarCentre::ChipAndTabs);

    controller.toggleSidebar();
    CHECK(controller.sidebarWidth() == shown);
    CHECK(widthChanges.count() == 2);
}

TEST_CASE("a sidebar change is remembered", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    useNativeSidebarDefaults(app);
    auto const metrics = app.app().sidebarMetrics();
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;
    REQUIRE_FALSE(app.guiState().state.sidebar.has_value());

    SECTION("a width is clamped to the row's bounds, then written")
    {
        controller.setSidebarWidth(100000);
        CHECK(controller.sidebarWidth() == metrics.tokens.maxWidth);
        REQUIRE(app.guiState().state.sidebar.has_value());
        CHECK(app.guiState().state.sidebar->widthPx == metrics.tokens.maxWidth);
        CHECK(app.guiState().state.sidebar->visibility == SidebarVisibility::Shown);

        controller.setSidebarWidth(1);
        CHECK(controller.sidebarWidth() == metrics.tokens.minWidth);
        CHECK(app.guiState().state.sidebar->widthPx == metrics.tokens.minWidth);
    }

    SECTION("a width that changes nothing writes nothing")
    {
        auto const saves = app.guiState().saves;
        controller.setSidebarWidth(controller.sidebarWidth());
        CHECK(app.guiState().saves == saves);
    }

    SECTION("a visibility is written, and the next window starts with it")
    {
        controller.toggleSidebar();
        REQUIRE(app.guiState().state.sidebar.has_value());
        CHECK(app.guiState().state.sidebar->visibility == SidebarVisibility::Hidden);

        auto next = contour::test::ScopedController { app.manager() };
        CHECK(next.controller->sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    }
}

TEST_CASE("a config reload changes an open window's tab placement", "[contour][sidebar][controller]")
{
    using contour::test::ConfiguredTestApp;
    auto app = ConfiguredTestApp {};
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;
    REQUIRE(controller.tabPlacement() == static_cast<int>(TabPlacement::Top));
    auto changes = QSignalSpy { &controller, &contour::window::WindowController::tabPlacementChanged };

    contour::test::writeConfigFile(
        app.configFile(),
        contour::test::profileConfigYaml(app.app().profileName(), "tab_placement: sidebar\n"));
    REQUIRE(app.manager().reloadAllSessions());
    CHECK(controller.tabPlacement() == static_cast<int>(TabPlacement::Sidebar));
    CHECK(changes.count() == 1);

    // The same placement again says nothing, and nothing of placement was remembered.
    REQUIRE(app.manager().reloadAllSessions());
    CHECK(changes.count() == 1);
}

TEST_CASE("a Terminal-row sidebar width is remembered in cells, not pixels", "[contour][sidebar][geometry]")
{
    using contour::config::LengthUnit;
    auto prefs = SidebarPrefs { .visibility = SidebarVisibility::Shown, .widthPx = 260 };
    contour::geometry::rememberSidebarUnits(prefs, LengthUnit::Cell, 30);
    CHECK(prefs.widthCells == 30);
    CHECK(prefs.widthPx == 260);
    CHECK(contour::geometry::rememberedSidebarUnits(prefs, LengthUnit::Cell) == 30);
    CHECK(contour::geometry::rememberedSidebarUnits(prefs, LengthUnit::Pixel) == 260);

    // The same on a pixel row leaves the cells alone.
    contour::geometry::rememberSidebarUnits(prefs, LengthUnit::Pixel, 300);
    CHECK(prefs.widthPx == 300);
    CHECK(prefs.widthCells == 30);
}

TEST_CASE("a sidebar menu's Close Tab… asks first and closes the row it was opened on only once confirmed",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(app.manager(), *window, 3);
    REQUIRE(window->count() == 3);

    auto asked =
        QSignalSpy { &*window, &contour::window::WindowController::sidebarCloseConfirmationRequested };
    window->sidebar().openContextMenu(tabRowOf(*window, made.tabs[0].value), QPointF { 3, 4 });
    auto const close = actionIdOf(window->sidebarContextMenuModel(), "Close Tab…");
    REQUIRE(close.has_value());
    window->triggerSidebarContextMenuAction(*close);

    // It asks, though nothing runs in the tab, and the tab lives on.
    auto const closedSession = [&](std::size_t index) {
        auto const* session = app.manager().sessionForId(made.sessions[index]);
        REQUIRE(session != nullptr);
        return session->terminal().device().isClosed();
    };
    REQUIRE(asked.count() == 1);
    CHECK(asked.front().at(0).toInt() == static_cast<int>(contour::window::CloseKind::Tab));
    CHECK(asked.front().at(2).toULongLong() == made.tabs[0].value);
    CHECK(window->count() == 3);
    CHECK_FALSE(closedSession(0));

    // The destructive answer ends the tab the menu was opened on, not the one the window shows.
    auto const wording = window->closeConfirmation(
        static_cast<int>(contour::window::CloseKind::Tab), 0, made.tabs[0].value, "t", {});
    CHECK(wording.value("acceptText").toString() == "Close Tab");
    window->confirmClose(static_cast<int>(contour::window::CloseKind::Tab),
                         asked.front().at(1).toULongLong(),
                         made.tabs[0].value,
                         0);
    CHECK(closedSession(0));
    CHECK_FALSE(closedSession(1));
    CHECK_FALSE(closedSession(2));
}
