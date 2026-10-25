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

#include <QtGui/QColor>

#include <catch2/catch_test_macros.hpp>

#include <memory>
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

[[nodiscard]] QColor colorAt(WindowController const& controller, int row)
{
    return controller.data(controller.index(row), static_cast<int>(WindowController::Roles::ColorRole))
        .value<QColor>();
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
