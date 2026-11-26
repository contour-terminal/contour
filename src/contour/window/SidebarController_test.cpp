// SPDX-License-Identifier: Apache-2.0
//
// The controller's half of the workspace sidebar: what a window starts with (the configured defaults, or what
// the user last chose), what it declares as horizontal chrome, and what a change writes back to the GUI
// state.

#include <contour/config/Actions.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/geometry/SidebarGeometry.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/ContextMenuRows.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/SidebarFakes.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/SwitcherPresenter.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QColor>
#include <QtGui/QWindow>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
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

using contour::test::FakeDirectorySource;
using contour::window::DirectoryPane;
using contour::window::DirectoryTab;
using contour::window::DirectoryWorkspace;
using contour::window::PaneKey;
using contour::window::SelectResult;
using contour::window::SidebarPresence;
using contour::window::SidebarRowKind;

[[nodiscard]] DirectoryTab tabHolding(uint64_t id, std::vector<uint64_t> const& panes)
{
    auto tab = DirectoryTab {};
    tab.id = id;
    tab.title = "t";
    tab.name = "t";
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
    auto source = FakeDirectorySource {};
    auto const shown = window->directorySource()->directory().selected;
    REQUIRE(shown.has_value());
    source.listed.selected = shown;
    source.listed.workspaces = { DirectoryWorkspace {
        .id = *shown, .name = "w", .tabs = { tabHolding(901, { 9001 }), tabHolding(902, { 9002 }) } } };
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

    auto source = FakeDirectorySource {};
    auto const shown = window->directorySource()->directory().selected.value_or(1);
    source.listed.selected = shown;
    source.listed.workspaces = {
        DirectoryWorkspace { .id = shown, .name = "here", .tabs = { tabHolding(1, { 11 }) } },
        DirectoryWorkspace {
            .id = 777, .name = "there", .tabs = { tabHolding(2, { 22 }), tabHolding(3, { 33 }) } }
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
    auto source = FakeDirectorySource {};
    auto const shown = window->directorySource()->directory().selected.value_or(1);
    source.listed.selected = shown;
    auto there = DirectoryWorkspace { .id = 777,
                                      .name = "there",
                                      .tabs = { tabHolding(2, { 22 }), tabHolding(3, { 33 }) } };
    there.tabs[1].panes[0].finished = vthost::proto::FinishedCommand { .exitCode = 0 };
    there.tabs[1].panes[0].finishedSerial = 1;
    source.listed.workspaces = {
        DirectoryWorkspace { .id = shown, .name = "here", .tabs = { tabHolding(1, { 11 }) } },
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
        auto source = FakeDirectorySource {};
        auto const shown = window->directorySource()->directory().selected.value_or(1);
        source.listed.selected = shown;
        source.listed.workspaces = { DirectoryWorkspace {
            .id = shown,
            .name = "d",
            .tabs = { tabHolding(5001, { 1 }), tabHolding(5002, { active.value }) } } };
        window->setDirectorySource(&source);
        window->sidebar().refreshNow();
        REQUIRE(window->sidebar().currentIndex() >= 0);
        auto const& row =
            window->sidebar().rows()[static_cast<std::size_t>(window->sidebar().currentIndex())];
        CHECK(row.key.tab == 5002);
    }
}

TEST_CASE("colouring a tab repaints its sidebar row", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const window =
        contour::test::ScopedController { app.manager(), contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(app.manager(), *window, 1);
    auto const tab = made.tabs.at(0);
    auto const row = tabRowOf(*window, tab.value);
    auto const tabColorOfRow = [&] {
        return window->sidebar().index(row).data(contour::window::SidebarModel::TabColorRole).value<QColor>();
    };
    REQUIRE(tabColorOfRow().alpha() == 0);

    // Only the model is told; the window's own notifications must bring the row up to date.
    app.manager().model().setTabColor(
        tab, vtworkspace::TabColorSource::User, vtbackend::RGBColor { 0xE8, 0x85, 0x3A });
    QCoreApplication::processEvents();
    CHECK(tabColorOfRow() == QColor { 0xE8, 0x85, 0x3A });

    app.manager().model().resetTabColor(tab, vtworkspace::TabColorSource::User);
    QCoreApplication::processEvents();
    CHECK(tabColorOfRow().alpha() == 0);
}

TEST_CASE("the sidebar lists a workspace's tabs in the tab bar's order", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(manager, *window, 3);
    auto const a = made.tabs[0].value;
    auto const b = made.tabs[1].value;
    auto const c = made.tabs[2].value;
    window->activateTab(0);
    QCoreApplication::processEvents();

    // The sidebar's tab rows, as they stand: no refresh is forced, so a move nobody announced shows.
    auto const sidebarTabs = [&] {
        return std::ranges::to<std::vector>(
            window->sidebar().rows()
            | std::views::filter([](auto const& row) { return row.key.kind == SidebarRowKind::Tab; })
            | std::views::transform([](auto const& row) { return row.key.tab; }));
    };
    // The tab bar's order: the model's tabs of the displayed workspace, row by row.
    auto const stripTabs = [&] {
        auto const* const shown = manager.model().workspace(window->displayedWorkspace());
        return std::ranges::to<std::vector>(
            std::views::iota(0, shown->tabCount())
            | std::views::transform([&](int i) { return shown->tabAt(i)->id().value; }));
    };
    window->sidebar().refreshNow();
    REQUIRE(sidebarTabs() == std::vector<uint64_t> { a, b, c });

    SECTION("a move that leaves the active tab where it was")
    {
        manager.moveTab(window->displayedWorkspace(), 2, 1);
        QCoreApplication::processEvents();
        CHECK(stripTabs() == std::vector<uint64_t> { a, c, b });
        CHECK(sidebarTabs() == stripTabs());
    }
    SECTION("a move of the active tab itself")
    {
        manager.moveTab(window->displayedWorkspace(), 0, 2);
        QCoreApplication::processEvents();
        CHECK(stripTabs() == std::vector<uint64_t> { b, c, a });
        CHECK(sidebarTabs() == stripTabs());
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

TEST_CASE("a window takes sidebar.row_spacing when it opens, and keeps it over a reload",
          "[contour][sidebar][controller]")
{
    using contour::config::SidebarRowSpacing;
    using contour::test::ConfiguredTestApp;
    auto app = ConfiguredTestApp {};
    auto window = contour::test::ScopedController { app.manager() };
    REQUIRE(window.controller->sidebarRowSpacing() == static_cast<int>(SidebarRowSpacing::Comfortable));

    contour::test::writeConfigFile(
        app.configFile(),
        contour::test::profileConfigYaml(app.app().profileName(), "sidebar:\n    row_spacing: compact\n"));
    REQUIRE(app.manager().reloadAllSessions());
    REQUIRE(app.app().config().sidebar.value().rowSpacing == SidebarRowSpacing::Compact);
    // The open window keeps the rows it opened with; the next one opens compact.
    CHECK(window.controller->sidebarRowSpacing() == static_cast<int>(SidebarRowSpacing::Comfortable));
    auto next = contour::test::ScopedController { app.manager() };
    CHECK(next.controller->sidebarRowSpacing() == static_cast<int>(SidebarRowSpacing::Compact));
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
    auto const node = asked.front().at(0);
    REQUIRE(node.metaType() == QMetaType::fromType<contour::window::CloseNode>());
    CHECK(node.value<contour::window::CloseNode>().kind == contour::window::CloseKind::Tab);
    CHECK(node.value<contour::window::CloseNode>().row.tab == made.tabs[0].value);
    CHECK(window->count() == 3);
    CHECK_FALSE(closedSession(0));

    // The destructive answer ends the tab the menu was opened on, not the one the window shows.
    auto const wording = window->closeConfirmation(node, "t", {});
    CHECK(wording.value("acceptText").toString() == "Close Tab");
    CHECK(window->closeConfirmation(QVariant { 7 }, "t", {}).isEmpty());
    window->confirmClose(node);
    CHECK(closedSession(0));
    CHECK_FALSE(closedSession(1));
    CHECK_FALSE(closedSession(2));
}

namespace
{
/// The model row of pane @p pane among @p window's sidebar rows.
[[nodiscard]] int paneRowOf(contour::window::WindowController& window, PaneKey pane)
{
    window.sidebar().refreshNow();
    auto const& rows = window.sidebar().rows();
    auto const at = std::ranges::find_if(
        rows, [&](auto const& row) { return row.key.kind == SidebarRowKind::Pane && row.key.pane == pane; });
    REQUIRE(at != rows.end());
    return static_cast<int>(std::distance(rows.begin(), at));
}

/// The model row of workspace @p workspace among @p window's sidebar rows.
[[nodiscard]] int workspaceRowOf(contour::window::WindowController& window, uint64_t workspace)
{
    window.sidebar().refreshNow();
    auto const& rows = window.sidebar().rows();
    auto const at = std::ranges::find_if(rows, [&](auto const& row) {
        return row.key.kind == SidebarRowKind::Workspace && row.key.workspace == workspace;
    });
    REQUIRE(at != rows.end());
    return static_cast<int>(std::distance(rows.begin(), at));
}

using contour::test::menuRowOf;

/// @return The action ids of the command rows of submenu @p title, in order.
[[nodiscard]] std::vector<int> submenuActionsOf(QVariantList const& rows, QString const& title)
{
    auto ids = std::vector<int> {};
    for (auto const& child: menuRowOf(rows, title)["children"].toList())
        if (auto const row = child.toMap(); row["kind"].toString() == "command")
            ids.push_back(row["actionId"].toInt());
    return ids;
}
} // namespace

TEST_CASE("a sidebar pane menu moves its pane into the tab it offered, though the tabs moved meanwhile",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(manager, *window, 3);
    manager.splitActivePane(false, window->activeSession());
    auto const& model = manager.model();
    REQUIRE(model.findTab(made.tabs[2])->paneCount() == 2);
    auto const pane = manager.paneKeyOf(model.findTab(made.tabs[2])->activePane()->session());

    window->sidebar().openContextMenu(paneRowOf(*window, pane), QPointF { 3, 4 });
    // Its tab has another pane, so both moves out of it are offered.
    CHECK(menuRowOf(window->sidebarContextMenuModel(), "Move to New Tab")["enabled"].toBool());
    CHECK(menuRowOf(window->sidebarContextMenuModel(), "Move to New Workspace")["enabled"].toBool());
    // The tabs it can join: the first and the second, its own excluded.
    auto const joins = submenuActionsOf(window->sidebarContextMenuModel(), "Move to Tab");
    REQUIRE(joins.size() == 2);

    // The first tab moves to the end while the menu is open: the row offered for the second tab still means
    // the second, where its places now name the pane's own tab.
    manager.moveTab(window->displayedWorkspace(), 0, 2);
    window->triggerSidebarContextMenuAction(joins[1]);
    CHECK(model.findTab(made.tabs[1])->paneCount() == 2);
    CHECK(model.findTab(made.tabs[2])->paneCount() == 1);
}

TEST_CASE("a sidebar pane menu moves its pane to a new tab and to a new workspace",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(manager, *window, 2);
    manager.splitActivePane(false, window->activeSession());
    auto const& model = manager.model();
    auto const workspace = window->displayedWorkspace();
    REQUIRE(model.findTab(made.tabs[1])->paneCount() == 2);
    auto const pane = manager.paneKeyOf(model.findTab(made.tabs[1])->activePane()->session());

    window->sidebar().openContextMenu(paneRowOf(*window, pane), QPointF { 3, 4 });
    auto const newTab = actionIdOf(window->sidebarContextMenuModel(), "Move to New Tab");
    REQUIRE(newTab.has_value());
    window->triggerSidebarContextMenuAction(*newTab);
    CHECK(model.workspace(workspace)->tabCount() == 3);
    CHECK(model.findTab(made.tabs[1])->paneCount() == 1);
    // Its tab of its own lists no pane row any more: the move back into a split comes from the drag.
    manager.moveSidebarNode(
        *window,
        pane,
        contour::window::JoinTab { .tab = { .workspace = workspace.value, .tab = made.tabs[1].value } });
    REQUIRE(model.findTab(made.tabs[1])->paneCount() == 2);
    CHECK(model.workspace(workspace)->tabCount() == 2);

    window->sidebar().openContextMenu(paneRowOf(*window, pane), QPointF { 3, 4 });
    auto const newWorkspace = actionIdOf(window->sidebarContextMenuModel(), "Move to New Workspace");
    REQUIRE(newWorkspace.has_value());
    window->triggerSidebarContextMenuAction(*newWorkspace);
    CHECK(window->displayedWorkspace() != workspace);
    CHECK(model.workspace(workspace)->tabCount() == 2);
    CHECK(model.findTab(made.tabs[1])->paneCount() == 1);
    CHECK(window->ownedWorkspaces().size() == 2);
}

namespace
{
/// A tab split once: its first pane A and the pane B split off it, B active.
struct SplitTab
{
    vtworkspace::TabId tab;
    PaneKey a;
    PaneKey b;
};

[[nodiscard]] SplitTab splitLastTab(contour::session::TerminalSessionManager& manager,
                                    contour::window::WindowController& window,
                                    vtworkspace::TabId tab)
{
    auto const& model = manager.model();
    auto const a = manager.paneKeyOf(model.findTab(tab)->activePane()->session());
    manager.splitActivePane(false, window.activeSession());
    REQUIRE(model.findTab(tab)->paneCount() == 2);
    auto const b = manager.paneKeyOf(model.findTab(tab)->activePane()->session());
    REQUIRE(a != b);
    return SplitTab { .tab = tab, .a = a, .b = b };
}

/// @return The leaf of @p tab whose session is kept under @p key.
[[nodiscard]] vtworkspace::Pane* leafOf(contour::session::TerminalSessionManager& manager,
                                        vtworkspace::TabId tab,
                                        PaneKey key)
{
    auto* const leaf = manager.model().findTab(tab)->rootPane()->walkTree(
        [&](vtworkspace::Pane& pane) { return pane.isLeaf() && manager.paneKeyOf(pane.session()) == key; });
    REQUIRE(leaf != nullptr);
    return leaf;
}

/// @return The node beside @p pane under its parent; null for the root.
[[nodiscard]] vtworkspace::Pane const* siblingOf(vtworkspace::Pane const& pane)
{
    auto const* const parent = pane.parent();
    if (parent == nullptr)
        return nullptr;
    return parent->first() == &pane ? parent->second() : parent->first();
}

/// Checks that a split put a new leaf beside @p split's @p divided, leaving the other pane's leaf where it
/// was.
void checkSplitBeside(contour::session::TerminalSessionManager& manager,
                      SplitTab const& split,
                      PaneKey divided)
{
    auto const other = divided == split.a ? split.b : split.a;
    REQUIRE(manager.model().findTab(split.tab)->paneCount() == 3);
    auto const* const sibling = siblingOf(*leafOf(manager, split.tab, divided));
    REQUIRE(sibling != nullptr);
    CHECK(sibling->isLeaf());
    CHECK(manager.paneKeyOf(sibling->session()) != other);
    // The other pane's sibling is the split that now holds the divided pane.
    auto const* const otherSibling = siblingOf(*leafOf(manager, split.tab, other));
    REQUIRE(otherSibling != nullptr);
    CHECK_FALSE(otherSibling->isLeaf());
}
} // namespace

TEST_CASE("a pane row's split divides that pane", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(manager, *window, 1);
    auto const split = splitLastTab(manager, *window, made.tabs[0]);

    window->sidebar().openContextMenu(paneRowOf(*window, split.a), QPointF { 3, 4 });
    auto const action = actionIdOf(window->sidebarContextMenuModel(), "Split Vertically");
    REQUIRE(action.has_value());
    window->triggerSidebarContextMenuAction(*action);

    checkSplitBeside(manager, split, split.a);
}

TEST_CASE("a tab row's split divides the tab's active pane", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(manager, *window, 2);
    auto const split = splitLastTab(manager, *window, made.tabs[1]);
    auto const workspace = window->displayedWorkspace();

    SECTION("the tab shown")
    {
        // Nothing to do: the split tab is the one shown.
    }
    SECTION("another tab shown: the tab is shown first")
    {
        manager.activateTab(workspace, 0);
        REQUIRE(manager.model().workspace(workspace)->activeTab()->id() == made.tabs[0]);
    }

    window->sidebar().openContextMenu(tabRowOf(*window, split.tab.value), QPointF { 3, 4 });
    auto const action = actionIdOf(window->sidebarContextMenuModel(), "Split Horizontally");
    REQUIRE(action.has_value());
    window->triggerSidebarContextMenuAction(*action);

    CHECK(manager.model().workspace(workspace)->activeTab()->id() == split.tab);
    checkSplitBeside(manager, split, split.b);
}

TEST_CASE("a one-tab workspace's menu merges its tab into another tab", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const first = contour::test::createSessionTabs(manager, *window, 2);
    auto const firstWorkspace = window->displayedWorkspace();
    auto const second = manager.createWorkspaceIn(window->windowId());
    manager.selectWorkspace(window->windowId(), 1);
    auto const lone = contour::test::createSessionTabs(manager, *window, 1);
    manager.selectWorkspace(window->windowId(), 0);
    auto const& model = manager.model();

    window->sidebar().openContextMenu(workspaceRowOf(*window, second.value), QPointF { 3, 4 });
    auto const merges = submenuActionsOf(window->sidebarContextMenuModel(), "Merge into Tab");
    REQUIRE(merges.size() == 2);
    window->triggerSidebarContextMenuAction(merges[0]);
    // Its tab joined the first workspace's first tab; the workspace it emptied is gone.
    CHECK(model.findTab(first.tabs[0])->paneCount() == 2);
    CHECK(model.findTab(lone.tabs[0]) == nullptr);
    CHECK(window->ownedWorkspaces().size() == 1);
    CHECK(window->displayedWorkspace() == firstWorkspace);
}

namespace
{
/// @return The row key a colour pick request (workspaceColorPickRequested(), sidebarTabColorPickRequested())
///         carries for the picker to hand back.
[[nodiscard]] contour::window::SidebarRowKey pickedRow(QList<QVariant> const& request)
{
    auto const key = request.at(1);
    REQUIRE(key.metaType() == QMetaType::fromType<contour::window::SidebarRowKey>());
    return key.value<contour::window::SidebarRowKey>();
}

/// @return Row @p row of @p window's sidebar's key, as a picker carries it.
[[nodiscard]] QVariant rowKeyAt(contour::window::WindowController& window, int row)
{
    REQUIRE(row >= 0);
    return QVariant::fromValue(window.sidebar().rows()[static_cast<std::size_t>(row)].key);
}
} // namespace

TEST_CASE("SetWorkspaceColor colours the displayed workspace's rows, ResetWorkspaceColor clears them",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    std::ignore = contour::test::createSessionTabs(manager, *window, 1);
    auto const shown = window->displayedWorkspace();
    auto const other = manager.createWorkspaceIn(window->windowId());
    manager.selectWorkspace(window->windowId(), 1);
    std::ignore = contour::test::createSessionTabs(manager, *window, 1);
    manager.selectWorkspace(window->windowId(), 0);
    REQUIRE(window->displayedWorkspace() == shown);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    auto const shownRow = workspaceRowOf(*window, shown.value);
    auto const otherRow = workspaceRowOf(*window, other.value);
    auto const colorOfRow = [&](int row) {
        return window->sidebar()
            .index(row)
            .data(contour::window::SidebarModel::WorkspaceColorRole)
            .value<QColor>();
    };
    REQUIRE(colorOfRow(shownRow).alpha() == 0);

    // Only the action runs; the window's own notifications must bring the rows up to date.
    CHECK((*session)(contour::actions::SetWorkspaceColor { vtbackend::RGBColor { 0xC2, 0x54, 0x8F } }));
    QCoreApplication::processEvents();
    CHECK(colorOfRow(shownRow) == QColor { 0xC2, 0x54, 0x8F });
    CHECK(colorOfRow(otherRow).alpha() == 0);

    CHECK((*session)(contour::actions::ResetWorkspaceColor {}));
    QCoreApplication::processEvents();
    CHECK(colorOfRow(shownRow).alpha() == 0);

    // Without a colour, the binding opens the picker for the displayed workspace.
    auto picks = QSignalSpy { &*window, &contour::window::WindowController::workspaceColorPickRequested };
    CHECK((*session)(contour::actions::SetWorkspaceColor {}));
    REQUIRE(picks.count() == 1);
    CHECK(picks.at(0).at(0).toInt() == workspaceRowOf(*window, shown.value));
    CHECK(pickedRow(picks.at(0)) == window->sidebar().rows()[static_cast<std::size_t>(shownRow)].key);
}

namespace
{
/// The rows of the submenu titled @p title among @p rows.
[[nodiscard]] QVariantList submenuRows(QVariantList const& rows, QString const& title)
{
    for (auto const& value: rows)
    {
        auto const row = value.toMap();
        if (row["kind"].toString() == "submenu" && row["title"].toString() == title)
            return row["children"].toList();
    }
    return {};
}
} // namespace

TEST_CASE("the workspace menu's Workspace Color colours its workspace without showing it",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    std::ignore = contour::test::createSessionTabs(manager, *window, 1);
    auto const shown = window->displayedWorkspace();
    auto const other = manager.createWorkspaceIn(window->windowId());
    manager.selectWorkspace(window->windowId(), 1);
    std::ignore = contour::test::createSessionTabs(manager, *window, 1);
    manager.selectWorkspace(window->windowId(), 0);
    REQUIRE(window->displayedWorkspace() == shown);

    auto const otherRow = [&] {
        return workspaceRowOf(*window, other.value);
    };
    auto const colorOfOther = [&] {
        return window->sidebar()
            .index(otherRow())
            .data(contour::window::SidebarModel::WorkspaceColorRole)
            .value<QColor>();
    };
    auto const colourRows = [&] {
        window->sidebar().openContextMenu(otherRow(), QPointF {});
        return submenuRows(window->sidebarContextMenuModel(), "Workspace Color");
    };
    REQUIRE(colorOfOther().alpha() == 0);

    // The first swatch: the palette's first colour.
    auto const palette = window->tabColorPalette();
    REQUIRE_FALSE(palette.isEmpty());
    auto const rows = colourRows();
    REQUIRE_FALSE(rows.isEmpty());
    // The swatches come as one row of them, the menu's swatch grid.
    auto const swatchRow = rows.front().toMap();
    REQUIRE(swatchRow["kind"].toString() == "swatches");
    auto const swatches = swatchRow["swatches"].toList();
    REQUIRE(swatches.size() == palette.size());
    auto const swatch = swatches.front().toMap();
    CHECK(swatch["color"].value<QColor>() == palette.front().value<QColor>());
    window->triggerSidebarContextMenuAction(swatch["actionId"].toInt());
    QCoreApplication::processEvents();
    CHECK(colorOfOther() == palette.front().value<QColor>());
    CHECK(window->displayedWorkspace() == shown);
    CHECK_FALSE(window->rowActionPending());

    SECTION("Reset clears it, still without showing it")
    {
        auto const reset = actionIdOf(colourRows(), "Reset");
        REQUIRE(reset.has_value());
        window->triggerSidebarContextMenuAction(*reset);
        QCoreApplication::processEvents();
        CHECK(colorOfOther().alpha() == 0);
        CHECK(window->displayedWorkspace() == shown);
    }
    SECTION("Custom… asks for the picker over that workspace")
    {
        auto picks = QSignalSpy { &*window, &contour::window::WindowController::workspaceColorPickRequested };
        auto const custom = actionIdOf(colourRows(), "Custom…");
        REQUIRE(custom.has_value());
        window->triggerSidebarContextMenuAction(*custom);
        REQUIRE(picks.count() == 1);
        CHECK(picks.at(0).at(0).toInt() == otherRow());
        CHECK(pickedRow(picks.at(0)).workspace == other.value);
        CHECK(window->displayedWorkspace() == shown);
    }
    SECTION("the picker's choice and its reset colour the row it was opened for")
    {
        window->setRowColor(rowKeyAt(*window, otherRow()), QColor { 0x12, 0x34, 0x56 });
        QCoreApplication::processEvents();
        CHECK(colorOfOther() == QColor { 0x12, 0x34, 0x56 });
        window->resetRowColor(rowKeyAt(*window, otherRow()));
        QCoreApplication::processEvents();
        CHECK(colorOfOther().alpha() == 0);
        CHECK(window->displayedWorkspace() == shown);
    }
}

TEST_CASE("a sidebar tab menu's Tab Color colours its own tab, not the focused one",
          "[contour][sidebar][controller][tabcolor]")
{
    // Whether or not a tab strip is drawn to hang its own picker from.
    auto const placement = GENERATE(TabPlacement::Top, TabPlacement::Sidebar);
    INFO("tab placement " << static_cast<int>(placement));
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    app.app().config().tabPlacement = placement;
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    auto const made = contour::test::createSessionTabs(manager, *window, 2);
    auto const activeTab = [&] {
        return manager.model().workspace(window->displayedWorkspace())->activeTab()->id();
    };
    // The window shows the second tab; the menu is opened on the first, as a sidebar's usually is.
    auto const right = made.tabs.at(0);
    auto const focused = made.tabs.at(1);
    REQUIRE(activeTab() == focused);
    auto const colourRows = [&] {
        window->sidebar().openContextMenu(tabRowOf(*window, right.value), QPointF {});
        return submenuRows(window->sidebarContextMenuModel(), "Tab Color");
    };
    auto const userColorOf = [&](vtworkspace::TabId tab) {
        return manager.model().findTab(tab)->color(vtworkspace::TabColorSource::User);
    };
    auto picks = QSignalSpy { &*window, &contour::window::WindowController::sidebarTabColorPickRequested };
    auto stripPicks = QSignalSpy { &*window, &contour::window::WindowController::tabColorPickRequested };

    // Without a colour of the user's, there is nothing to reset.
    REQUIRE_FALSE(menuRowOf(colourRows(), "Reset").isEmpty());
    CHECK_FALSE(menuRowOf(colourRows(), "Reset")["enabled"].toBool());

    SECTION("a swatch colours that tab and leaves the focus where it is")
    {
        auto const palette = window->tabColorPalette();
        REQUIRE_FALSE(palette.isEmpty());
        auto const swatchRow = colourRows().front().toMap();
        REQUIRE(swatchRow["kind"].toString() == "swatches");
        auto const swatch = swatchRow["swatches"].toList().front().toMap();
        window->triggerSidebarContextMenuAction(swatch["actionId"].toInt());
        QCoreApplication::processEvents();
        CHECK(userColorOf(right) == contour::platform::toRGBColor(palette.front().value<QColor>()));
        CHECK_FALSE(userColorOf(focused).has_value());
        CHECK(activeTab() == focused);
        CHECK_FALSE(window->rowActionPending());
    }
    SECTION("Reset is offered once the tab has a colour, and clears that tab's only")
    {
        manager.model().setTabColor(
            right, vtworkspace::TabColorSource::User, vtbackend::RGBColor { 1, 2, 3 });
        manager.model().setTabColor(
            focused, vtworkspace::TabColorSource::User, vtbackend::RGBColor { 4, 5, 6 });
        auto const reset = menuRowOf(colourRows(), "Reset");
        REQUIRE(reset["enabled"].toBool());
        window->triggerSidebarContextMenuAction(reset["actionId"].toInt());
        QCoreApplication::processEvents();
        CHECK_FALSE(userColorOf(right).has_value());
        CHECK(userColorOf(focused) == vtbackend::RGBColor { 4, 5, 6 });
        CHECK(activeTab() == focused);
    }
    SECTION("Custom… asks for the picker over that tab's row")
    {
        auto const custom = actionIdOf(colourRows(), "Custom…");
        REQUIRE(custom.has_value());
        window->triggerSidebarContextMenuAction(*custom);
        REQUIRE(picks.count() == 1);
        CHECK(picks.at(0).at(0).toInt() == tabRowOf(*window, right.value));
        CHECK(pickedRow(picks.at(0)).tab == right.value);
        CHECK(stripPicks.count() == 0); // not the tab strip's picker, which a sidebar placement does not draw
        CHECK(activeTab() == focused);
    }
    SECTION("the picker's choice and its reset colour the tab row it was opened for")
    {
        window->setRowColor(rowKeyAt(*window, tabRowOf(*window, right.value)), QColor { 0x12, 0x34, 0x56 });
        CHECK(userColorOf(right) == vtbackend::RGBColor { 0x12, 0x34, 0x56 });
        CHECK_FALSE(userColorOf(focused).has_value());
        window->resetRowColor(rowKeyAt(*window, tabRowOf(*window, right.value)));
        CHECK_FALSE(userColorOf(right).has_value());
        CHECK(activeTab() == focused);
    }
    SECTION("the picker colours none of another window's tabs")
    {
        auto const other =
            contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
        auto const theirs = contour::test::createSessionTabs(manager, *other, 1).tabs.at(0);
        manager.model().setTabColor(
            theirs, vtworkspace::TabColorSource::User, vtbackend::RGBColor { 4, 5, 6 });
        auto const theirsHere = QVariant::fromValue(
            window->sidebar().ownRowKey(contour::window::SidebarRowKind::Tab, 0, theirs.value));
        window->setRowColor(theirsHere, QColor { 0x12, 0x34, 0x56 });
        CHECK(userColorOf(theirs) == vtbackend::RGBColor { 4, 5, 6 });
        window->resetRowColor(theirsHere);
        CHECK(userColorOf(theirs) == vtbackend::RGBColor { 4, 5, 6 });
    }
}

TEST_CASE("SetTabColor without a colour opens the sidebar's picker where no tab strip is drawn",
          "[contour][sidebar][controller][tabcolor]")
{
    using contour::config::TabBarVisibility;
    // Where the tabs live, and whether the strip would be drawn at the top.
    auto const [placement, visibility] =
        GENERATE(std::pair { TabPlacement::Top, TabBarVisibility::Always },
                 std::pair { TabPlacement::Top, TabBarVisibility::Never },
                 std::pair { TabPlacement::Sidebar, TabBarVisibility::Always });
    INFO("tab placement " << static_cast<int>(placement) << ", tab bar " << static_cast<int>(visibility));
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    app.app().config().tabPlacement = placement;
    auto& manager = app.manager();
    auto const window =
        contour::test::ScopedController { manager, contour::config::MenuPresentation::InScene };
    window->setTabBarVisibility(visibility);
    auto const made = contour::test::createSessionTabs(manager, *window, 2);
    auto const focused = made.tabs.at(1);
    auto picks = QSignalSpy { &*window, &contour::window::WindowController::sidebarTabColorPickRequested };
    auto stripPicks = QSignalSpy { &*window, &contour::window::WindowController::tabColorPickRequested };

    // What the SetTabColor action naming no colour does, from a key binding, the palette or the menu bar.
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    CHECK((*session)(contour::actions::SetTabColor {}));

    if (placement == TabPlacement::Top && visibility == TabBarVisibility::Always)
    {
        // The strip's own picker, over the active tab.
        CHECK(stripPicks.count() == 1);
        CHECK(picks.count() == 0);
    }
    else
    {
        CHECK(stripPicks.count() == 0);
        REQUIRE(picks.count() == 1);
        CHECK(picks.at(0).at(0).toInt() == tabRowOf(*window, focused.value));
        CHECK(pickedRow(picks.at(0)).tab == focused.value);
    }
}

// {{{ The switcher over a window: the real WindowController hosting it, through the real presenter.

namespace
{

/// @return The window's own workspace @p id, named @p name.
[[nodiscard]] contour::window::SwitcherWorkspace ownWorkspace(uint64_t id, std::string name)
{
    return contour::window::SwitcherWorkspace { .ref = {},
                                                .position = static_cast<uint32_t>(id),
                                                .name = std::move(name),
                                                .tabs = 1,
                                                .state = contour::window::SwitcherWorkspaceState::Hidden,
                                                .status = {},
                                                .id = id };
}

/// What the workspace chip's dropdown opens with over @p window: every daemon, its own first.
[[nodiscard]] contour::window::SwitcherOpening chipOpening(vtworkspace::WorkspaceId window)
{
    return contour::window::SwitcherOpening {
        .window = window,
        .windowDaemon = contour::session::SwitchTarget { .kind = vthost::proto::SwitchDaemonKind::Name,
                                                         .daemon = "work",
                                                         .workspace = {} },
        .mode = contour::session::SwitchMode::Switch,
        .prompt = contour::window::SwitcherPrompt::Browse,
        .pick = std::nullopt,
        .scope = contour::window::SwitcherScope::Everything,
        .windowWorkspaces = { ownWorkspace(1, "contour") },
        .origin = contour::window::SwitcherOrigin::Chip,
    };
}

} // namespace

TEST_CASE("a window hosts the switcher: the chip's dropdown opens, shows the sidebar's rows, and closes",
          "[contour][sidebar][controller][switcher]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto window = contour::test::ScopedControllerWithOsWindow { app.manager() };
    window->bindWindow(window.osWindow.get());
    window.osWindow->show();
    REQUIRE(window->sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    REQUIRE(window->sidebar().presence() == SidebarPresence::Hidden);

    auto switcher = contour::window::SwitcherController { contour::window::SwitcherServices {} };
    auto presenter = contour::window::SwitcherPresenter { switcher };
    auto changed = QSignalSpy { &*window, &contour::window::WindowController::switcherChanged };
    auto withdrawn = QSignalSpy { &*window, &contour::window::WindowController::switcherWithdrawn };
    auto cancelled = QSignalSpy { &switcher, &contour::window::SwitcherController::cancelled };

    presenter.open(*window, chipOpening(window.id), QRectF { 10, 0, 80, 24 });
    CHECK(window->presentedSwitcher() == &switcher);
    CHECK(window->isWorkspaceDropdownOpen());
    CHECK(window->chip()->accessibleDescription() == QStringLiteral("expanded"));
    CHECK(changed.count() == 1);
    // The dropdown shows the sidebar's running times, so the hidden sidebar's rows are kept live.
    CHECK(window->sidebar().presence() == SidebarPresence::Shown);

    SECTION("a choice or a cancel withdraws it")
    {
        switcher.cancel();
        CHECK(window->presentedSwitcher() == nullptr);
        CHECK_FALSE(window->isWorkspaceDropdownOpen());
        CHECK(window->chip()->accessibleDescription() == QStringLiteral("collapsed"));
        CHECK(withdrawn.count() == 1);
        CHECK(window->sidebar().presence() == SidebarPresence::Hidden);
    }
    SECTION("a click outside closes it, as a cancel")
    {
        window->closeSwitcher();
        CHECK(cancelled.count() == 1);
        CHECK(window->presentedSwitcher() == nullptr);
        CHECK_FALSE(window->isWorkspaceDropdownOpen());
        CHECK(window->sidebar().presence() == SidebarPresence::Hidden);
        // Closed already: a second close does nothing.
        window->closeSwitcher();
        CHECK(cancelled.count() == 1);
    }
    SECTION("centred, it is no dropdown")
    {
        presenter.open(*window, chipOpening(window.id), std::nullopt);
        CHECK(window->presentedSwitcher() == &switcher);
        CHECK_FALSE(window->isWorkspaceDropdownOpen());
        CHECK(window->sidebar().presence() == SidebarPresence::Shown);
    }
}

TEST_CASE("the switcher follows its window's sidebar while it is presented there, and no longer",
          "[contour][sidebar][controller][switcher]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto window = contour::test::ScopedController { app.manager() };
    auto switcher = contour::window::SwitcherController { contour::window::SwitcherServices {} };
    auto presenter = contour::window::SwitcherPresenter { switcher };
    auto reads = 0;
    auto const follow = [&] {
        presenter.followSidebar(window->sidebar(), [&] {
            ++reads;
            return std::vector { ownWorkspace(1, "contour"), ownWorkspace(2, "renamed") };
        });
    };

    presenter.open(*window, chipOpening(window.id), QRectF { 10, 0, 80, 24 });
    follow();
    emit window->sidebar().workspaceRowsChanged();
    CHECK(reads == 1);
    auto const titles = [&] {
        auto list = std::vector<std::string> {};
        for (auto const& row: switcher.rows())
            list.push_back(row.title);
        return list;
    };
    CHECK(std::ranges::contains(titles(), "renamed"));

    // Withdrawn, a rebuild is read for nobody.
    switcher.cancel();
    emit window->sidebar().workspaceRowsChanged();
    CHECK(reads == 1);

    // Opened again, it follows only once told to.
    presenter.open(*window, chipOpening(window.id), QRectF { 10, 0, 80, 24 });
    emit window->sidebar().workspaceRowsChanged();
    CHECK(reads == 1);
    follow();
    emit window->sidebar().workspaceRowsChanged();
    CHECK(reads == 2);

    // Not presented: nothing to follow.
    switcher.cancel();
    follow();
    emit window->sidebar().workspaceRowsChanged();
    CHECK(reads == 2);
}

// }}}
