// SPDX-License-Identifier: Apache-2.0
//
// The tab actions the menus run, at the WindowController layer: each maps to the tab at a row (or to a
// TabId, resolved to its row), headless and without backing sessions -- tabs are minted straight through
// the vtworkspace model.

#include <contour/config/Actions.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Color.hpp>

#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QColor>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <ranges>
#include <tuple>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace
{

using contour::test::ScopedController;
using contour::test::TestApp;
namespace actions = contour::actions;
using contour::window::WindowController;

/// Mints @p n model tabs in @p controller's window, returning their ids in row order.
[[nodiscard]] std::vector<vtworkspace::TabId> createTabs(contour::session::TerminalSessionManager& manager,
                                                         WindowController const& controller,
                                                         int n)
{
    auto ids = std::vector<vtworkspace::TabId> {};
    for (auto const _: std::views::iota(0, n))
    {
        auto* tab = manager.model().createTab(controller.windowId());
        REQUIRE(tab != nullptr);
        ids.push_back(tab->id());
    }
    return ids;
}

/// Tabs that each have a real (mock-PTY) session: their ids and session ids in row order.
struct SessionTabs
{
    std::vector<vtworkspace::TabId> tabs {};
    std::vector<vtworkspace::SessionId> sessions {};
};

[[nodiscard]] SessionTabs createSessionTabs(contour::session::TerminalSessionManager& manager,
                                            WindowController& controller,
                                            int n)
{
    auto result = SessionTabs {};
    for (auto const _: std::views::iota(0, n))
    {
        controller.createNewTab();
        auto const* tab = manager.model().window(controller.windowId())->activeTab();
        REQUIRE(tab != nullptr);
        REQUIRE(controller.activeSession() != nullptr);
        result.tabs.push_back(tab->id());
        result.sessions.push_back(controller.activeSession()->modelSessionId());
    }
    return result;
}

[[nodiscard]] QColor colorAt(WindowController const& controller, int row)
{
    return controller.data(controller.index(row), static_cast<int>(WindowController::Roles::ColorRole))
        .value<QColor>();
}

/// The `actionId` of the command row titled @p title in @p rows, searching submenus too.
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

TEST_CASE("CloseOtherTabs keeps the tab of the row it runs on", "[contour][windowmenus][tabaction]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    auto const ids = createTabs(app.manager(), *window, 3);
    REQUIRE(window->count() == 3);

    window->runTabAction(1, actions::Action { actions::CloseOtherTabs {} });

    REQUIRE(window->count() == 1);
    CHECK(app.manager().model().findTab(ids[0]) == nullptr);
    CHECK(app.manager().model().findTab(ids[1]) != nullptr);
    CHECK(app.manager().model().findTab(ids[2]) == nullptr);
}

TEST_CASE("CloseTabsToRight closes everything after the row", "[contour][windowmenus][tabaction]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 3);

    window->runTabAction(0, actions::Action { actions::CloseTabsToRight {} });

    CHECK(window->count() == 1);
}

TEST_CASE("SetTabTitle and SetTabColor ask the row's tab to edit", "[contour][windowmenus][tabaction]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 3);
    auto titleSpy = QSignalSpy { window.controller, &WindowController::tabTitleEditRequested };
    auto colorSpy = QSignalSpy { window.controller, &WindowController::tabColorPickRequested };

    window->runTabAction(2, actions::Action { actions::SetTabTitle {} });
    REQUIRE(titleSpy.count() == 1);
    CHECK(titleSpy.front().front().toInt() == 2);

    window->runTabAction(2, actions::Action { actions::SetTabColor {} });
    REQUIRE(colorSpy.count() == 1);
    CHECK(colorSpy.front().front().toInt() == 2);
}

TEST_CASE("SetTabColor with a color colours the row, and ResetTabColor clears it",
          "[contour][windowmenus][tabaction]")
{
    using namespace vtbackend;
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 2);

    window->runTabAction(1, actions::Action { actions::SetTabColor { .color = 0xFF0000_rgb } });
    CHECK(colorAt(*window, 1) == QColor(0xFF, 0x00, 0x00));
    CHECK(colorAt(*window, 0).alpha() == 0);

    window->runTabAction(1, actions::Action { actions::ResetTabColor {} });
    CHECK(colorAt(*window, 1).alpha() == 0);
}

