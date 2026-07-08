// SPDX-License-Identifier: Apache-2.0
//
// Multi-window coverage at the manager + WindowController layer, headless (offscreen QGuiApplication
// from test_main, no PTY: tabs are minted straight through the vtmux model, which never spawns a
// backing session).
//
// The headline cases are REGRESSION PINS for the WindowId-routing refactor: before it, the manager
// resolved every tab/pane write through the legacy single-window pointer, so a tab activation, move,
// bulk close, rename, or pane-ratio drag issued from a SECOND OS window silently operated on the
// FIRST window's model. Each pin drives the operation through window B's controller and asserts A is
// untouched.

#include <contour/ContourGuiApp.h>
#include <contour/PaneProxy.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>
#include <contour/test/GuiTestFixtures.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

namespace
{

using contour::test::ScopedController;
using contour::test::TestApp;

/// Mints @p n model tabs in the controller's window (no backing sessions).
void createTabs(contour::TerminalSessionManager& manager, contour::WindowController& controller, int n)
{
    for (int i = 0; i < n; ++i)
        REQUIRE(manager.model().createTab(controller.windowId()) != nullptr);
}

[[nodiscard]] QString rawTitleAt(contour::WindowController& controller, int row)
{
    return controller.data(controller.index(row), contour::WindowController::RawTitleRole).toString();
}

} // namespace

TEST_CASE("newWindow stages the spawn screen; takePendingSpawnScreen consumes it exactly once",
          "[contour][multiwindow]")
{
    TestApp app;
    auto* screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);

    // Stage (the QML engine is not running in this test; newWindow skips the window load then).
    app.app().newWindow(screen);

    // Consume-once contract: the first take yields the staged screen, any further take yields null
    // (a later window spawn without an explicit screen must not inherit a stale one).
    CHECK(app.app().takePendingSpawnScreen() == screen);
    CHECK(app.app().takePendingSpawnScreen() == nullptr);
}

TEST_CASE("two controllers adapt distinct model windows with independent tab rows", "[contour][multiwindow]")
{
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    REQUIRE(windowA.controller != nullptr);
    REQUIRE(windowB.controller != nullptr);
    CHECK(windowA.id != windowB.id);
    CHECK(manager.controllerFor(windowA.id) == windowA.controller);
    CHECK(manager.controllerFor(windowB.id) == windowB.controller);

    createTabs(manager, *windowA, 2);
    createTabs(manager, *windowB, 3);
    CHECK(windowA->count() == 2);
    CHECK(windowB->count() == 3);
}

TEST_CASE("tab strip Multiple gate tracks the live tab count and re-notifies QML", "[contour][multiwindow]")
{
    using contour::config::TabBarVisibility;

    // Model-only tabs (createTab / closeTabsToRight): they drive the same onTabAdded / onTabClosed
    // hooks synchronously without needing a backing session (closeTabAtIndex on a session-less tab is
    // an async session-teardown path, so it is not used here).
    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };
    REQUIRE(window.controller != nullptr);

    window->seedTabBarVisibility(TabBarVisibility::Multiple);
    // The seed itself emits the change once (mode is one input to the resolved gate). Spy AFTER seeding
    // so the counts below reflect only the count-driven add/close notifications.
    QSignalSpy shouldShowSpy(window.controller, &contour::WindowController::tabBarShouldShowChanged);

    // One tab: Multiple hides the strip.
    createTabs(manager, *window, 1);
    CHECK(window->count() == 1);
    CHECK_FALSE(window->tabBarShouldShow());

    // Second tab: the strip shows, and the add fired the gate notification.
    createTabs(manager, *window, 1);
    CHECK(window->count() == 2);
    CHECK(window->tabBarShouldShow());

    // Back to one tab: the strip hides again, and the close fired the notification too.
    window->closeTabsToRight(0);
    CHECK(window->count() == 1);
    CHECK_FALSE(window->tabBarShouldShow());

    // Two adds + one close each drove tabBarShouldShowChanged (the onTabAdded / onTabClosed wiring);
    // without those emits the QML `visible` binding would go stale.
    CHECK(shouldShowSpy.count() == 3);
}