TEST_CASE("MoveTabToNewWindow hands the row's tab to the next window", "[contour][windowmenus][tabaction]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 2);

    window->runTabAction(1, actions::Action { actions::MoveTabToNewWindow {} });

    ScopedController const second { app.manager() };
    CHECK(app.manager().consumePendingTransplant(second.controller));
    CHECK(second->count() == 1);
}

TEST_CASE("runTabAction by TabId resolves the row, and ignores a tab that has gone",
          "[contour][windowmenus][tabaction]")
{
    // Closing a tab ends its sessions, so these tabs have real (mock-PTY) ones.
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    ScopedController const window { app.manager() };
    auto ids = std::vector<vtworkspace::TabId> {};
    for (auto const _: std::views::iota(0, 3))
    {
        window->createNewTab();
        auto const* tab = app.manager().model().window(window.id)->activeTab();
        REQUIRE(tab != nullptr);
        ids.push_back(tab->id());
    }
    REQUIRE(window->count() == 3);

    // A session ends on its own thread, so the tab goes a moment after the action returns.
    window->runTabAction(ids[2], actions::Action { actions::CloseTab {} });
    QTRY_COMPARE_WITH_TIMEOUT(window->count(), 2, 5000);

    window->runTabAction(ids[2], actions::Action { actions::CloseTab {} });
    QTest::qWait(100);
    CHECK(window->count() == 2);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("showAbout asks the window to show it", "[contour][windowmenus][tabaction]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    auto spy = QSignalSpy { window.controller, &WindowController::aboutRequested };

    window->showAbout();

    CHECK(spy.count() == 1);
}

TEST_CASE("CloseOtherTabs ends the other tabs' sessions and keeps the anchor's",
          "[contour][windowmenus][tabaction]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    ScopedController const window { app.manager() };
    auto const made = createSessionTabs(app.manager(), *window, 3);
    REQUIRE(window->count() == 3);

    window->runTabAction(1, actions::Action { actions::CloseOtherTabs {} });

    QTRY_COMPARE_WITH_TIMEOUT(window->count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(app.manager().sessionForId(made.sessions[0]) == nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(app.manager().sessionForId(made.sessions[2]) == nullptr, 5000);
    CHECK(app.manager().sessionForId(made.sessions[1]) != nullptr);
    CHECK(app.manager().model().findTab(made.tabs[1]) != nullptr);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("CloseOtherTabs keeps the anchor when it is not the active tab",
          "[contour][windowmenus][tabaction]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    ScopedController const window { app.manager() };
    auto const made = createSessionTabs(app.manager(), *window, 3);

    // Make the active tab one that is NOT the anchor, whatever order the tabs were created in.
    window->activateTab(2);
    REQUIRE(app.manager().model().window(window.id)->activeTab()->id() == made.tabs[2]);

    window->runTabAction(0, actions::Action { actions::CloseOtherTabs {} });

    QTRY_COMPARE_WITH_TIMEOUT(window->count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(app.manager().sessionForId(made.sessions[2]) == nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(app.manager().sessionForId(made.sessions[1]) == nullptr, 5000);
    CHECK(app.manager().sessionForId(made.sessions[0]) != nullptr);
    CHECK(app.manager().model().findTab(made.tabs[0]) != nullptr);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("CloseTabsToRight on the last tab closes nothing", "[contour][windowmenus][tabaction]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    ScopedController const window { app.manager() };
    auto const made = createSessionTabs(app.manager(), *window, 3);

    window->runTabAction(2, actions::Action { actions::CloseTabsToRight {} });
    QTest::qWait(200);

    CHECK(window->count() == 3);
    for (auto const session: made.sessions)
        CHECK(app.manager().sessionForId(session) != nullptr);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("the tab menu is built for the right-clicked tab", "[contour][windowmenus]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    auto const ids = createTabs(app.manager(), *window, 3);
    window->activateTab(0);
    auto requested = QSignalSpy { window.controller, &WindowController::tabContextMenuRequested };

    window->openTabContextMenu(2, {});

    REQUIRE(requested.count() == 1);
    CHECK(requested.front().front().toInt() == 2);
    REQUIRE_FALSE(window->tabContextMenuModel().isEmpty());

    auto const closeOthers = actionIdOf(window->tabContextMenuModel(), QStringLiteral("Close Other Tabs"));
    REQUIRE(closeOthers.has_value());
    window->triggerTabContextMenuAction(*closeOthers);

    REQUIRE(window->count() == 1);
    CHECK(app.manager().model().findTab(ids[2]) != nullptr);
    CHECK(app.manager().model().findTab(ids[0]) == nullptr);
}

TEST_CASE("a pick against a tab that has gone is a no-op", "[contour][windowmenus]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 3);

    window->openTabContextMenu(2, {});
    window->closeTabsToRight(0);
    REQUIRE(window->count() == 1);

    auto const closeTab = actionIdOf(window->tabContextMenuModel(), QStringLiteral("Close Tab"));
    REQUIRE(closeTab.has_value());
    window->triggerTabContextMenuAction(*closeTab);

    CHECK(window->count() == 1);
}

TEST_CASE("Rename… names the right-clicked row", "[contour][windowmenus]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 3);
    auto titleSpy = QSignalSpy { window.controller, &WindowController::tabTitleEditRequested };

    window->openTabContextMenu(1, {});
    auto const rename = actionIdOf(window->tabContextMenuModel(), QStringLiteral("Rename…"));
    REQUIRE(rename.has_value());
    window->triggerTabContextMenuAction(*rename);

    REQUIRE(titleSpy.count() == 1);
    CHECK(titleSpy.front().front().toInt() == 1);
}

TEST_CASE("the new-tab menu lists the profiles", "[contour][windowmenus]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    auto requested = QSignalSpy { window.controller, &WindowController::newTabMenuRequested };

    window->openNewTabMenu({});

    CHECK(requested.count() == 1);
    auto const model = window->newTabMenuModel();
    REQUIRE_FALSE(model.isEmpty());
    CHECK(model.back().toMap()["title"].toString() == QStringLiteral("Settings"));
}

TEST_CASE("the pane and title-bar menus still pop in the scene", "[contour][windowmenus]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    auto requested = QSignalSpy { window.controller, &WindowController::titleBarContextMenuRequested };

    window->openTitleBarContextMenu();

    CHECK(requested.count() == 1);
}

TEST_CASE("the menu bar sees this window's tabs", "[contour][windowmenus][menubar]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 3);

    auto const state = window->menuBarState();

    CHECK(state.window == contour::command::WindowPresence::Present);
    CHECK(state.tabCount == 3);
    CHECK(state.activeTab == window->activeTabIndex());
    CHECK(state.tabTitles == window->tabTitles());
    auto const* tab = app.manager().model().window(window->windowId())->activeTab();
    REQUIRE(tab != nullptr);
    CHECK(state.paneCount == tab->paneCount());
}

TEST_CASE("a menu bar action without a session does nothing", "[contour][windowmenus][menubar]")
{
    TestApp app;
    ScopedController const window { app.manager() };
    std::ignore = createTabs(app.manager(), *window, 2);
    REQUIRE(window->activeSession() == nullptr);

    window->runMenuBarAction(actions::Action { actions::CloseTab {} });

    CHECK(window->count() == 2);
}

TEST_CASE("the menu bar's Minimize and Zoom reach the window", "[contour][windowmenus][menubar]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedControllerWithOsWindow const window { app.manager() };
    window->bindWindow(window.osWindow.get());
    std::ignore = createSessionTabs(app.manager(), *window, 1);
    REQUIRE(window->activeSession() != nullptr);
    REQUIRE_FALSE(window.osWindow->windowStates().testFlag(Qt::WindowMaximized));

    // Through the menu bar's path: the active session runs the action, and the window acts on it.
    window->runMenuBarAction(actions::Action { actions::ZoomWindow {} });
    QTRY_VERIFY_WITH_TIMEOUT(window.osWindow->windowStates().testFlag(Qt::WindowMaximized), 5000);

    window->runMenuBarAction(actions::Action { actions::ZoomWindow {} });
    QTRY_VERIFY_WITH_TIMEOUT(!window.osWindow->windowStates().testFlag(Qt::WindowMaximized), 5000);

    window->runMenuBarAction(actions::Action { actions::MinimizeWindow {} });
    QTRY_VERIFY_WITH_TIMEOUT(window->isMinimized(), 5000);

    contour::test::closeAllTabs(*window);
}