TEST_CASE("tab strip seeds are first-write-wins per window", "[contour][multiwindow]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };
    REQUIRE(window.controller != nullptr);

    // Defaults before any seed (mirror the ConfigEntry defaults: Top / Always).
    CHECK(window->tabBarPosition() == static_cast<int>(TabBarPosition::Top));
    CHECK(window->tabBarVisibility() == static_cast<int>(TabBarVisibility::Always));

    // First seed takes effect...
    window->seedTabBarPosition(TabBarPosition::Bottom);
    window->seedTabBarVisibility(TabBarVisibility::Never);
    CHECK(window->tabBarPosition() == static_cast<int>(TabBarPosition::Bottom));
    CHECK(window->tabBarVisibility() == static_cast<int>(TabBarVisibility::Never));

    // ...a later seed (arriving on every session rebind in production) is a no-op, so a runtime state
    // is never reset by a tab switch or split collapse.
    window->seedTabBarPosition(TabBarPosition::Top);
    window->seedTabBarVisibility(TabBarVisibility::Always);
    CHECK(window->tabBarPosition() == static_cast<int>(TabBarPosition::Bottom));
    CHECK(window->tabBarVisibility() == static_cast<int>(TabBarVisibility::Never));
}

TEST_CASE("tab strip Always/Never gates ignore the tab count", "[contour][multiwindow]")
{
    using contour::config::TabBarVisibility;

    TestApp app;
    auto& manager = app.manager();

    SECTION("Always -> shown even with zero or one tab")
    {
        ScopedController window { manager };
        window->seedTabBarVisibility(TabBarVisibility::Always);
        CHECK(window->tabBarShouldShow()); // no tabs yet
        createTabs(manager, *window, 1);
        CHECK(window->tabBarShouldShow()); // one tab
    }

    SECTION("Never -> hidden even with multiple tabs")
    {
        ScopedController window { manager };
        window->seedTabBarVisibility(TabBarVisibility::Never);
        createTabs(manager, *window, 3);
        CHECK_FALSE(window->tabBarShouldShow());
    }
}

TEST_CASE("REGRESSION: tab operations from a second window target that window, not the first",
          "[contour][multiwindow]")
{
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    createTabs(manager, *windowA, 2);
    createTabs(manager, *windowB, 3);

    SECTION("activateTab changes only window B's active tab")
    {
        auto const activeInA = windowA->activeTabIndex();
        windowB->activateTab(1);
        CHECK(windowB->activeTabIndex() == 1);
        CHECK(windowA->activeTabIndex() == activeInA);
    }

    SECTION("setTabTitle renames the row in window B only")
    {
        windowB->setTabTitle(0, QStringLiteral("renamed-in-B"));
        CHECK(rawTitleAt(*windowB, 0) == QStringLiteral("renamed-in-B"));
        CHECK(rawTitleAt(*windowA, 0).isEmpty());

        windowB->resetTabTitle(0);
        CHECK(rawTitleAt(*windowB, 0).isEmpty());
    }

    SECTION("moveTab reorders window B's rows and leaves window A's order alone")
    {
        // Tag B's rows through the model so the order is observable, and A's likewise.
        windowB->setTabTitle(0, QStringLiteral("b0"));
        windowB->setTabTitle(1, QStringLiteral("b1"));
        windowB->setTabTitle(2, QStringLiteral("b2"));
        windowA->setTabTitle(0, QStringLiteral("a0"));
        windowA->setTabTitle(1, QStringLiteral("a1"));

        windowB->moveTab(0, 2);
        CHECK(rawTitleAt(*windowB, 0) == QStringLiteral("b1"));
        CHECK(rawTitleAt(*windowB, 2) == QStringLiteral("b0"));
        CHECK(rawTitleAt(*windowA, 0) == QStringLiteral("a0"));
        CHECK(rawTitleAt(*windowA, 1) == QStringLiteral("a1"));
    }

    SECTION("closeOtherTabs / closeTabsToRight prune window B only")
    {
        windowB->closeTabsToRight(0);
        CHECK(windowB->count() == 1);
        CHECK(windowA->count() == 2);

        createTabs(manager, *windowB, 2);
        windowB->closeOtherTabs(1);
        CHECK(windowB->count() == 1);
        CHECK(windowA->count() == 2);
    }
}

TEST_CASE("beginActiveTabTitleEdit requests the active tab's inline editor, per window",
          "[contour][multiwindow]")
{
    // The SetTabTitle action asks the acting window's WindowController to open the inline title
    // editor for its ACTIVE tab; the controller signals tabTitleEditRequested(activeTabIndex) so
    // the matching QML TabItem starts editing. Verify the signal fires with the active tab's row,
    // and only on the window whose controller was asked (no cross-window leakage).
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    createTabs(manager, *windowA, 2);
    createTabs(manager, *windowB, 3);

    windowB->activateTab(2);
    REQUIRE(windowB->activeTabIndex() == 2);

    auto spyB = QSignalSpy(windowB.controller, &contour::WindowController::tabTitleEditRequested);
    auto spyA = QSignalSpy(windowA.controller, &contour::WindowController::tabTitleEditRequested);

    windowB->beginActiveTabTitleEdit();

    REQUIRE(spyB.count() == 1);
    CHECK(spyB.takeFirst().at(0).toInt() == 2); // window B's active tab row
    CHECK(spyA.count() == 0);                   // window A's controller was not asked
}

TEST_CASE("beginActiveTabTitleEdit is a no-op when the window has no tabs", "[contour][multiwindow]")
{
    // Guard: with no active tab (activeTabIndex() < 0) the controller must not emit, so the QML
    // editor is never asked to open on a nonexistent tab.
    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };
    REQUIRE(window->activeTabIndex() < 0);

    auto spy = QSignalSpy(window.controller, &contour::WindowController::tabTitleEditRequested);
    window->beginActiveTabTitleEdit();
    CHECK(spy.count() == 0);
}

TEST_CASE("REGRESSION: pane-proxy writes (ratio, activate) target their own window's tab",
          "[contour][multiwindow][split]")
{
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    createTabs(manager, *windowA, 1);
    createTabs(manager, *windowB, 1);

    // Split BOTH windows' tabs so a mis-routed write would visibly land on A.
    auto* tabA = manager.model().window(windowA.id)->activeTab();
    auto* tabB = manager.model().window(windowB.id)->activeTab();
    REQUIRE(tabA != nullptr);
    REQUIRE(tabB != nullptr);
    REQUIRE(manager.model().splitActivePane(tabA->id(), vtmux::SplitState::Vertical) != nullptr);
    REQUIRE(manager.model().splitActivePane(tabB->id(), vtmux::SplitState::Vertical) != nullptr);
    REQUIRE(tabA->rootPane()->ratio() == Catch::Approx(0.5));
    REQUIRE(tabB->rootPane()->ratio() == Catch::Approx(0.5));

    auto* rootProxyB = windowB->activeTabRootPane();
    REQUIRE(rootProxyB != nullptr);
    REQUIRE_FALSE(rootProxyB->isLeaf());

    SECTION("a divider drag in window B moves window B's ratio only")
    {
        rootProxyB->setRatio(0.25);
        CHECK(tabB->rootPane()->ratio() == Catch::Approx(0.25));
        CHECK(tabA->rootPane()->ratio() == Catch::Approx(0.5));
    }

    SECTION("activating a pane in window B changes window B's active pane only")
    {
        auto* second = rootProxyB->second();
        REQUIRE(second != nullptr);
        auto const activeInA = tabA->activePane()->id();

        second->activate();
        CHECK(tabB->activePane()->id() == second->modelId());
        CHECK(tabA->activePane()->id() == activeInA);
        CHECK(second->active());
    }
}

TEST_CASE("removeWindowController prunes exactly the closed window's controller",
          "[contour][multiwindow][close]")
{
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    createTabs(manager, *windowB, 1);

    // Not-last: A's controller unregisters; B keeps working (its rows survive).
    manager.removeWindowController(windowA.id);
    CHECK(manager.controllerFor(windowA.id) == nullptr);
    CHECK(manager.controllerFor(windowB.id) == windowB.controller);
    CHECK(windowB->count() == 1);
    // With no backing sessions, B's window may close at any time.
    CHECK(windowB->canCloseWindow());

    // Last: B unregisters too; the registry is empty.
    manager.removeWindowController(windowB.id);
    CHECK(manager.controllerFor(windowB.id) == nullptr);

    // Unknown ids are a no-op (idempotent close path; the guards replay it once more on scope exit).
    manager.removeWindowController(windowA.id);
}

TEST_CASE("WindowController projects real-session tabs and routes rename/color to the model",
          "[contour][multiwindow][model]")
{
    // Drives the REAL WindowController QAbstractListModel surface (data/roleNames/resolvedTabLabel/
    // rowCount) over tabs backed by REAL sessions created through the injected MockPtySessionFactory —
    // the projection that TabListModel_test can only exercise through a hand-copied surrogate.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    window->createNewTab();
    REQUIRE(factory->createdPtys.size() == 2);
    REQUIRE(window->count() == 2);

    // roleNames exposes every projected role name.
    auto const roles = window->roleNames();
    CHECK(roles.contains(contour::WindowController::TitleRole));
    CHECK(roles.contains(contour::WindowController::RawTitleRole));
    CHECK(roles.contains(contour::WindowController::IsActiveRole));

    // A never-renamed tab has an empty raw title but a resolved (template-expanded) title.
    CHECK(rawTitleAt(*window, 0).isEmpty());
    CHECK_NOTHROW(window->data(window->index(0), contour::WindowController::TitleRole).toString());

    // Rename routes to the model and surfaces through RawTitleRole; reset restores the default.
    window->setTabTitle(0, QStringLiteral("deploy"));
    CHECK(rawTitleAt(*window, 0) == "deploy");
    window->resetTabTitle(0);
    CHECK(rawTitleAt(*window, 0).isEmpty());

    // An empty rename resets rather than blanking (the vtmux-layer invariant, via the controller).
    window->setTabTitle(0, QStringLiteral("x"));
    window->setTabTitle(0, QString {});
    CHECK(rawTitleAt(*window, 0).isEmpty());

    // Color set/reset route to the model without throwing.
    CHECK_NOTHROW(window->setTabColor(0, QColor(Qt::red)));
    CHECK_NOTHROW(window->resetTabColor(0));

    // Active-index projection: activating row 0 makes it the active tab.
    window->activateTab(0);
    CHECK(window->activeTabIndex() == 0);

    // Clean up the backing sessions.
    for (int row = window->count() - 1; row >= 0; --row)
        window->closeTabAtIndex(row);
}

TEST_CASE("a background (unfocused) tab's title updates in real time", "[contour][multiwindow][model]")
{
    // Regression: a program-driven title change on a tab that is NOT the active tab (so its session
    // has no display attached) must still refresh that tab's strip label immediately — not only once
    // the tab is next focused. The refresh is posted to the session QObject (GUI thread), so it works
    // without a display; pump the event loop to deliver the queued post.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    window->createNewTab();
    REQUIRE(window->count() == 2);

    // Make row 0 the active tab, so row 1 is the background (display-less) tab under test.
    window->activateTab(0);
    REQUIRE(window->activeTabIndex() == 0);

    auto* backgroundTab = manager.model().window(window.id)->tabAt(1);
    REQUIRE(backgroundTab != nullptr);
    auto* backgroundSession = manager.sessionForId(backgroundTab->activePane()->session());
    REQUIRE(backgroundSession != nullptr);

    // Watch the tab strip for a TitleRole change on row 1.
    auto spy = QSignalSpy(window.controller, &contour::WindowController::dataChanged);

    // Program-driven title change on the BACKGROUND session (as an OSC title sequence would do).
    backgroundSession->setTitle(QStringLiteral("background-build-running"));

    // The refresh is queued onto the session (GUI thread); deliver it.
    QCoreApplication::processEvents();

    // The background tab's resolved title now reflects the change...
    CHECK(window->data(window->index(1), contour::WindowController::TitleRole).toString()
          == QStringLiteral("background-build-running"));
    // ...and a dataChanged carrying TitleRole was emitted for row 1 (not row 0, the active tab).
    auto sawRow1TitleChange = false;
    for (auto const& emission: spy)
    {
        auto const topLeft = emission.at(0).toModelIndex();
        auto const roles = emission.at(2).value<QList<int>>();
        if (topLeft.row() == 1 && roles.contains(contour::WindowController::TitleRole))
            sawRow1TitleChange = true;
    }
    CHECK(sawRow1TitleChange);

    for (int row = window->count() - 1; row >= 0; --row)
        window->closeTabAtIndex(row);
}

TEST_CASE("WindowController routes bulk-close and move-tab to its own window",
          "[contour][multiwindow][model]")
{
    // Real controller + real sessions: exercises the closeOtherTabs/closeTabsToRight/moveTab
    // routing that only MultiWindow_test can drive against the live model.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    for (int i = 0; i < 4; ++i)
        window->createNewTab();
    REQUIRE(window->count() == 4);

    SECTION("closeTabsToRight leaves the anchor and everything to its left")
    {
        window->closeTabsToRight(1);
        CHECK(window->count() == 2);
    }

    SECTION("closeOtherTabs leaves only the kept tab")
    {
        window->closeOtherTabs(2);
        CHECK(window->count() == 1);
    }

    SECTION("moveTab reorders within the window")
    {
        window->setTabTitle(0, QStringLiteral("first"));
        window->moveTab(0, 3);
        CHECK(rawTitleAt(*window, 3) == "first");
        // Move-to-same-index is a silent no-op.
        window->moveTab(2, 2);
        CHECK(window->count() == 4);
    }

    // Drain remaining sessions.
    for (int row = window->count() - 1; row >= 0; --row)
        window->closeTabAtIndex(row);
}

TEST_CASE("WindowController owns window-scoped title-bar visibility", "[contour][multiwindow][titlebar]")
{
    // titleBarVisible is window state on the controller (not per display). Seeding is first-write-
    // wins; toggle flips it; a later seed does not clobber a runtime toggle.
    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };

    window->seedTitleBarVisible(true);
    CHECK(window->titleBarVisible());

    window->toggleTitleBar();
    CHECK_FALSE(window->titleBarVisible());

    // A subsequent seed (e.g. a session rebind re-applying the profile) must NOT override the toggle.
    window->seedTitleBarVisible(true);
    CHECK_FALSE(window->titleBarVisible());

    // Explicit set overrides.
    window->setTitleBarVisible(true);
    CHECK(window->titleBarVisible());
}

TEST_CASE("TerminalSessionManager splits, focuses and closes panes through the acting session",
          "[contour][multiwindow][split]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();

    auto* acting = window->activeSession();
    REQUIRE(acting != nullptr);
    auto* tab = manager.model().window(window.id)->activeTab();
    REQUIRE(tab != nullptr);
    REQUIRE(tab->rootPane()->isLeaf());

    // Split the active pane vertically through the manager (the keybinding entry point).
    manager.splitActivePane(/*vertical*/ true, acting);
    CHECK_FALSE(tab->rootPane()->isLeaf());

    // Focus movement across the split does not throw and keeps the tab consistent.
    CHECK_NOTHROW(manager.focusPane(vtmux::FocusDirection::Left, acting));
    CHECK_NOTHROW(manager.focusPane(vtmux::FocusDirection::Right, acting));

    // Closing the active pane collapses the split back to a single leaf.
    manager.closeActivePane(window->activeSession());
    CHECK(tab->rootPane()->isLeaf());

    window->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager swap/move/toggle/resize route through the acting session",
          "[contour][multiwindow][split]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();

    auto* acting = window->activeSession();
    REQUIRE(acting != nullptr);
    auto* tab = manager.model().window(window.id)->activeTab();
    REQUIRE(tab != nullptr);

    // Two vertical splits -> three side-by-side panes so every op has a neighbor to act on.
    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    REQUIRE(tab->paneCount() == 3);

    // Toggle the active pane's split orientation (H<->V): does not throw, keeps the pane count.
    CHECK_NOTHROW(manager.toggleActivePaneOrientation(window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Resize the active pane in each direction: no throw, pane count preserved (ratio nudge only).
    CHECK_NOTHROW(manager.resizeActivePane(vtmux::FocusDirection::Left, 0.05, window->activeSession()));
    CHECK_NOTHROW(manager.resizeActivePane(vtmux::FocusDirection::Right, 0.05, window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Swap the active pane with its left neighbor: pane count preserved (sessions trade slots).
    CHECK_NOTHROW(manager.swapPane(vtmux::FocusDirection::Left, window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Move (re-parent) the active pane: pane count preserved, tree still valid.
    CHECK_NOTHROW(manager.movePane(vtmux::FocusDirection::Left, window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Guard paths: a null acting session is a no-op, never a crash.
    CHECK_NOTHROW(manager.swapPane(vtmux::FocusDirection::Left, nullptr));
    CHECK_NOTHROW(manager.movePane(vtmux::FocusDirection::Left, nullptr));
    CHECK_NOTHROW(manager.toggleActivePaneOrientation(nullptr));
    CHECK_NOTHROW(manager.resizeActivePane(vtmux::FocusDirection::Left, 0.05, nullptr));

    window->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager moves a tab between windows, sessions surviving",
          "[contour][multiwindow][move]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    windowA->createNewTab();
    windowA->createNewTab(); // A: [t0, t1]
    windowB->createNewTab(); // B: [b0]
    REQUIRE(windowA->count() == 2);
    REQUIRE(windowB->count() == 1);

    // Record the session that backs A's active tab; it must still resolve after the move (survived).
    auto const ptyCountBefore = factory->createdPtys.size();
    auto* movedSession = windowA->activeSession();
    REQUIRE(movedSession != nullptr);
    auto const movedSessionId = movedSession->modelSessionId();

    // Drag A's last tab (row 1) into window B at row 0.
    manager.moveTabToWindow(windowA.id, 1, windowB.id, 0);

    CHECK(windowA->count() == 1); // A lost the tab
    CHECK(windowB->count() == 2); // B gained it
    // No new PTY was created — the session was transplanted, not recreated.
    CHECK(factory->createdPtys.size() == ptyCountBefore);
    // The moved session still resolves through the process-wide registry.
    CHECK(manager.sessionForId(movedSessionId) == movedSession);

    windowA->closeTabAtIndex(0);
    windowB->closeTabAtIndex(0);
    windowB->closeTabAtIndex(0);
}

TEST_CASE("moving a window's LAST tab into another window closes the now-empty source window",
          "[contour][multiwindow][move]")
{
    // Dragging the last tab out of a window and into another must close the now-empty source window —
    // an empty window with no tabs is not a valid state (regression from live testing).
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };

    // Bind a real OS window to A: closeWindow() must close THIS QQuickWindow too, otherwise its title
    // bar / tab strip lingers on screen as a ghost after the last tab is dragged away.
    QQuickWindow osWindowA;
    windowA->bindWindow(&osWindowA);
    osWindowA.show();
    QCoreApplication::processEvents();
    REQUIRE(osWindowA.isVisible());

    windowA->createNewTab(); // A: [t0]  (its only tab)
    windowB->createNewTab(); // B: [b0]
    REQUIRE(windowA->count() == 1);
    REQUIRE(windowB->count() == 1);
    REQUIRE(manager.controllerFor(windowA.id) != nullptr);

    // Drag A's only tab into window B.
    manager.moveTabToWindow(windowA.id, 0, windowB.id, 1);
    QCoreApplication::processEvents();

    // B now holds both tabs; A is empty and its window has been closed (model + controller + OS window).
    CHECK(windowB->count() == 2);
    CHECK(manager.model().window(windowA.id) == nullptr); // model window gone
    CHECK(manager.controllerFor(windowA.id) == nullptr);  // controller dropped
    CHECK_FALSE(osWindowA.isVisible());                   // the OS window is gone, not a ghost

    // Drain the deferred controller delete so LeakSanitizer sees a clean teardown; ScopedController's
    // own removeWindowController(windowA) is then an idempotent no-op.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

TEST_CASE("manager tab switching and moving route for the acting session", "[contour][multiwindow]")
{
    // The keybinding-facing tab navigation (SwitchToTab*/MoveTabTo*) resolves the acting session's
    // window and mutates only that window's model — covering the acting-session routing plus the
    // guard paths for null/unregistered acting sessions.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    window->createNewTab();
    window->createNewTab();
    REQUIRE(window->count() == 3);
    REQUIRE(window->activeTabIndex() == 2); // newest tab becomes active

    auto* acting = window->activeSession();
    REQUIRE(acting != nullptr);

    // Positional switch (1-based positions per the SwitchToTab action contract).
    manager.switchToTab(1, acting);
    CHECK(window->activeTabIndex() == 0);

    acting = window->activeSession();
    manager.switchToTabRight(acting);
    CHECK(window->activeTabIndex() == 1);

    acting = window->activeSession();
    manager.switchToTabLeft(acting);
    CHECK(window->activeTabIndex() == 0);

    // Previous-tab round trip: switching away and invoking previous returns to the earlier tab.
    acting = window->activeSession();
    manager.switchToTab(3, acting);
    CHECK(window->activeTabIndex() == 2);
    acting = window->activeSession();
    manager.switchToPreviousTab(acting);
    CHECK(window->activeTabIndex() == 0);

    // Tab moving: right, left, and to an absolute position.
    acting = window->activeSession();
    manager.moveTabToRight(acting);
    CHECK(window->activeTabIndex() == 1);
    manager.moveTabToLeft(acting);
    CHECK(window->activeTabIndex() == 0);
    manager.moveTabTo(2, acting);
    CHECK(window->activeTabIndex() == 1);

    // Guard paths: null and unregistered acting sessions are safe no-ops.
    auto const before = window->activeTabIndex();
    manager.switchToTabLeft(nullptr);
    manager.switchToTabRight(nullptr);
    manager.switchToPreviousTab(nullptr);
    manager.switchToTab(2, nullptr);
    manager.moveTabTo(1, nullptr);
    manager.moveTabToLeft(nullptr);
    manager.moveTabToRight(nullptr);
    manager.closeTab(nullptr);
    manager.splitActivePane(true, nullptr);
    manager.closeActivePane(nullptr);
    manager.focusPane(vtmux::FocusDirection::Left, nullptr);
    CHECK(window->activeTabIndex() == before);

    // Out-of-range positional switches are no-ops.
    acting = window->activeSession();
    manager.switchToTab(0, acting);
    manager.switchToTab(99, acting);
    CHECK(window->activeTabIndex() == before);

    // closeTab(acting): the model prune is ASYNCHRONOUS via the session's exit watcher, which only
    // runs for STARTED sessions (a display attach starts them; these headless sessions never start).
    // The synchronous, observable contract here is that every pane PTY of the acting tab is closed.
    acting = window->activeSession();
    manager.closeTab(acting);
    CHECK(std::ranges::any_of(factory->createdPtys, [](auto* pty) { return pty->isClosed(); }));

    // Color-preference propagation reaches every live session without crashing.
    manager.updateColorPreference(vtbackend::ColorPreference::Dark);
    manager.updateColorPreference(vtbackend::ColorPreference::Light);

    // currentSessionIsTerminated is a logging-only notification: safe at any time.
    manager.currentSessionIsTerminated();

    for (int row = window->count() - 1; row >= 0; --row)
        window->closeTabAtIndex(row);
    QCoreApplication::processEvents();
}

TEST_CASE("manager pane split/close/focus operate on the acting session's tab", "[contour][multiwindow]")
{
    // Headless pane-tree operations through the manager (the QML-independent half of the split
    // feature): split the active pane, focus across the divider, and close the split back down.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);
    auto* acting = window->activeSession();
    REQUIRE(acting != nullptr);

    manager.splitActivePane(/*vertical*/ true, acting);
    QCoreApplication::processEvents();

    auto* tab = manager.model().window(window->windowId())->activeTab();
    REQUIRE(tab != nullptr);
    CHECK(tab->hasMultiplePanes());

    // Focus movement across the split (Left from the new right-hand pane).
    manager.focusPane(vtmux::FocusDirection::Left, window->activeSession());
    QCoreApplication::processEvents();

    // Close the active pane; the tab collapses back to a single pane.
    manager.closeActivePane(window->activeSession());
    QCoreApplication::processEvents();
    CHECK_FALSE(tab->hasMultiplePanes());

    for (int row = window->count() - 1; row >= 0; --row)
        window->closeTabAtIndex(row);
    QCoreApplication::processEvents();
}

TEST_CASE("manager guards unknown windows and null acting sessions", "[contour][multiwindow]")
{
    // The manager's window/session-keyed entry points must degrade to safe no-ops for inputs that do
    // not resolve — an unknown WindowId (a closed window) and a null acting session (a keybinding
    // firing during teardown). These are the guards every routing method rides.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();

    // Creating a session in a window id that was never minted resolves to no model window -> nullptr.
    CHECK(manager.createSessionInBackground(vtmux::WindowId { 9999 }) == nullptr);

    // Tab/pane operations with a null acting session resolve to no target tab -> safe no-ops.
    CHECK_NOTHROW(manager.switchToTab(0, nullptr));
    CHECK_NOTHROW(manager.moveTabTo(1, nullptr));
    CHECK_NOTHROW(manager.moveTabToLeft(nullptr));
    CHECK_NOTHROW(manager.moveTabToRight(nullptr));
    CHECK_NOTHROW(manager.splitActivePane(/*vertical*/ true, nullptr));
    CHECK_NOTHROW(manager.focusPane(vtmux::FocusDirection::Left, nullptr));
    CHECK_NOTHROW(manager.closeActivePane(nullptr));
}
