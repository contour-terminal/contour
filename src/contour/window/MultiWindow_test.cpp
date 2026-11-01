// SPDX-License-Identifier: Apache-2.0
//
// Multi-window coverage at the manager + WindowController layer, headless (offscreen QGuiApplication
// from test_main, no PTY: tabs are minted straight through the vtworkspace model, which never spawns a
// backing session).
//
// The headline cases are REGRESSION PINS for the WindowId-routing refactor: before it, the manager
// resolved every tab/pane write through the legacy single-window pointer, so a tab activation, move,
// bulk close, rename, or pane-ratio drag issued from a SECOND OS window silently operated on the
// FIRST window's model. Each pin drives the operation through window B's controller and asserts A is
// untouched.

#include <contour/session/EndReason.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/TabBarDropPolicy.hpp>
#include <contour/window/WindowController.hpp>

#include <vtpty/MockPty.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vthost/client/ConnectionState.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace
{

using contour::test::ScopedController;
using contour::test::TestApp;

using contour::test::RecordingTaskbar;

/// Mints @p n model tabs in the controller's window (no backing sessions).
void createTabs(contour::session::TerminalSessionManager& manager,
                contour::window::WindowController& controller,
                int n)
{
    for (int i = 0; i < n; ++i)
        REQUIRE(manager.model().createTab(controller.windowId()) != nullptr);
}

[[nodiscard]] QString rawTitleAt(contour::window::WindowController& controller, int row)
{
    return controller
        .data(controller.index(row), static_cast<int>(contour::window::WindowController::Roles::RawTitleRole))
        .toString();
}

} // namespace

TEST_CASE("a window's hosting notice shows until it is dismissed", "[contour][multiwindow][hosting]")
{
    contour::test::TestApp app;
    contour::test::ScopedController const window { app.manager() };
    auto& controller = *window.controller;
    auto changes = QSignalSpy { &controller, &contour::window::WindowController::hostingNoticeChanged };
    REQUIRE(controller.hostingNotice().isEmpty());

    controller.showHostingNotice(QStringLiteral("Contour could not reach its daemon (gone)."));
    CHECK(controller.hostingNotice() == QStringLiteral("Contour could not reach its daemon (gone)."));
    // The same line again is no change, and announces none.
    controller.showHostingNotice(QStringLiteral("Contour could not reach its daemon (gone)."));
    CHECK(changes.count() == 1);

    controller.dismissHostingNotice();
    CHECK(controller.hostingNotice().isEmpty());
    CHECK(changes.count() == 2);
}

TEST_CASE("a window says its notices only while accessibility_announcements allows",
          "[contour][multiwindow][notices]")
{
    contour::test::TestApp app;
    auto& manager = app.manager();
    auto* said = static_cast<contour::test::RecordingAnnouncer*>(nullptr);
    auto const recording =
        [&said](
            std::function<QObject*()> const& /*anchor*/) -> std::unique_ptr<contour::platform::Announcer> {
        auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
        said = announcer.get();
        return announcer;
    };
    auto const* backing = manager.model().createWindow();
    auto controller = contour::window::WindowController { manager,
                                                          backing->id(),
                                                          contour::config::MenuPresentation::InScene,
                                                          std::nullopt,
                                                          core::platform::defaultSteadyClock(),
                                                          recording };
    REQUIRE(said != nullptr);
    using contour::window::Notice;
    using contour::window::NoticeKind;

    // Off, the notice still shows; nobody is told it.
    app.app().config().accessibilityAnnouncements = false;
    controller.notices().post(Notice { .kind = NoticeKind::Copied, .text = "Copied 3 lines" });
    CHECK(controller.notices().toastText() == QStringLiteral("Copied 3 lines"));
    CHECK(said->said.empty());

    // On again -- as a reload sets it -- the next one is said.
    app.app().config().accessibilityAnnouncements = true;
    controller.notices().post(
        Notice { .kind = NoticeKind::ConfigReloadFailed, .text = "Contour could not reload" });
    REQUIRE(said->said.size() == 1);
    CHECK(said->said.front().message == QStringLiteral("Contour could not reload"));
}

TEST_CASE("a local window names no host in its chrome", "[contour][multiwindow][window][hosts]")
{
    contour::test::TestApp app;
    contour::test::ScopedController const window { app.manager() };
    auto const& controller = *window.controller;
    CHECK(controller.hostLabel().isEmpty());
    CHECK(controller.windowTitle(QStringLiteral("vim")) == QStringLiteral("vim"));
}

TEST_CASE("new tabs use their profile's default color", "[contour][multiwindow][tab-color]")
{
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>());
    auto const color = vtbackend::RGBColor { 0x2E, 0x34, 0x40 };
    app.app().config().profile().tabColor.value() = color;
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* modelWindow = manager.model().window(window.id);
    REQUIRE(modelWindow->tabCount() == 2);
    for (auto const index: { 0, 1 })
    {
        auto* tab = modelWindow->tabAt(index);
        CHECK(tab->color(vtworkspace::TabColorSource::Profile) == color);
        window->setTabColor(index, QColor(Qt::red));
        window->resetTabColor(index);
        CHECK(tab->color() == color);
    }
}

TEST_CASE("unsupported taskbar adapters do not schedule updates", "[contour][multiwindow][taskbar]")
{
    auto sink = std::make_unique<RecordingTaskbar>(RecordingTaskbar::Support::Unavailable);
    auto* recording = sink.get();
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>(),
                nullptr,
                nullptr,
                nullptr,
                TestApp::testHostIdentity(),
                nullptr,
                {},
                std::move(sink));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    auto const now = app.steadyClock().now();
    manager.recordProgress(window->activeSession()->modelSessionId(),
                           { .state = vtbackend::ProgressState::Normal, .percentage = 40 },
                           now);
    QCoreApplication::processEvents();
    CHECK(recording->windows.empty());
    CHECK(manager.taskbarProgress(window.id, now).state == vtbackend::ProgressState::Normal);
}

TEST_CASE("taskbar progress follows background panes across window moves and closure",
          "[contour][multiwindow][taskbar]")
{
    auto sink = std::make_unique<RecordingTaskbar>();
    auto* recording = sink.get();
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>(),
                nullptr,
                nullptr,
                nullptr,
                TestApp::testHostIdentity(),
                nullptr,
                {},
                std::move(sink));
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    windowA->createNewTab();
    auto* background = windowA->activeSession();
    manager.splitActivePane(true, background);
    windowA->createNewTab(); // The reporting pane is now in an inactive tab and is not its active pane.
    windowB->createNewTab();
    auto* paused = windowB->activeSession();
    auto const now = app.steadyClock().now();
    manager.recordProgress(
        background->modelSessionId(), { .state = vtbackend::ProgressState::Error, .percentage = 40 }, now);
    manager.recordProgress(
        paused->modelSessionId(), { .state = vtbackend::ProgressState::Paused, .percentage = 20 }, now);
    QCoreApplication::processEvents();
    CHECK(recording->windows.at(windowA.id.value).state == vtbackend::ProgressState::Error);
    CHECK(recording->windows.at(windowB.id.value).state == vtbackend::ProgressState::Paused);

    manager.moveTabToWindow(windowA.id, 0, windowB.id, 0);
    QCoreApplication::processEvents();
    CHECK(recording->windows.at(windowA.id.value).state == vtbackend::ProgressState::Inactive);
    CHECK(recording->windows.at(windowB.id.value).state == vtbackend::ProgressState::Error);

    // Mock PTYs are not started: drive the sessionClosed endpoint rather than waiting for an exit watcher.
    manager.removeSession(*background);
    QCoreApplication::processEvents();
    CHECK(recording->windows.at(windowB.id.value).state == vtbackend::ProgressState::Paused);
    app.app().config().progressTimeout.value() = std::chrono::milliseconds { 10 };
    manager.recordProgress(
        paused->modelSessionId(), { .state = vtbackend::ProgressState::Paused, .percentage = 20 }, now);
    QCoreApplication::processEvents();
    app.steadyClock().advance(std::chrono::milliseconds { 11 });
    // Time is explicit; wait only for Qt to deliver the scheduled callback, with no subsequent report.
    REQUIRE(QTest::qWaitFor(
        [&] { return recording->windows.at(windowB.id.value).state == vtbackend::ProgressState::Inactive; },
        1000));
    windowA->closeTabAtIndex(0);
    windowB->closeTabAtIndex(0);
}

TEST_CASE("application taskbar progress combines windows without resetting report deadlines",
          "[contour][multiwindow][taskbar]")
{
    auto sink = std::make_unique<RecordingTaskbar>(RecordingTaskbar::Support::Available,
                                                   contour::platform::TaskbarProgressScope::Application);
    auto* recording = sink.get();
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>(),
                nullptr,
                nullptr,
                nullptr,
                TestApp::testHostIdentity(),
                nullptr,
                {},
                std::move(sink));
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    windowA->createNewTab();
    windowB->createNewTab();
    auto* failed = windowA->activeSession();
    auto* running = windowB->activeSession();
    auto const now = app.steadyClock().now();
    app.app().config().progressTimeout.value() = std::chrono::milliseconds { 10 };
    manager.recordProgress(
        failed->modelSessionId(), { .state = vtbackend::ProgressState::Error, .percentage = 60 }, now);
    manager.recordProgress(
        running->modelSessionId(), { .state = vtbackend::ProgressState::Normal, .percentage = 20 }, now);
    QCoreApplication::processEvents();
    CHECK(recording->windows.empty());
    CHECK(
        recording->application
        == contour::platform::TaskbarProgress { .state = vtbackend::ProgressState::Error, .percentage = 60 });
    manager.moveTabToWindow(windowA.id, 0, windowB.id, 0);
    QCoreApplication::processEvents();
    CHECK(recording->application.state == vtbackend::ProgressState::Error);
    manager.removeSession(*failed);
    QCoreApplication::processEvents();
    CHECK(recording->application
          == contour::platform::TaskbarProgress { .state = vtbackend::ProgressState::Normal,
                                                  .percentage = 20 });
    app.steadyClock().advance(std::chrono::milliseconds { 11 });
    REQUIRE(QTest::qWaitFor(
        [&] { return recording->application.state == vtbackend::ProgressState::Inactive; }, 1000));
    windowB->closeTabAtIndex(0);
}

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
    QSignalSpy const shouldShowSpy(window.controller,
                                   &contour::window::WindowController::tabBarShouldShowChanged);

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

TEST_CASE("a config reload overrides the seeded tab strip state", "[contour][multiwindow]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };
    REQUIRE(window.controller != nullptr);

    // The seed latch exists so a session rebind cannot reset a runtime override. It must NOT swallow a
    // reload: before applyTabBarFromConfig existed, saving tab_bar_visibility in the settings page did
    // nothing at all until the application was restarted, because the reload arrived as another seed.
    window->seedTabBarPosition(TabBarPosition::Bottom);
    window->seedTabBarVisibility(TabBarVisibility::Never);

    auto positionSpy =
        QSignalSpy(window.controller, &contour::window::WindowController::tabBarPositionChanged);
    auto visibilitySpy =
        QSignalSpy(window.controller, &contour::window::WindowController::tabBarVisibilityChanged);
    auto shouldShowSpy =
        QSignalSpy(window.controller, &contour::window::WindowController::tabBarShouldShowChanged);

    window->applyTabBarFromConfig(TabBarPosition::Top, TabBarVisibility::Multiple);
    CHECK(window->tabBarPosition() == static_cast<int>(TabBarPosition::Top));
    CHECK(window->tabBarVisibility() == static_cast<int>(TabBarVisibility::Multiple));
    CHECK(positionSpy.count() == 1);
    CHECK(visibilitySpy.count() == 1);
    // The resolved gate depends on the mode, so QML must be told to re-evaluate it too.
    CHECK(shouldShowSpy.count() == 1);

    // Applying the same values again changes nothing and says nothing.
    window->applyTabBarFromConfig(TabBarPosition::Top, TabBarVisibility::Multiple);
    CHECK(positionSpy.count() == 1);
    CHECK(visibilitySpy.count() == 1);

    // And a later session rebind still cannot clobber what the reload applied.
    window->seedTabBarPosition(TabBarPosition::Bottom);
    window->seedTabBarVisibility(TabBarVisibility::Never);
    CHECK(window->tabBarPosition() == static_cast<int>(TabBarPosition::Top));
    CHECK(window->tabBarVisibility() == static_cast<int>(TabBarVisibility::Multiple));
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

TEST_CASE("the active tab stays active while Settings is open", "[contour][multiwindow][settings]")
{
    // Settings is a window of its own: opening it covers no tab, so the strip keeps highlighting the
    // active one.
    constexpr auto Tabs = 2;
    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };
    REQUIRE(window.controller != nullptr);
    createTabs(manager, *window, Tabs);
    window->activateTab(1);

    app.app().openSettings(nullptr);

    auto const isActive = [&](int row) {
        return window
            ->data(window->index(row),
                   static_cast<int>(contour::window::WindowController::Roles::IsActiveRole))
            .toBool();
    };
    CHECK(isActive(1));
    CHECK_FALSE(isActive(0));
    CHECK(window->rowCount() == window->count());
    CHECK(window->count() == Tabs);
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

    auto spyB = QSignalSpy(windowB.controller, &contour::window::WindowController::tabTitleEditRequested);
    auto spyA = QSignalSpy(windowA.controller, &contour::window::WindowController::tabTitleEditRequested);

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

    auto spy = QSignalSpy(window.controller, &contour::window::WindowController::tabTitleEditRequested);
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
    REQUIRE(manager.model().splitActivePane(tabA->id(), vtworkspace::SplitState::Vertical) != nullptr);
    REQUIRE(manager.model().splitActivePane(tabB->id(), vtworkspace::SplitState::Vertical) != nullptr);
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
    CHECK(roles.contains(static_cast<int>(contour::window::WindowController::Roles::TitleRole)));
    CHECK(roles.contains(static_cast<int>(contour::window::WindowController::Roles::RawTitleRole)));
    CHECK(roles.contains(static_cast<int>(contour::window::WindowController::Roles::IsActiveRole)));

    // A never-renamed tab has an empty raw title but a resolved (template-expanded) title.
    CHECK(rawTitleAt(*window, 0).isEmpty());
    CHECK_NOTHROW(
        window->data(window->index(0), static_cast<int>(contour::window::WindowController::Roles::TitleRole))
            .toString());

    // Rename routes to the model and surfaces through RawTitleRole; reset restores the default.
    window->setTabTitle(0, QStringLiteral("deploy"));
    CHECK(rawTitleAt(*window, 0) == "deploy");
    window->resetTabTitle(0);
    CHECK(rawTitleAt(*window, 0).isEmpty());

    // An empty rename resets rather than blanking (the vtworkspace-layer invariant, via the controller).
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
    contour::test::closeAllTabs(*window);
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
    auto spy = QSignalSpy(window.controller, &contour::window::WindowController::dataChanged);

    // Program-driven title change on the BACKGROUND session (as an OSC title sequence would do).
    backgroundSession->setTitle(QStringLiteral("background-build-running"));

    // The refresh is queued onto the session (GUI thread); deliver it.
    QCoreApplication::processEvents();

    // The background tab's resolved title now reflects the change...
    CHECK(
        window->data(window->index(1), static_cast<int>(contour::window::WindowController::Roles::TitleRole))
            .toString()
        == QStringLiteral("background-build-running"));
    // ...and a dataChanged carrying TitleRole was emitted for row 1 (not row 0, the active tab).
    auto sawRow1TitleChange = false;
    for (auto const& emission: spy)
    {
        auto const topLeft = emission.at(0).toModelIndex();
        auto const roles = emission.at(2).value<QList<int>>();
        if (topLeft.row() == 1
            && roles.contains(static_cast<int>(contour::window::WindowController::Roles::TitleRole)))
            sawRow1TitleChange = true;
    }
    CHECK(sawRow1TitleChange);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("a tab whose label comes out empty is named after the program in front",
          "[contour][multiwindow][model][tablabel]")
{
    // A blank tab names nothing: the label falls back to the session's foreground process, and only while
    // it is empty.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    factoryOwned->makePty = [](vtbackend::PageSize size) -> std::unique_ptr<vtpty::Pty> {
        auto pty = std::make_unique<vtpty::MockPty>(size);
        pty->setForegroundProcessName("vim");
        return pty;
    };
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    REQUIRE(window->count() == 1);
    auto const titleAt = [&](int row) {
        return window
            ->data(window->index(row), static_cast<int>(contour::window::WindowController::Roles::TitleRole))
            .toString();
    };
    auto* session =
        manager.sessionForId(manager.model().window(window.id)->tabAt(0)->activePane()->session());
    REQUIRE(session != nullptr);
    REQUIRE(session->resolvedWindowTitle().empty()); // the premise: the default label comes out empty

    CHECK(titleAt(0) == QStringLiteral("vim"));

    session->setTitle(QStringLiteral("build"));
    QCoreApplication::processEvents();
    CHECK(titleAt(0) == QStringLiteral("build"));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("the default tab label shows the tab name an application sets, else the window title",
          "[contour][multiwindow][model][tablabel]")
{
    // Issue #2170: OSC 30 named the tab in the indicator status line, while the tab bar kept showing the
    // OSC 2 window title. Under the default template the tab bar now agrees with the status line.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    REQUIRE(window->count() == 1);
    auto const titleAt = [&](int row) {
        return window
            ->data(window->index(row), static_cast<int>(contour::window::WindowController::Roles::TitleRole))
            .toString();
    };
    auto* session =
        manager.sessionForId(manager.model().window(window.id)->tabAt(0)->activePane()->session());
    REQUIRE(session != nullptr);

    session->terminal().writeToScreen("\033]2;Window Title\033\\");
    QCoreApplication::processEvents();
    CHECK(titleAt(0) == QStringLiteral("Window Title")); // no tab name yet: the window title

    session->terminal().writeToScreen("\033]30;Tab Title\033\\");
    session->terminal().writeToScreen("\033]2;Another Window Title\033\\");
    QCoreApplication::processEvents();
    CHECK(titleAt(0) == QStringLiteral("Tab Title")); // the tab name wins over a later window title

    // An explicit {WindowTitle} template keeps showing the window title despite the tab name.
    auto* tab = manager.model().window(window.id)->tabAt(0);
    manager.model().setTabTitle(tab->id(), "{TabPosition}: {WindowTitle}");
    QCoreApplication::processEvents();
    CHECK(titleAt(0) == QStringLiteral("1: Another Window Title"));
    manager.model().setTabTitle(tab->id(), "{TabPosition}: {TabName}");
    QCoreApplication::processEvents();
    CHECK(titleAt(0) == QStringLiteral("1: Tab Title"));
    manager.model().setTabTitle(tab->id(), ""); // back to the default template

    // An empty OSC 30 clears the name: the tab falls back to its window title, not blank.
    session->terminal().writeToScreen("\033]30;\033\\");
    QCoreApplication::processEvents();
    CHECK(titleAt(0) == QStringLiteral("Another Window Title"));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("setTabColorForSession routes a DECAC frame color to the session's tab",
          "[contour][multiwindow][model]")
{
    // The backend DECAC item-2 path calls TerminalSessionManager::setTabColorForSession(sessionId, ...)
    // (posted from TerminalSession::setWindowFrameColor). This is the direct unit test of that routing:
    // a session's model id must resolve to its hosting tab and land the color on Tab::_color.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);

    auto* tab = manager.model().window(window.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    auto const sessionId = tab->activePane()->session();
    REQUIRE_FALSE(tab->color().has_value());

    auto const red = vtbackend::RGBColor { 0xFF, 0x00, 0x00 };
    manager.setTabColorForSession(sessionId, red);
    REQUIRE(tab->color().has_value());
    CHECK(*tab->color() == red);

    manager.resetTabColorForSession(sessionId);
    CHECK_FALSE(tab->color().has_value());

    // An unknown session id is a harmless no-op (the null-tab guard).
    CHECK_NOTHROW(manager.setTabColorForSession(vtworkspace::SessionId { 999999 }, red));
    CHECK_FALSE(tab->color().has_value());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("setTabTitleForSession routes a tmux %window-renamed to the session's tab",
          "[contour][multiwindow][model]")
{
    // The tmux mirror's %window-renamed path calls TerminalSessionManager::setTabTitleForSession(
    // sessionId, name) (posted from TmuxController::applyPendingRenames). This is the direct unit test
    // of that routing: a session's model id resolves to its hosting tab and lands the runtime title.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);

    auto* tab = manager.model().window(window.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    auto const sessionId = tab->activePane()->session();
    REQUIRE_FALSE(tab->runtimeTitle().has_value());

    manager.setTabTitleForSession(sessionId, "build");
    REQUIRE(tab->runtimeTitle().has_value());
    CHECK(*tab->runtimeTitle() == "build");

    // A later rename overwrites it.
    manager.setTabTitleForSession(sessionId, "deploy");
    CHECK(*tab->runtimeTitle() == "deploy");

    // An unknown session id is a harmless no-op (the null-tab guard).
    CHECK_NOTHROW(manager.setTabTitleForSession(vtworkspace::SessionId { 999999 }, "ghost"));
    CHECK(*tab->runtimeTitle() == "deploy");

    contour::test::closeAllTabs(*window);
}

TEST_CASE("the SetTabColor / ResetTabColor actions color the acting session's tab",
          "[contour][multiwindow][model]")
{
    // The whole keybinding chain, headless: action -> TerminalSession::operator() -> the manager ->
    // the hosting window's controller -> SessionModel, landing in TabColorSource::User. That last part
    // is the contract worth pinning: a key bound to SetTabColor must behave exactly like picking the
    // same color in the flyout, DECAC precedence and all.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);

    auto* tab = manager.model().window(window.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    auto const sessionId = tab->activePane()->session();
    auto* session = manager.sessionForId(sessionId);
    REQUIRE(session != nullptr);
    REQUIRE_FALSE(tab->color().has_value());

    auto const red = vtbackend::RGBColor { 0xFF, 0x00, 0x00 };
    auto const appColor = vtbackend::RGBColor { 0x11, 0x22, 0x33 };

    SECTION("a color-carrying SetTabColor applies it directly, outranking the application's")
    {
        manager.setTabColorForSession(sessionId, appColor); // as if the app had sent DECAC
        REQUIRE(tab->color() == appColor);

        CHECK((*session)(contour::actions::SetTabColor { red }));
        CHECK(tab->color() == red); // the user's choice wins

        // ResetTabColor drops only what the user chose, so the application's color resurfaces — the
        // same "default means whatever I did not choose" rule the flyout's Reset button follows.
        CHECK((*session)(contour::actions::ResetTabColor {}));
        CHECK(tab->color() == appColor);
    }

    SECTION("a colorless SetTabColor opens the picker instead of coloring anything")
    {
        auto spy = QSignalSpy(window.controller, &contour::window::WindowController::tabColorPickRequested);

        CHECK((*session)(contour::actions::SetTabColor {}));

        REQUIRE(spy.count() == 1);
        CHECK(spy.takeFirst().at(0).toInt() == window->activeTabIndex());
        CHECK_FALSE(tab->color().has_value()); // it asked; it did not decide
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("beginActiveTabColorPick requests the active tab's picker, per window", "[contour][multiwindow]")
{
    // Same per-window routing the title editor gets: the signal must carry the ACTIVE row and reach only
    // the window whose controller was asked, or a chord pressed in window B pops a picker in window A.
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    createTabs(manager, *windowA, 2);
    createTabs(manager, *windowB, 3);

    windowB->activateTab(2);
    REQUIRE(windowB->activeTabIndex() == 2);

    auto spyB = QSignalSpy(windowB.controller, &contour::window::WindowController::tabColorPickRequested);
    auto spyA = QSignalSpy(windowA.controller, &contour::window::WindowController::tabColorPickRequested);

    windowB->beginActiveTabColorPick();

    REQUIRE(spyB.count() == 1);
    CHECK(spyB.takeFirst().at(0).toInt() == 2);
    CHECK(spyA.count() == 0);

    SECTION("with no tabs there is nothing to color, so nothing is asked")
    {
        ScopedController empty { manager };
        REQUIRE(empty->activeTabIndex() < 0);

        auto spy = QSignalSpy(empty.controller, &contour::window::WindowController::tabColorPickRequested);
        empty->beginActiveTabColorPick();
        CHECK(spy.count() == 0);

        // And the row-less setters stay no-ops rather than reaching for tab -1.
        CHECK_NOTHROW(empty->setActiveTabColor(vtbackend::RGBColor { 0xFF, 0x00, 0x00 }));
        CHECK_NOTHROW(empty->resetActiveTabColor());
    }
}

TEST_CASE("the SaveLayout action saves the acting window, and nameless opens the prompt",
          "[contour][multiwindow][layout]")
{
    // The layout-side mirror of the SetTabColor chain: a named SaveLayout runs the whole keybinding path
    // headless (action -> TerminalSession::operator() -> the manager -> the injected LayoutStore), while a
    // nameless one opens the save-as prompt instead of saving anything — exactly as a colorless SetTabColor
    // opens the picker rather than coloring.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto storeOwned = std::make_unique<contour::test::InMemoryLayoutStore>();
    auto* store = storeOwned.get();
    contour::test::TestApp app { std::move(factoryOwned), std::move(storeOwned) };
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);

    auto* tab = manager.model().window(window.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    auto* session = manager.sessionForId(tab->activePane()->session());
    REQUIRE(session != nullptr);

    SECTION("a named SaveLayout persists the window's tabs through the store")
    {
        CHECK((*session)(contour::actions::SaveLayout { .name = "dev" }));

        REQUIRE(store->savedPaths.size() == 1);
        CHECK(store->layouts.contains("dev"));
        // The live config sees it too, so a LaunchLayout later in this run finds it.
        CHECK(app.app().config().layouts.value().contains("dev"));
    }

    SECTION("a nameless SaveLayout opens the save-as prompt instead of saving anything")
    {
        auto spy = QSignalSpy(window.controller, &contour::window::WindowController::saveLayoutRequested);

        CHECK((*session)(contour::actions::SaveLayout {}));

        CHECK(spy.count() == 1);          // it asked for a name
        CHECK(store->savedPaths.empty()); // and wrote nothing until it has one
    }

    SECTION("saveLayoutAs persists under the typed name, and a blank name is a no-op")
    {
        window->saveLayoutAs(QStringLiteral("  from-dialog  ")); // the prompt's accept, whitespace and all
        REQUIRE(store->savedPaths.size() == 1);
        CHECK(store->layouts.contains("from-dialog")); // trimmed before it lands as the key

        window->saveLayoutAs(QStringLiteral("   ")); // a blank field must never write a nameless layout
        CHECK(store->savedPaths.size() == 1);        // still just the one save
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("Save Layout checks the name, refuses a taken one, and replaces only when asked",
          "[contour][multiwindow][layout]")
{
    auto storeOwned = std::make_unique<contour::test::InMemoryLayoutStore>();
    auto* store = storeOwned.get();
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>(),
                                 std::move(storeOwned) };
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    REQUIRE(window->count() == 1);
    CHECK(window->layoutSummary() == "1 tab · 1 pane · working directories and commands");

    SECTION("a name the configuration holds is taken, as typed and trimmed")
    {
        app.app().config().layouts.value()["dev"] = contour::config::Layout {};
        auto const check = window->checkLayoutName(QStringLiteral("  dev "), false);
        CHECK(check.value("nameState").toString() == "taken");
        CHECK(check.value("button").toString() == "disabled");
        CHECK(check.value("trimmed").toString() == "dev");
        CHECK(window->checkLayoutName(QStringLiteral("dev"), true).value("button").toString() == "replace");
        CHECK(window->checkLayoutName(QStringLiteral("web"), false).value("button").toString() == "save");
    }

    SECTION("a name the layouts file holds is taken too, once the dialog opens")
    {
        store->layouts["filed"] = contour::config::Layout {};
        window->beginSaveLayoutPrompt();
        CHECK(window->checkLayoutName(QStringLiteral("filed"), false).value("nameState").toString()
              == "taken");
        CHECK(window->checkLayoutName(QStringLiteral("filed"), true).value("button").toString() == "replace");
    }

    SECTION("a name taken in the store since the check is not overwritten")
    {
        store->layouts["dev"] = contour::config::Layout {};
        auto const before = store->layouts;
        window->saveLayoutAs(QStringLiteral("dev"));
        CHECK(store->savedPaths.empty());
        CHECK(store->layouts.size() == before.size());
        CHECK(window->notices().toastText().toStdString()
              == contour::window::layoutSaveFailedNoticeText("dev",
                                                             contour::config::LayoutSaveError::NameTaken));
    }

    SECTION("Replace rewrites it, and says so")
    {
        store->layouts["dev"] = contour::config::Layout {};
        window->replaceLayout(QStringLiteral("dev"));
        REQUIRE(store->savedPaths.size() == 1);
        REQUIRE(store->layouts.contains("dev"));
        CHECK(store->layouts.at("dev").tabs.size() == 1);
        CHECK(window->notices().toastText() == QStringLiteral("Saved layout “dev”"));
    }

    SECTION("an unreadable store says why in a toast")
    {
        store->loadError = "bad yaml";
        window->saveLayoutAs(QStringLiteral("web"));
        CHECK(store->savedPaths.empty());
        auto const expected =
            std::format("{}.", contour::config::describe(contour::config::LayoutSaveError::StoreUnreadable));
        CHECK(window->notices().toastText().toStdString().ends_with(expected));
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("beginSaveLayoutPrompt requests the save-as prompt, per window", "[contour][multiwindow]")
{
    // The same per-window routing the color picker gets: the request must reach only the window whose
    // controller was asked, or a chord pressed in window B pops a prompt over window A.
    TestApp app;
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };
    createTabs(manager, *windowA, 2);
    createTabs(manager, *windowB, 1);

    auto spyB = QSignalSpy(windowB.controller, &contour::window::WindowController::saveLayoutRequested);
    auto spyA = QSignalSpy(windowA.controller, &contour::window::WindowController::saveLayoutRequested);

    windowB->beginSaveLayoutPrompt();

    CHECK(spyB.count() == 1);
    CHECK(spyA.count() == 0);
}

TEST_CASE("a user's tab color outranks DECAC and survives a terminal reset", "[contour][multiwindow][model]")
{
    // The user's choice always wins, and no escape sequence can take it away: a DECAC reset — which is
    // also what RIS ("reset", "Clear History And Reset") triggers via Terminal::hardReset — withdraws
    // only the application's own color. Conversely, returning the tab to its default color un-picks the
    // user's choice and reveals whatever the application assigned, rather than erasing it.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);

    auto* tab = manager.model().window(window.id)->tabAt(0);
    REQUIRE(tab != nullptr);
    auto const sessionId = tab->activePane()->session();

    auto const appColor = vtbackend::RGBColor { 0x11, 0x22, 0x33 };
    auto const userColor = vtbackend::RGBColor { 0xAA, 0xBB, 0xCC };

    // The application colors its tab; then the user overrides it from the tab's color flyout.
    manager.setTabColorForSession(sessionId, appColor);
    REQUIRE(tab->color() == appColor);
    window->setTabColor(0, QColor(0xAA, 0xBB, 0xCC));
    CHECK(tab->color() == userColor);

    // A terminal reset withdraws the application's color. The user's choice stays on screen.
    manager.resetTabColorForSession(sessionId);
    CHECK(tab->color() == userColor);

    // The application colors its tab again — still outranked, but remembered.
    manager.setTabColorForSession(sessionId, appColor);
    CHECK(tab->color() == userColor);

    // "Default" for the user means: whatever I did not choose. Here, the application's color.
    window->resetTabColor(0);
    CHECK(tab->color() == appColor);

    // With the application's color withdrawn too, the tab is finally uncolored.
    manager.resetTabColorForSession(sessionId);
    CHECK_FALSE(tab->color().has_value());

    contour::test::closeAllTabs(*window);
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
    contour::test::closeAllTabs(*window);
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
    CHECK_NOTHROW(manager.focusPane(vtworkspace::FocusDirection::Left, acting));
    CHECK_NOTHROW(manager.focusPane(vtworkspace::FocusDirection::Right, acting));

    // Closing the active pane collapses the split back to a single leaf.
    manager.closeActivePane(window->activeSession(), contour::session::SessionEnd::Destroy);
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
    CHECK_NOTHROW(manager.resizeActivePane(vtworkspace::FocusDirection::Left, 0.05, window->activeSession()));
    CHECK_NOTHROW(
        manager.resizeActivePane(vtworkspace::FocusDirection::Right, 0.05, window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Swap the active pane with its left neighbor: pane count preserved (sessions trade slots).
    CHECK_NOTHROW(manager.swapPane(vtworkspace::FocusDirection::Left, window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Move (re-parent) the active pane: pane count preserved, tree still valid.
    CHECK_NOTHROW(manager.movePane(vtworkspace::FocusDirection::Left, window->activeSession()));
    CHECK(tab->paneCount() == 3);

    // Guard paths: a null acting session is a no-op, never a crash.
    CHECK_NOTHROW(manager.swapPane(vtworkspace::FocusDirection::Left, nullptr));
    CHECK_NOTHROW(manager.movePane(vtworkspace::FocusDirection::Left, nullptr));
    CHECK_NOTHROW(manager.toggleActivePaneOrientation(nullptr));
    CHECK_NOTHROW(manager.resizeActivePane(vtworkspace::FocusDirection::Left, 0.05, nullptr));
    CHECK_NOTHROW(manager.toggleActivePaneZoom(nullptr));

    window->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager zooming re-roots the rendered pane tree at the active leaf",
          "[contour][multiwindow][split][zoom]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();

    auto* tab = manager.model().window(window.id)->activeTab();
    REQUIRE(tab != nullptr);

    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    REQUIRE(tab->paneCount() == 2);
    auto const ptyCount = factory->createdPtys.size();

    // Unzoomed, QML renders the whole tree: the root proxy is the split node.
    auto* splitRootProxy = window->activeTabRootPane();
    REQUIRE(splitRootProxy != nullptr);
    REQUIRE_FALSE(splitRootProxy->isLeaf());
    auto const splitRootId = splitRootProxy->paneId();

    auto const activeLeafId = static_cast<int>(tab->activePane()->id().value);
    auto* activeSession = window->activeSession();

    // Zoom: the rendered root becomes the ACTIVE LEAF itself. That re-rooting is the whole feature —
    // PaneNode.qml gives a leaf the full area it is handed, so the sibling simply leaves the scene.
    manager.toggleActivePaneZoom(window->activeSession());
    CHECK(tab->isZoomed());

    auto* zoomedProxy = window->activeTabRootPane();
    REQUIRE(zoomedProxy != nullptr);
    CHECK(zoomedProxy->isLeaf());
    CHECK(zoomedProxy->paneId() == activeLeafId);
    CHECK(zoomedProxy->session() == activeSession);

    // The hidden pane is HIDDEN, not closed: the tree keeps both panes and no PTY was torn down.
    CHECK(tab->paneCount() == 2);
    CHECK(factory->createdPtys.size() == ptyCount);

    // Unzoom: the split root comes back — and it is the very SAME proxy object, which is the proof
    // that the rebuild did not prune the panes hidden behind the zoom (proxies are cached per PaneId,
    // so a pruned-and-recreated one would be a different object, and its terminal a new one).
    manager.toggleActivePaneZoom(window->activeSession());
    CHECK_FALSE(tab->isZoomed());
    CHECK(window->activeTabRootPane() == splitRootProxy);
    CHECK(window->activeTabRootPane()->paneId() == splitRootId);
    CHECK(tab->paneCount() == 2);

    window->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager zoom follows focus and is cleared by restructuring",
          "[contour][multiwindow][split][zoom]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();

    auto* tab = manager.model().window(window.id)->activeTab();
    REQUIRE(tab != nullptr);
    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    REQUIRE(tab->paneCount() == 2);

    manager.toggleActivePaneZoom(window->activeSession());
    REQUIRE(tab->isZoomed());
    auto const zoomedLeafId = window->activeTabRootPane()->paneId();

    // Moving focus while zoomed keeps the zoom and shows the newly focused pane, so the rendered root
    // must move with it.
    manager.focusPane(vtworkspace::FocusDirection::Left, window->activeSession());
    CHECK(tab->isZoomed());
    auto* nowRendered = window->activeTabRootPane();
    REQUIRE(nowRendered != nullptr);
    CHECK(nowRendered->isLeaf());
    CHECK(nowRendered->paneId() != zoomedLeafId);
    CHECK(nowRendered->paneId() == static_cast<int>(tab->activePane()->id().value));

    // Splitting must surface the pane it creates, so it cancels the zoom and the whole tree renders.
    manager.splitActivePane(/*vertical*/ false, window->activeSession());
    CHECK_FALSE(tab->isZoomed());
    CHECK(tab->paneCount() == 3);
    CHECK_FALSE(window->activeTabRootPane()->isLeaf());

    window->closeTabAtIndex(0);
}

TEST_CASE("WindowController: the tab strip stops saying \"Multiple panes\" while zoomed",
          "[contour][multiwindow][split][zoom]")
{
    // Regression: the strip's label (resolvedTabLabel) re-derived Tab::title()'s precedence in order to
    // template it, and was never taught the zoom rule. So a zoomed tab kept reading "Multiple panes" in
    // the strip while the indicator status line — which does go through Tab::title() — simultaneously
    // renamed the very same tab after the pane on screen. Two names for one tab. Both now read the one
    // rule (Tab::usesMultiplePanesLabel), so they cannot drift apart again.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();

    auto* tab = manager.model().window(window.id)->activeTab();
    REQUIRE(tab != nullptr);

    auto const label = [&] {
        return window
            ->data(window->index(0), static_cast<int>(contour::window::WindowController::Roles::TitleRole))
            .toString()
            .toStdString();
    };
    auto const multiplePanes = std::string { vtworkspace::Tab::MultiplePanesLabel };

    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    REQUIRE(tab->paneCount() == 2);
    CHECK(label() == multiplePanes);

    // Zoomed: exactly one pane is on screen, so the tab is named after it — never the placeholder.
    manager.toggleActivePaneZoom(window->activeSession());
    REQUIRE(tab->isZoomed());
    CHECK(label() != multiplePanes);

    // ...and back, because the siblings are visible again.
    manager.toggleActivePaneZoom(window->activeSession());
    REQUIRE_FALSE(tab->isZoomed());
    CHECK(label() == multiplePanes);

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
    contour::test::ScopedControllerWithOsWindow const windowA { manager };
    ScopedController windowB { manager };

    // Bind a real OS window to A: closeWindow() must close THIS QQuickWindow too, otherwise its title
    // bar / tab strip lingers on screen as a ghost after the last tab is dragged away.
    auto& osWindowA = *windowA.osWindow;
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
    manager.closeActivePane(nullptr, contour::session::SessionEnd::Destroy);
    manager.focusPane(vtworkspace::FocusDirection::Left, nullptr);
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

    contour::test::closeAllTabs(*window);
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
    manager.focusPane(vtworkspace::FocusDirection::Left, window->activeSession());
    QCoreApplication::processEvents();

    // Close the active pane; the tab collapses back to a single pane.
    manager.closeActivePane(window->activeSession(), contour::session::SessionEnd::Destroy);
    QCoreApplication::processEvents();
    CHECK_FALSE(tab->hasMultiplePanes());

    contour::test::closeAllTabs(*window);
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
    CHECK(manager.createSessionInBackground(vtworkspace::WindowId { 9999 }) == nullptr);

    // Tab/pane operations with a null acting session resolve to no target tab -> safe no-ops.
    CHECK_NOTHROW(manager.switchToTab(0, nullptr));
    CHECK_NOTHROW(manager.moveTabTo(1, nullptr));
    CHECK_NOTHROW(manager.moveTabToLeft(nullptr));
    CHECK_NOTHROW(manager.moveTabToRight(nullptr));
    CHECK_NOTHROW(manager.splitActivePane(/*vertical*/ true, nullptr));
    CHECK_NOTHROW(manager.focusPane(vtworkspace::FocusDirection::Left, nullptr));
    CHECK_NOTHROW(manager.closeActivePane(nullptr, contour::session::SessionEnd::Destroy));
}

TEST_CASE("WindowController: minimised state, restoring it, and the attention requests Main.qml acts on",
          "[contour][window][notification]")
{
    TestApp app;
    contour::test::ScopedControllerWithOsWindow const window { app.manager() };

    SECTION("without an OS window nothing is minimised, and raising is a no-op")
    {
        CHECK_FALSE(window->isMinimized());
        window->raiseAndActivate();
        CHECK_FALSE(window->isMinimized());
    }

    SECTION("a minimised window is reported, and raising restores it without losing maximised")
    {
        window->bindWindow(window.osWindow.get());
        window.osWindow->setWindowStates(Qt::WindowMinimized | Qt::WindowMaximized);
        CHECK(window->isMinimized());

        window->raiseAndActivate();
        CHECK_FALSE(window->isMinimized());
        CHECK(window.osWindow->windowStates().testFlag(Qt::WindowMaximized));
    }

    SECTION("attention requests are re-emitted as signals for the window's QML")
    {
        QSignalSpy const notifications(window.controller,
                                       &contour::window::WindowController::notificationRequested);
        QSignalSpy const bells(window.controller, &contour::window::WindowController::bellRequested);
        QSignalSpy const alerts(window.controller, &contour::window::WindowController::alertRequested);

        window->requestNotification(QString::fromUtf8("✓ make finished"), QStringLiteral("took 12s"));
        window->requestBell(QStringLiteral("qrc:/contour/bell.wav"), 0.5);
        window->requestAlert();

        REQUIRE(notifications.count() == 1);
        CHECK(notifications.at(0).at(0).toString() == QString::fromUtf8("✓ make finished"));
        CHECK(notifications.at(0).at(1).toString() == QStringLiteral("took 12s"));
        REQUIRE(bells.count() == 1);
        CHECK(bells.at(0).at(0).toString() == QStringLiteral("qrc:/contour/bell.wav"));
        CHECK(bells.at(0).at(1).toDouble() == 0.5);
        CHECK(alerts.count() == 1);
    }
}

// {{{ Drops onto a pane

namespace
{
using contour::window::DropPayload;
using contour::window::DropZone;
using contour::window::HostKind;
using contour::window::MoveRefusal;
using contour::window::NodeDrag;

/// A pane card's extent in the tests below.
constexpr auto PaneWidth = 1000.0;
constexpr auto PaneHeight = 500.0;

/// @p json as another host wrote it: the same ids, which in that host name other sessions.
[[nodiscard]] QString fromAnotherHost(QString const& json)
{
    auto drag = contour::window::parseNodeDrag(json);
    REQUIRE(drag.has_value());
    drag->host.id += QStringLiteral("x");
    return contour::window::writeNodeDrag(*drag);
}

/// What @p controller says a drop of @p payload at (@p x, @p y) over @p session's pane does.
[[nodiscard]] DropZone zoneAt(contour::window::WindowController const& controller,
                              contour::session::TerminalSession const* session,
                              DropPayload payload,
                              double x,
                              double y,
                              QString const& node = {})
{
    return static_cast<DropZone>(
        controller.dropZoneAt(session, static_cast<int>(payload), x, y, PaneWidth, PaneHeight, node));
}

/// The window and tab showing @p session, as the model has them.
[[nodiscard]] std::pair<vtworkspace::WindowId, vtworkspace::Tab*> placeOf(
    contour::session::TerminalSessionManager const& manager, contour::session::TerminalSession const* session)
{
    auto const [window, tab, leaf] = manager.model().findSessionLeaf(session->modelSessionId());
    return { window != nullptr ? window->id() : vtworkspace::WindowId {}, tab };
}

/// A session alive where it was: the same pty, open, and what is written to it after a move shows.
void requireAlive(contour::session::TerminalSession* session, vtpty::Pty const* pty, std::string const& text)
{
    CHECK(&session->terminal().device() == pty);
    CHECK_FALSE(pty->isClosed());
    session->terminal().writeToScreen(text);
    CHECK(session->terminal().primaryScreen().grid().renderMainPageText().contains(text));
}

/// Two in-process windows whose announcements are recorded.
///
/// Window A holds tab 0 of two panes (`left`, `right`, `right` active) and tab 1 of one (`lone`); window B
/// holds one tab of one pane (`other`).
struct TwoWindows
{
    contour::test::MockPtySessionFactory* factory = nullptr;
    TestApp app;
    contour::session::TerminalSessionManager& manager;
    contour::test::RecordingAnnouncer* saidA = nullptr;
    contour::test::RecordingAnnouncer* saidB = nullptr;
    ScopedController a;
    ScopedController b;
    contour::session::TerminalSession* left;
    contour::session::TerminalSession* right;
    contour::session::TerminalSession* lone;
    contour::session::TerminalSession* other;
    std::size_t ptysBefore;

    TwoWindows():
        app(makeFactory()),
        manager(app.manager()),
        a(manager, recordInto(saidA)),
        b(manager, recordInto(saidB)),
        left(openTab(*a)),
        right(splitOff(manager, left)),
        lone(openTab(*a)),
        other(openTab(*b)),
        ptysBefore(factory->createdPtys.size())
    {
        app.app().config().accessibilityAnnouncements = true;
        REQUIRE((left != nullptr && right != nullptr && lone != nullptr && other != nullptr));
        REQUIRE(left != right);
    }

    ~TwoWindows()
    {
        factory->reportedHosting = contour::session::SessionHosting::Local;
        // A move may have closed either window.
        for (auto const id: { a.id, b.id })
            if (auto* const window = manager.controllerFor(id))
                contour::test::closeAllTabs(*window);
    }

    TwoWindows(TwoWindows const&) = delete;
    TwoWindows& operator=(TwoWindows const&) = delete;
    TwoWindows(TwoWindows&&) = delete;
    TwoWindows& operator=(TwoWindows&&) = delete;

    /// The pane drag of @p session, as its card writes it.
    [[nodiscard]] QString paneOf(contour::session::TerminalSession const* session) const
    {
        auto const [window, tab] = placeOf(manager, session);
        auto* const controller = manager.controllerFor(window);
        REQUIRE(controller != nullptr);
        auto const json = controller->paneDragPayload(session);
        REQUIRE_FALSE(json.isEmpty());
        return json;
    }

    /// Window A's tab of two panes, as its strip writes it.
    [[nodiscard]] QString splitTab() const
    {
        auto const json = a->tabDragPayload(0);
        REQUIRE(contour::window::parseNodeDrag(json)->payload == DropPayload::Tab);
        return json;
    }

    /// Every session the windows started with is the same object, alive, and nothing was spawned.
    void requireNothingRespawned() const
    {
        CHECK(factory->createdPtys.size() == ptysBefore);
        for (auto* session: { left, right, lone, other })
            CHECK_FALSE(session->terminal().device().isClosed());
    }

    /// What window @p said was told last.
    [[nodiscard]] static QString lastSaid(contour::test::RecordingAnnouncer const* said)
    {
        return said->said.empty() ? QString {} : said->said.back().message;
    }

  private:
    /// Opens a tab in @p window. @return Its pane's session.
    [[nodiscard]] static contour::session::TerminalSession* openTab(contour::window::WindowController& window)
    {
        window.createNewTab();
        return window.activeSession();
    }

    /// Splits @p session's pane. @return The new pane's session.
    [[nodiscard]] static contour::session::TerminalSession* splitOff(
        contour::session::TerminalSessionManager& manager, contour::session::TerminalSession* session)
    {
        manager.splitActivePane(/*vertical*/ true, session);
        return manager.controllerFor(placeOf(manager, session).first)->activeSession();
    }

    [[nodiscard]] std::unique_ptr<contour::session::SessionFactory> makeFactory()
    {
        auto owned = std::make_unique<contour::test::MockPtySessionFactory>();
        factory = owned.get();
        return owned;
    }

    [[nodiscard]] static contour::window::NoticeAnnouncerFactory recordInto(
        contour::test::RecordingAnnouncer*& said)
    {
        return [&said](std::function<QObject*()> const& /*anchor*/)
                   -> std::unique_ptr<contour::platform::Announcer> {
            auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
            said = announcer.get();
            return announcer;
        };
    }
};
} // namespace

TEST_CASE("parseNodeDrag reads what writeNodeDrag writes, and nothing else", "[contour][multiwindow][drop]")
{
    auto const drag = NodeDrag { .payload = DropPayload::Tab,
                                 .host = { .kind = HostKind::Daemon, .id = QStringLiteral("42") },
                                 .window = vtworkspace::WindowId { 3 },
                                 .tabIndex = 1,
                                 .sessions = { 7, 9 } };
    CHECK(contour::window::parseNodeDrag(contour::window::writeNodeDrag(drag)) == drag);
    auto pane = drag;
    pane.payload = DropPayload::Pane;
    pane.host = { .kind = HostKind::Process, .id = QStringLiteral("token") };
    pane.sessions = { 5 };
    CHECK(contour::window::parseNodeDrag(contour::window::writeNodeDrag(pane)) == pane);

    auto const valid = std::string {
        R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4]})"
    };
    REQUIRE(contour::window::parseNodeDrag(QString::fromStdString(valid)).has_value());
    for (
        auto const* const json: {
            "",
            "not json",
            "[]",
            R"({"host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4],"x":1})",
            R"({"kind":"pane","host":{"kind":"process","id":"t","x":1},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process"},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"window","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"server","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":""},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":7},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":1,"host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":"t","windowId":1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":"1","tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1.5,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":-1,"tabIndex":0,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":-1,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":3000000000,"sessions":[4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":4})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[-4]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":["4"]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4.5]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[]})",
            R"({"kind":"pane","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4,5]})",
            R"({"kind":"tab","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4]})",
            // A session named twice: a forged tab.
            R"({"kind":"tab","host":{"kind":"process","id":"t"},"windowId":1,"tabIndex":0,"sessions":[4,4]})",
        })
    {
        INFO(json);
        CHECK_FALSE(contour::window::parseNodeDrag(QString::fromUtf8(json)).has_value());
    }
}

// A forged payload may name any number of sessions: one naming more than a tab can hold is refused unread,
// and one naming a session twice is refused however long it is.
TEST_CASE("parseNodeDrag refuses an oversized or duplicated session list", "[contour][multiwindow][drop]")
{
    auto const tabOf = [](std::vector<uint64_t> sessions) {
        return contour::window::writeNodeDrag(
            NodeDrag { .payload = DropPayload::Tab,
                       .host = { .kind = HostKind::Daemon, .id = QStringLiteral("42") },
                       .window = vtworkspace::WindowId { 3 },
                       .tabIndex = 1,
                       .sessions = std::move(sessions) });
    };
    auto const distinct = [](std::size_t count) {
        return std::ranges::to<std::vector>(std::views::iota(uint64_t { 1 }, uint64_t { count } + 1));
    };
    auto const largest = distinct(contour::window::MaxDragSessions);
    CHECK(contour::window::parseNodeDrag(tabOf(largest)).has_value());
    CHECK_FALSE(contour::window::parseNodeDrag(tabOf(distinct(contour::window::MaxDragSessions + 1))));
    CHECK_FALSE(contour::window::parseNodeDrag(tabOf(distinct(100'000))));

    // The repeated session last and far from the first: a scan of neighbours alone would not find it.
    auto repeated = largest;
    repeated.back() = repeated.front();
    CHECK_FALSE(contour::window::parseNodeDrag(tabOf(repeated)));
}

TEST_CASE("a pane dropped on another window's pane edge moves there alive", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const* const pty = &f.left->terminal().device();
    auto const json = f.paneOf(f.left);
    REQUIRE(zoneAt(*f.b, f.other, DropPayload::Pane, 950, 250, json) == DropZone::SplitRight);

    f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitRight), json);

    auto const [window, tab] = placeOf(f.manager, f.left);
    CHECK(window == f.b.id);
    REQUIRE(tab != nullptr);
    CHECK(f.manager.sessionsOfTab(tab) == std::vector { f.other, f.left });
    CHECK(f.manager.model().window(f.a.id)->tabAt(0)->paneCount() == 1);
    requireAlive(f.left, pty, "after the move");
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Pane moved to window %1").arg(f.b.id.value));
    CHECK(f.manager.focusedWindow() == f.b.id);
}

TEST_CASE("a pane dropped beside a pane of its own window says it moved", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.paneOf(f.left);
    REQUIRE(zoneAt(*f.a, f.lone, DropPayload::Pane, 500, 20, json) == DropZone::SplitTop);

    f.a->dropNodeOnPane(f.lone, static_cast<int>(DropZone::SplitTop), json);

    auto const [window, tab] = placeOf(f.manager, f.left);
    CHECK(window == f.a.id);
    CHECK(f.manager.sessionsOfTab(tab) == std::vector { f.left, f.lone });
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(f.saidA) == QStringLiteral("Pane moved"));
}

TEST_CASE("a pane dropped on an edge band of its only sibling re-splits the tab along that edge",
          "[contour][multiwindow][drop]")
{
    // Window A's first tab is [left | right]. Each drop is at the point the policy resolves to the band.
    struct Row
    {
        std::string_view name;
        bool dragsRight; ///< Whether the right pane is dragged onto the left one, or the other way round.
        double x;
        double y;
        DropZone zone;
        vtworkspace::SplitState split;
        bool rightFirst; ///< Whether the right pane is the split's first child after the drop.
    };
    auto const rows = std::array {
        Row { "right onto left's top",
              true,
              500,
              20,
              DropZone::SplitTop,
              vtworkspace::SplitState::Horizontal,
              true },
        Row { "right onto left's bottom",
              true,
              500,
              480,
              DropZone::SplitBottom,
              vtworkspace::SplitState::Horizontal,
              false },
        Row { "right onto left's left",
              true,
              20,
              250,
              DropZone::SplitLeft,
              vtworkspace::SplitState::Vertical,
              true },
        Row { "left onto right's left",
              false,
              20,
              250,
              DropZone::SplitLeft,
              vtworkspace::SplitState::Vertical,
              false },
        Row { "left onto right's right",
              false,
              980,
              250,
              DropZone::SplitRight,
              vtworkspace::SplitState::Vertical,
              true },
        Row { "left onto right's top",
              false,
              500,
              20,
              DropZone::SplitTop,
              vtworkspace::SplitState::Horizontal,
              false },
        Row { "left onto right's bottom",
              false,
              500,
              480,
              DropZone::SplitBottom,
              vtworkspace::SplitState::Horizontal,
              true },
    };
    for (auto const& row: rows)
        DYNAMIC_SECTION(row.name)
        {
            auto f = TwoWindows {};
            auto* const dragged = row.dragsRight ? f.right : f.left;
            auto* const onto = row.dragsRight ? f.left : f.right;
            auto const json = f.paneOf(dragged);
            REQUIRE(zoneAt(*f.a, onto, DropPayload::Pane, row.x, row.y, json) == row.zone);

            f.a->dropNodeOnPane(onto, static_cast<int>(row.zone), json);

            auto const [window, tab] = placeOf(f.manager, dragged);
            CHECK(window == f.a.id);
            REQUIRE(tab != nullptr);
            CHECK(tab->rootPane()->splitState() == row.split);
            CHECK(f.manager.sessionsOfTab(tab)
                  == (row.rightFirst ? std::vector { f.right, f.left } : std::vector { f.left, f.right }));
            CHECK(tab->activePane()->session() == dragged->modelSessionId());
            f.requireNothingRespawned();
        }
}

TEST_CASE("a pane dropped on another pane's middle swaps with it", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.paneOf(f.left);
    REQUIRE(zoneAt(*f.b, f.other, DropPayload::Pane, 500, 250, json) == DropZone::Swap);
    CHECK(f.b->dropGhostLabel(static_cast<int>(DropZone::Swap), static_cast<int>(DropPayload::Pane), f.other)
          == QStringLiteral("Swap with %1").arg(f.other->title()));
    auto const [otherWindowBefore, otherTabBefore] = placeOf(f.manager, f.other);
    auto const [leftWindowBefore, leftTabBefore] = placeOf(f.manager, f.left);

    f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::Swap), json);

    CHECK(placeOf(f.manager, f.left).first == otherWindowBefore);
    CHECK(placeOf(f.manager, f.left).second == otherTabBefore);
    CHECK(placeOf(f.manager, f.other).first == leftWindowBefore);
    CHECK(placeOf(f.manager, f.other).second == leftTabBefore);
    requireAlive(f.left, &f.left->terminal().device(), "swapped in");
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Panes swapped"));
}

TEST_CASE("a pane released on a tab splits right of that tab's active pane", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    f.b->dropNodeOnTab(0, f.paneOf(f.left));

    auto const* const tab = f.manager.model().window(f.b.id)->tabAt(0);
    REQUIRE(tab->paneCount() == 2);
    CHECK(tab->rootPane()->splitState() == vtworkspace::SplitState::Vertical);
    CHECK(f.manager.sessionsOfTab(placeOf(f.manager, f.left).second) == std::vector { f.other, f.left });
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Pane moved to window %1").arg(f.b.id.value));
}

TEST_CASE("a drop while the window's drag runs moves once the drag has returned, and the drag ends once",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto& probe = f.app.dragEndProbe();
    auto const json = f.a->beginPaneDrag(f.left);
    REQUIRE(json == f.paneOf(f.left));
    // The pane is carried, in its own window only.
    CHECK(f.a->carriedPane() == f.left);
    CHECK(f.b->carriedPane() == nullptr);

    SECTION("a drop in another window is held back, the layout stays whole under the source's delegate")
    {
        f.b->dropNodeInGap(1, json);
        QCoreApplication::processEvents();
        CHECK(f.b->count() == 1);
        CHECK(probe.forgotten == 0);

        f.a->finishNodeDrag(Qt::MoveAction);
        CHECK(probe.forgotten == 1);
        // From the event loop, not from inside the delegate's signal.
        CHECK(f.b->count() == 1);
        QCoreApplication::processEvents();
        CHECK(f.b->count() == 2);
        CHECK(placeOf(f.manager, f.left).first == f.b.id);
    }

    SECTION("a drop in the source's own window waits too: the pane's header outlives the drag loop")
    {
        auto const* const tab = f.manager.model().window(f.a.id)->tabAt(0);
        auto const before = tab->rootPane()->splitState();
        f.a->dropNodeOnPane(f.right, static_cast<int>(DropZone::SplitBottom), json);
        QCoreApplication::processEvents();
        CHECK(tab->rootPane()->splitState() == before);
        f.a->finishNodeDrag(Qt::MoveAction);
        CHECK(tab->rootPane()->splitState() == before);
        QCoreApplication::processEvents();
        CHECK(tab->rootPane()->splitState() != before);
        CHECK(probe.forgotten == 1);
    }

    SECTION("the end is told once, however often it is told")
    {
        f.a->finishNodeDrag(Qt::MoveAction);
        f.a->finishNodeDrag(Qt::MoveAction);
        f.a->finishNodeDrag(Qt::IgnoreAction);
        CHECK(probe.forgotten == 1);
        CHECK(f.a->carriedPane() == nullptr);
    }

    SECTION("the next drag ends the one a lost delegate left behind, and is a drag of its own")
    {
        f.b->dropNodeInGap(1, json);
        REQUIRE_FALSE(f.a->beginPaneDrag(f.right).isEmpty());
        CHECK(probe.forgotten == 1);
        CHECK(f.a->carriedPane() == f.right);
        QCoreApplication::processEvents(); // the held drop is made when the stale drag ends
        CHECK(f.b->count() == 2);
        f.a->finishNodeDrag(Qt::MoveAction);
        CHECK(probe.forgotten == 2);
    }

    SECTION("a window that closes with its drag running ends it")
    {
        f.a->closeWindow();
        CHECK(probe.forgotten == 1);
        CHECK(f.a->carriedPane() == nullptr);
        f.a->finishNodeDrag(Qt::MoveAction);
        CHECK(probe.forgotten == 1);
    }
}

TEST_CASE("a pane dropped in a gap of a tab strip becomes a tab there", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    f.b->dropNodeInGap(1, f.paneOf(f.left));

    REQUIRE(f.b->count() == 2);
    auto* const tab = f.manager.model().window(f.b.id)->tabAt(1);
    CHECK(f.manager.sessionsOfTab(tab) == std::vector { f.left });
    CHECK(f.manager.model().window(f.a.id)->tabAt(0)->paneCount() == 1);
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Pane moved to new tab"));
}

TEST_CASE("a pane dropped outside every window opens a window of its own, where it was dropped",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.paneOf(f.left);
    auto* const screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);
    auto& probe = f.app.dragEndProbe();
    probe.over = contour::window::PointerOverContour::No;
    probe.spot =
        contour::window::DropSpot { .screen = screen,
                                    .at = screen->availableGeometry().topLeft() + QPoint { 300, 200 } };
    REQUIRE(f.a->beginPaneDrag(f.left) == json);
    f.a->finishNodeDrag(Qt::IgnoreAction);
    CHECK(probe.forgotten == 1);
    // Nothing moves until the window exists.
    CHECK(placeOf(f.manager, f.left).first == f.a.id);
    CHECK(f.app.app().takePendingSpawnScreen() == screen);

    auto* said = static_cast<contour::test::RecordingAnnouncer*>(nullptr);
    auto const recording =
        [&said](std::function<QObject*()> const&) -> std::unique_ptr<contour::platform::Announcer> {
        auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
        said = announcer.get();
        return announcer;
    };
    ScopedController c { f.manager, recording };
    // It opens where the drag ended, on that screen.
    REQUIRE(c->initialPosition().has_value());
    CHECK(c->initialPosition()->position == QPoint { 300, 200 });
    auto const monitor = static_cast<int>(QGuiApplication::screens().indexOf(screen));
    CHECK(c->initialPosition()->monitor == monitor);
    REQUIRE(f.manager.consumePendingTransplant(c.controller));

    CHECK(placeOf(f.manager, f.left).first == c.id);
    CHECK(c->count() == 1);
    CHECK(f.manager.model().window(f.a.id)->tabAt(0)->paneCount() == 1);
    requireAlive(f.left, &f.left->terminal().device(), "in its own window");
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(said) == QStringLiteral("Pane moved to new window"));
    contour::test::closeAllTabs(*c);
}

TEST_CASE("a tab of several panes moves alive to an edge, a tab, a gap or a window of its own",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.splitTab();
    auto const leftPty = &f.left->terminal().device();

    SECTION("an edge grafts the whole tab beside the pane")
    {
        REQUIRE(zoneAt(*f.b, f.other, DropPayload::Tab, 50, 250, json) == DropZone::SplitLeft);
        f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitLeft), json);
        CHECK(f.manager.model().window(f.b.id)->tabAt(0)->paneCount() == 3);
        CHECK(f.a->count() == 1);
        CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Tab moved"));
    }

    SECTION("the middle of a pane takes no tab")
    {
        CHECK(zoneAt(*f.b, f.other, DropPayload::Tab, 500, 250, json) == DropZone::PassThrough);
        f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::Swap), json);
        CHECK(f.a->count() == 2);
        CHECK(f.manager.model().window(f.b.id)->tabAt(0)->paneCount() == 1);
        CHECK(f.saidB->said.empty());
    }

    SECTION("released on a tab, it splits right of that tab's active pane")
    {
        f.b->dropNodeOnTab(0, json);
        CHECK(f.manager.model().window(f.b.id)->tabAt(0)->paneCount() == 3);
        CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Tab moved"));
    }

    SECTION("a gap takes it as the tab it is")
    {
        f.b->dropNodeInGap(0, json);
        REQUIRE(f.b->count() == 2);
        CHECK(placeOf(f.manager, f.left).second == f.manager.model().window(f.b.id)->tabAt(0));
        CHECK(f.manager.model().window(f.b.id)->tabAt(0)->paneCount() == 2);
        CHECK(TwoWindows::lastSaid(f.saidB) == QStringLiteral("Tab moved"));
    }

    SECTION("a gap of its own strip reorders it")
    {
        auto const tab = placeOf(f.manager, f.left).second;
        f.a->dropNodeInGap(2, json);
        CHECK(f.manager.model().window(f.a.id)->tabAt(1) == tab);
        CHECK(TwoWindows::lastSaid(f.saidA) == QStringLiteral("Tab moved"));
    }

    SECTION("outside every window, a window of its own")
    {
        REQUIRE(f.a->beginTabDrag(0) == json);
        // A tab carries no pane.
        CHECK(f.a->carriedPane() == nullptr);
        f.a->finishNodeDrag(Qt::IgnoreAction);
        ScopedController c { f.manager };
        REQUIRE(f.manager.consumePendingTransplant(c.controller));
        CHECK(placeOf(f.manager, f.left).first == c.id);
        CHECK(placeOf(f.manager, f.right).first == c.id);
        CHECK(f.a->count() == 1);
        requireAlive(f.left, leftPty, "the tab moved");
        f.requireNothingRespawned();
        contour::test::closeAllTabs(*c);
        return;
    }

    requireAlive(f.left, leftPty, "the tab moved");
    f.requireNothingRespawned();
}

TEST_CASE("a drag another process started is refused, and nothing moves", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const foreign = fromAnotherHost(f.paneOf(f.left));
    for (auto const [x, y]: { std::pair { 950.0, 250.0 }, { 500.0, 250.0 } })
        CHECK(zoneAt(*f.b, f.other, DropPayload::Pane, x, y, foreign) == DropZone::Refused);
    CHECK(f.b->refusalText(foreign).toStdString()
          == contour::window::refusalTextOf(MoveRefusal::OtherProcess));
    CHECK(f.b->refusalText(f.paneOf(f.left)).isEmpty());
    CHECK_FALSE(f.b->springOpens(0, foreign));

    f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitRight), foreign);
    f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::Swap), foreign);
    f.b->dropNodeOnTab(0, foreign);
    f.b->dropNodeInGap(0, foreign);
    CHECK(placeOf(f.manager, f.left).first == f.a.id);
    CHECK(f.b->count() == 1);
    CHECK(f.manager.model().window(f.b.id)->tabAt(0)->paneCount() == 1);
    CHECK(f.saidB->said.empty());
    f.requireNothingRespawned();
}

// The manager keeps why it moved nothing: a place's refusal is that refusal, not a model error it resembles,
// both into a window and out of every window; and the model's own refusals stay the model's.
TEST_CASE("a move nothing makes keeps the reason it was refused for", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const foreign = contour::window::parseNodeDrag(fromAnotherHost(f.paneOf(f.left)));
    REQUIRE(foreign.has_value());
    auto const refused = contour::session::MoveFailure { MoveRefusal::OtherProcess };

    auto const beside = f.manager.moveNode(
        *foreign,
        vtworkspace::EdgeTarget { .pane = f.other->modelSessionId(), .side = vtworkspace::SplitSide::Right },
        nullptr,
        {});
    REQUIRE_FALSE(beside.has_value());
    CHECK(beside.error() == refused);
    auto const outOfEvery = f.manager.moveNode(*foreign, vtworkspace::NewWindowTarget {}, nullptr, {});
    REQUIRE_FALSE(outOfEvery.has_value());
    CHECK(outOfEvery.error() == refused);

    auto const own = contour::window::parseNodeDrag(f.paneOf(f.left));
    REQUIRE(own.has_value());
    auto const ontoItself =
        f.manager.moveNode(*own, vtworkspace::SwapTarget { .pane = f.left->modelSessionId() }, nullptr, {});
    REQUIRE_FALSE(ontoItself.has_value());
    CHECK(ontoItself.error() == contour::session::MoveFailure { vtworkspace::MoveError::SelfTarget });
    CHECK(placeOf(f.manager, f.left).first == f.a.id);
    f.requireNothingRespawned();
}
TEST_CASE("a drag whose pane closed since it began is taken, and nothing moves",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.a->beginPaneDrag(f.lone);
    REQUIRE_FALSE(json.isEmpty());
    // Resolved as the drag began: a hover reads that back, until the layout changes under it.
    CHECK(zoneAt(*f.b, f.other, DropPayload::Pane, 950, 250, json) == DropZone::SplitRight);
    f.manager.removeSession(*f.lone); // the shell exits while its card is dragged
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(f.a->count() == 1);
    CHECK(f.a->carriedPane() == nullptr);

    CHECK(zoneAt(*f.b, f.other, DropPayload::Pane, 950, 250, json) == DropZone::NoOp);
    f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitRight), json);
    f.b->dropNodeInGap(1, json);
    f.a->finishNodeDrag(Qt::IgnoreAction);
    QCoreApplication::processEvents();
    CHECK(f.b->count() == 1);
    CHECK(f.manager.model().window(f.b.id)->tabAt(0)->paneCount() == 1);
    ScopedController c { f.manager };
    CHECK_FALSE(f.manager.consumePendingTransplant(c.controller));
    contour::test::closeAllTabs(*c);
}

TEST_CASE("moving out the last pane closes its window, and the app keeps running",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const pty = &f.other->terminal().device();
    f.a->dropNodeOnPane(f.lone, static_cast<int>(DropZone::SplitBottom), f.paneOf(f.other));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    CHECK(f.manager.model().window(f.b.id) == nullptr);
    CHECK(f.manager.controllerFor(f.b.id) == nullptr);
    CHECK(f.manager.controllerFor(f.a.id) != nullptr);
    CHECK(placeOf(f.manager, f.other).first == f.a.id);
    requireAlive(f.other, pty, "still running");
    f.requireNothingRespawned();
    CHECK(TwoWindows::lastSaid(f.saidA) == QStringLiteral("Pane moved to window %1").arg(f.a.id.value));
}

TEST_CASE("a drag that ends cancelled, or taken by a window, opens nothing", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.paneOf(f.left);
    // Escape inside a window: Qt says IgnoreAction, as for a drop outside, and QML cannot tell; the pointer
    // being over a Contour window as it ended does.
    f.app.dragEndProbe().over = contour::window::PointerOverContour::Yes;
    REQUIRE(f.a->beginPaneDrag(f.left) == json);
    f.a->finishNodeDrag(Qt::IgnoreAction);
    f.app.dragEndProbe().over = contour::window::PointerOverContour::No;
    // A drop some window recognised did its work there, refusals and no-ops included.
    REQUIRE(f.a->beginPaneDrag(f.left) == json);
    f.a->finishNodeDrag(Qt::MoveAction);
    // The whole of a window torn off would only recreate itself.
    REQUIRE_FALSE(f.b->beginPaneDrag(f.other).isEmpty());
    f.b->finishNodeDrag(Qt::IgnoreAction);

    ScopedController c { f.manager };
    CHECK_FALSE(f.manager.consumePendingTransplant(c.controller));
    CHECK(placeOf(f.manager, f.left).first == f.a.id);
    CHECK(placeOf(f.manager, f.other).first == f.b.id);
    contour::test::closeAllTabs(*c);
}

TEST_CASE("a pane over itself is a no-op, and its own tab never splits for its tab",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    CHECK(zoneAt(*f.a, f.left, DropPayload::Pane, 950, 250, f.paneOf(f.left)) == DropZone::NoOp);
    CHECK(zoneAt(*f.a, f.right, DropPayload::Pane, 950, 250, f.paneOf(f.left)) == DropZone::SplitRight);
    CHECK(zoneAt(*f.a, f.right, DropPayload::Tab, 950, 250, f.splitTab()) == DropZone::PassThrough);
    f.a->dropNodeOnPane(f.left, static_cast<int>(DropZone::SplitRight), f.paneOf(f.left));
    f.a->dropNodeOnPane(f.right, static_cast<int>(DropZone::SplitRight), f.splitTab());
    CHECK(f.a->count() == 2);
    CHECK(f.manager.model().window(f.a.id)->tabAt(0)->paneCount() == 2);
    CHECK(f.saidA->said.empty());
}

TEST_CASE("dwelling opens another tab, never the one the drag came from", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    CHECK_FALSE(f.a->springOpens(0, f.paneOf(f.left))); // the tab the pane came from
    CHECK(f.a->springOpens(1, f.paneOf(f.left)));       // another tab
    CHECK_FALSE(f.a->springOpens(0, f.splitTab()));     // the tab dragged
    CHECK_FALSE(f.a->springOpens(1, f.a->tabDragPayload(1)));
    CHECK(f.b->springOpens(0, f.splitTab()));
    CHECK_FALSE(f.b->springOpens(5, f.splitTab()));
    CHECK_FALSE(f.b->springOpens(0, QStringLiteral("{}")));
}

TEST_CASE("the tab strip is hit-tested against the spans it reports", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    using contour::window::TabBarSpot;
    auto const hit = [&](qreal x) {
        auto const answer = f.a->tabBarHit(x).hit();
        return std::pair { answer.spot, answer.index };
    };
    CHECK(hit(50) == std::pair { TabBarSpot::Gap, 0 });
    f.a->setTabSpans({ QRectF { 0, 0, 100, 30 }, QRectF { 100, 0, 100, 30 } });
    CHECK(hit(2) == std::pair { TabBarSpot::Gap, 0 });
    CHECK(hit(50) == std::pair { TabBarSpot::OnTab, 0 });
    CHECK(hit(100) == std::pair { TabBarSpot::Gap, 1 });
    CHECK(hit(150) == std::pair { TabBarSpot::OnTab, 1 });
    CHECK(hit(300) == std::pair { TabBarSpot::Gap, 2 });
}

TEST_CASE("QML reads a tab-strip hit as a value with a named spot", "[contour][multiwindow][drop][qml]")
{
    contour::window::registerTabBarQmlTypes();
    auto f = TwoWindows {};
    f.a->setTabSpans({ QRectF { 0, 0, 100, 30 }, QRectF { 100, 0, 100, 30 } });
    auto engine = QQmlEngine {};
    engine.rootContext()->setContextProperty(QStringLiteral("win"), f.a.controller);
    auto component = QQmlComponent { &engine };
    component.setData(R"(
        import QtQml
        import Contour.Terminal
        QtObject {
            readonly property var onTab: win.tabBarHit(150)
            readonly property var gap: win.tabBarHit(100)
            readonly property bool onTabNamed: onTab.spot === TabBarSpot.OnTab
            readonly property bool gapNamed: gap.spot === TabBarSpot.Gap
            readonly property int onTabIndex: onTab.index
            readonly property int gapIndex: gap.index
        }
    )",
                      QUrl {});
    auto const root = std::unique_ptr<QObject>(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(root != nullptr);
    CHECK(root->property("onTabNamed").toBool());
    CHECK(root->property("gapNamed").toBool());
    CHECK(root->property("onTabIndex").toInt() == 1);
    CHECK(root->property("gapIndex").toInt() == 1);
}

TEST_CASE("a tmux window or an old daemon refuses a move with its reason", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const json = f.paneOf(f.left);
    f.factory->reportedHosting = contour::session::SessionHosting::Remote;

    SECTION("tmux")
    {
        f.factory->reportedHostKind = contour::session::SessionHostKind::Tmux;
        CHECK(zoneAt(*f.b, f.other, DropPayload::Pane, 950, 250, json) == DropZone::Refused);
        CHECK(f.b->refusalText(json).toStdString() == contour::window::refusalTextOf(MoveRefusal::Tmux));
    }

    SECTION("a daemon that does not move panes")
    {
        f.factory->reportedHostKind = contour::session::SessionHostKind::Daemon;
        f.factory->reportedDaemonInstance = 7;
        auto const daemonDrag = f.b->paneDragPayload(f.other);
        REQUIRE(contour::window::parseNodeDrag(daemonDrag)->host
                == contour::window::HostIdentity { .kind = HostKind::Daemon, .id = QStringLiteral("7") });
        CHECK(zoneAt(*f.a, f.left, DropPayload::Pane, 950, 250, daemonDrag) == DropZone::Refused);
        CHECK(f.a->refusalText(daemonDrag).toStdString()
              == contour::window::refusalTextOf(MoveRefusal::OldDaemon));
        // The window's own process, hosted by a daemon now: the drag of before names another host.
        CHECK(f.a->refusalText(json).toStdString()
              == contour::window::refusalTextOf(MoveRefusal::OtherProcess));
    }

    f.a->dropNodeOnPane(f.right, static_cast<int>(DropZone::SplitRight), f.a->paneDragPayload(f.left));
    CHECK(f.factory->requestedMoves.empty());
    CHECK(f.manager.model().window(f.a.id)->tabAt(0)->paneCount() == 2);
}

namespace
{
/// TwoWindows hosted by a daemon older than MoveNode, as v0.8.0 is.
void hostOnAnOldDaemon(TwoWindows& f)
{
    f.factory->reportedHosting = contour::session::SessionHosting::Remote;
    f.factory->reportedHostKind = contour::session::SessionHostKind::Daemon;
    f.factory->reportedMoveSupport = contour::session::MoveSupport::Unsupported;
}
} // namespace

TEST_CASE("a daemon older than MoveNode keeps the tab moves it served, and refuses the rest",
          "[contour][multiwindow][drop][remote]")
{
    auto f = TwoWindows {};
    hostOnAnOldDaemon(f);

    SECTION("a tab dragged into another window's strip moves there")
    {
        f.b->dropNodeInGap(0, f.a->tabDragPayload(1));
        CHECK(placeOf(f.manager, f.lone).first == f.b.id);
        CHECK(f.b->count() == 2);
    }

    SECTION("a tab of several panes dragged into another window's strip moves there whole")
    {
        f.b->dropNodeInGap(1, f.splitTab());
        CHECK(placeOf(f.manager, f.left).first == f.b.id);
        CHECK(placeOf(f.manager, f.right).first == f.b.id);
    }

    SECTION("a tab reordered in its own strip")
    {
        f.a->dropNodeInGap(0, f.a->tabDragPayload(1));
        CHECK(f.manager.model().window(f.a.id)->indexOf(placeOf(f.manager, f.lone).second->id()) == 0);
    }

    SECTION("a tab dropped outside every window tears off")
    {
        REQUIRE_FALSE(f.a->beginTabDrag(1).isEmpty());
        f.a->finishNodeDrag(Qt::IgnoreAction);
        ScopedController c { f.manager };
        REQUIRE(f.manager.consumePendingTransplant(c.controller));
        CHECK(placeOf(f.manager, f.lone).first == c.id);
        f.requireNothingRespawned();
        contour::test::closeAllTabs(*c);
    }

    SECTION("a pane of a tab of several is refused everywhere, as too old")
    {
        auto const json = f.paneOf(f.left);
        CHECK(zoneAt(*f.b, f.other, DropPayload::Pane, 950, 250, json) == DropZone::Refused);
        CHECK(f.b->refusalText(json).toStdString() == contour::window::refusalTextOf(MoveRefusal::OldDaemon));
        f.b->dropNodeInGap(0, json);
        f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitRight), json);
        REQUIRE(f.a->beginPaneDrag(f.left) == json);
        f.a->finishNodeDrag(Qt::IgnoreAction);
        ScopedController c { f.manager };
        CHECK_FALSE(f.manager.consumePendingTransplant(c.controller));
        CHECK(placeOf(f.manager, f.left).first == f.a.id);
        contour::test::closeAllTabs(*c);
    }

    SECTION("a whole tab is refused onto a tab, a pane's middle, and an edge")
    {
        auto const json = f.a->tabDragPayload(1);
        f.a->dropNodeOnTab(0, json);
        CHECK(zoneAt(*f.a, f.left, DropPayload::Pane, 500, 250, json) == DropZone::Refused);
        CHECK(zoneAt(*f.a, f.left, DropPayload::Pane, 950, 250, json) == DropZone::Refused);
        f.a->dropNodeOnPane(f.left, static_cast<int>(DropZone::SplitRight), json);
        CHECK(f.a->count() == 2);
    }

    CHECK(f.factory->requestedMoves.empty());
    CHECK(f.factory->createdPtys.size() == f.ptysBefore);
}

TEST_CASE("a daemon that moves panes is not asked to tear off the whole of a window",
          "[contour][multiwindow][drop][remote]")
{
    auto f = TwoWindows {};
    f.factory->reportedHosting = contour::session::SessionHosting::Remote;
    f.factory->reportedHostKind = contour::session::SessionHostKind::Daemon;
    f.factory->reportedMoveSupport = contour::session::MoveSupport::Supported;
    // Window B's only tab, of one pane: torn off, it would only recreate its window.
    REQUIRE_FALSE(f.b->beginPaneDrag(f.other).isEmpty());
    f.b->finishNodeDrag(Qt::IgnoreAction);
    REQUIRE_FALSE(f.b->beginTabDrag(0).isEmpty());
    f.b->finishNodeDrag(Qt::IgnoreAction);
    CHECK(f.factory->requestedMoves.empty());
    // A pane of a tab of several is asked for.
    REQUIRE_FALSE(f.a->beginPaneDrag(f.left).isEmpty());
    f.a->finishNodeDrag(Qt::IgnoreAction);
    REQUIRE(f.factory->requestedMoves.size() == 1);
    CHECK(f.factory->requestedMoves.front().target == vtworkspace::MoveTargetKind::NewWindow);
}

TEST_CASE("a daemon that moves panes is asked, by its ids, and nothing moves until it pushes",
          "[contour][multiwindow][drop][remote]")
{
    auto f = TwoWindows {};
    f.factory->reportedHosting = contour::session::SessionHosting::Remote;
    f.factory->reportedHostKind = contour::session::SessionHostKind::Daemon;
    f.factory->reportedDaemonInstance = 7;
    f.factory->reportedMoveSupport = contour::session::MoveSupport::Supported;
    auto const json = f.paneOf(f.left);
    auto const hostId = [&](contour::session::TerminalSession const* session) {
        return f.factory->hostSessionIdOf(session->terminal().device()).value();
    };
    REQUIRE(contour::window::parseNodeDrag(json)->sessions == std::vector { hostId(f.left) });
    REQUIRE(zoneAt(*f.b, f.other, DropPayload::Pane, 950, 250, json) == DropZone::SplitRight);

    f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitRight), json);
    f.b->dropNodeInGap(1, json);
    f.a->dropNodeOnTab(1, f.splitTab());

    REQUIRE(f.factory->requestedMoves.size() == 3);
    auto const& edge = f.factory->requestedMoves[0];
    CHECK(edge.source == vtworkspace::MoveSourceKind::Pane);
    CHECK(edge.sourceSessions == std::vector { hostId(f.left) });
    CHECK(edge.target == vtworkspace::MoveTargetKind::Edge);
    CHECK(edge.targetSession == hostId(f.other));
    CHECK(edge.side == vtworkspace::SplitSide::Right);
    auto const& gap = f.factory->requestedMoves[1];
    CHECK(gap.target == vtworkspace::MoveTargetKind::TabWell);
    CHECK(gap.targetSession == hostId(f.other));
    CHECK(gap.index == 1);
    auto const& tab = f.factory->requestedMoves[2];
    CHECK(tab.source == vtworkspace::MoveSourceKind::Tab);
    CHECK(tab.sourceSessions == std::vector { hostId(f.right), hostId(f.left) });
    CHECK(tab.targetSession == hostId(f.lone));

    CHECK(placeOf(f.manager, f.left).first == f.a.id);
    CHECK(f.b->count() == 1);
    CHECK(f.a->count() == 2);
    f.requireNothingRespawned();
}

TEST_CASE("a move asked of the daemon is waited for until refused, timed out or the connection is lost",
          "[contour][multiwindow][drop][remote]")
{
    auto f = TwoWindows {};
    f.factory->reportedHosting = contour::session::SessionHosting::Remote;
    f.factory->reportedHostKind = contour::session::SessionHostKind::Daemon;
    f.factory->reportedMoveSupport = contour::session::MoveSupport::Supported;
    auto const ask = [&] {
        f.b->dropNodeOnPane(f.other, static_cast<int>(DropZone::SplitRight), f.paneOf(f.left));
        REQUIRE((f.manager.livePendingRemoteMove() != nullptr));
        return f.factory->requestedMoves.back().request;
    };

    SECTION("refused")
    {
        auto const request = ask();
        CHECK(request != 0);
        // Another request's refusal is not this one's.
        f.manager.remoteMoveRefused(request + 1);
        CHECK((f.manager.livePendingRemoteMove() != nullptr));
        f.manager.remoteMoveRefused(request);
        CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
    }

    SECTION("timed out")
    {
        std::ignore = ask();
        f.app.steadyClock().advance(contour::session::TerminalSessionManager::RemoteMoveTimeout);
        CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
        // Its push landing late opens and announces nothing.
        auto const late = std::array { f.factory->requestedMoves.back().request };
        f.manager.noteRemoteMovesLanded(late);
        f.manager.completeRemoteMoves(contour::session::TerminalSessionManager::ReconcileReach::Whole);
        CHECK(TwoWindows::lastSaid(f.saidB).isEmpty());
    }

    SECTION("the connection lost")
    {
        std::ignore = ask();
        f.manager.setHostConnection(vthost::client::ConnectionState::Reconnecting);
        CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
        f.manager.setHostConnection(vthost::client::ConnectionState::Connected);
    }

    SECTION("each request has a number of its own")
    {
        auto const first = ask();
        auto const second = ask();
        CHECK(second != first);
    }
}

TEST_CASE("a drop onto a pane whose shell is not known refuses a name that could run, with a toast",
          "[contour][multiwindow][drop]")
{
    // The mock pty has no foreground process to name, as a Windows or daemon-hosted pane has none.
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>());
    ScopedController window { app.manager() };
    window->createNewTab();
    auto* const pane = window->activeSession();
    REQUIRE(pane != nullptr);
    REQUIRE_FALSE(window->notices().toastShown());

    pane->insertDroppedUrls({ QUrl(QStringLiteral("file:///tmp/x%24%28id%29")) }); // /tmp/x$(id)
    REQUIRE(window->notices().toastShown());
    CHECK(window->notices().toastText().contains(QStringLiteral("shell is not known")));
}

TEST_CASE("a QML call reaches the real dropZoneAt and dropNodeOnPane with a session object",
          "[contour][multiwindow][drop][qml]")
{
    // Every other QML test runs against a controller mock that takes a QObject*. The real controller takes a
    // session::TerminalSession const*: if the engine did not convert, the call would fail and the zone
    // would be `undefined` -- which PaneCard must refuse, not accept.
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>());
    ScopedController window { app.manager() };
    window->createNewTab();
    window->createNewTab(); // [t0 (dragged), t1], t1 active
    auto* const pane = window->activeSession();
    REQUIRE(pane != nullptr);

    auto engine = QQmlEngine {};
    engine.rootContext()->setContextProperty("controller", window.controller);
    engine.rootContext()->setContextProperty("pane", pane);
    auto component = QQmlComponent { &engine };
    component.setData(R"(
        import QtQml
        QtObject {
            function zone(payload, x, y, node) {
                return controller.dropZoneAt(pane, payload, x, y, 1000, 500, node)
            }
            function label(zone, payload) { return controller.dropGhostLabel(zone, payload, pane) }
            function drop(zone, node) { controller.dropNodeOnPane(pane, zone, node) }
            function payload(row) {
                // A drag, begun and taken, so the drop below is not held back for it.
                const node = controller.beginTabDrag(row)
                controller.finishNodeDrag(Qt.MoveAction)
                return node
            }
        })",
                      QUrl {});
    REQUIRE(component.isReady());
    auto const probe = std::unique_ptr<QObject> { component.create() };
    REQUIRE(probe != nullptr);
    auto const zoneFromQml = [&](DropPayload payload, double x, double y, QString const& node) {
        auto answer = QVariant {};
        REQUIRE(QMetaObject::invokeMethod(probe.get(),
                                          "zone",
                                          Q_RETURN_ARG(QVariant, answer),
                                          Q_ARG(QVariant, static_cast<int>(payload)),
                                          Q_ARG(QVariant, x),
                                          Q_ARG(QVariant, y),
                                          Q_ARG(QVariant, node)));
        return answer;
    };

    auto fromQml = QVariant {};
    REQUIRE(QMetaObject::invokeMethod(
        probe.get(), "payload", Q_RETURN_ARG(QVariant, fromQml), Q_ARG(QVariant, 0)));
    auto const node = fromQml.toString();
    REQUIRE(contour::window::parseNodeDrag(node).has_value());
    // A number, not undefined: the call was made, and it is the C++ answer.
    auto const files = zoneFromQml(DropPayload::Files, 500, 250, {});
    REQUIRE(files.isValid());
    CHECK(files.toInt() == static_cast<int>(zoneAt(*window, pane, DropPayload::Files, 500, 250)));
    CHECK(files.toInt() == static_cast<int>(DropZone::InsertText));
    auto const split = zoneFromQml(DropPayload::Pane, 950, 250, node);
    REQUIRE(split.isValid());
    CHECK(split.toInt() == static_cast<int>(DropZone::SplitRight));
    auto label = QVariant {};
    REQUIRE(QMetaObject::invokeMethod(probe.get(),
                                      "label",
                                      Q_RETURN_ARG(QVariant, label),
                                      Q_ARG(QVariant, static_cast<int>(DropZone::SplitRight)),
                                      Q_ARG(QVariant, static_cast<int>(DropPayload::Pane))));
    CHECK(label.toString() == QStringLiteral("Pane → split right"));

    // And the drop reaches the model: the dragged pane is moved beside the hovered one.
    REQUIRE(window->count() == 2);
    QMetaObject::invokeMethod(
        probe.get(), "drop", Q_ARG(QVariant, static_cast<int>(DropZone::SplitRight)), Q_ARG(QVariant, node));
    CHECK(window->count() == 1);

    window->closeTabAtIndex(0);
}

TEST_CASE(
    "WindowController: dropPayloadOf and dragTitle read a node drag for a DropArea that has only its JSON",
    "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const title = f.left->title();
    REQUIRE_FALSE(title.isEmpty());

    SECTION("the kind the JSON says it is")
    {
        CHECK(f.a->dropPayloadOf(f.paneOf(f.left)) == static_cast<int>(DropPayload::Pane));
        CHECK(f.a->dropPayloadOf(f.splitTab()) == static_cast<int>(DropPayload::Tab));
        // A tab of one pane drags as that pane.
        CHECK(f.a->dropPayloadOf(f.a->tabDragPayload(1)) == static_cast<int>(DropPayload::Pane));
    }

    SECTION("JSON that is no node drag is none")
    {
        CHECK(f.a->dropPayloadOf(QStringLiteral("not json")) == static_cast<int>(DropPayload::None));
        CHECK(f.a->dropPayloadOf({}) == static_cast<int>(DropPayload::None));
    }

    SECTION("a drag names what it moves: the pane, or the first pane of the tab")
    {
        CHECK(f.a->dragTitle(f.paneOf(f.left)) == title);
        CHECK(f.a->dragTitle(f.paneOf(f.right)) == f.right->title());
        CHECK((f.a->dragTitle(f.splitTab()) == title || f.a->dragTitle(f.splitTab()) == f.right->title()));
        // Another window of this process names it as well.
        CHECK(f.b->dragTitle(f.paneOf(f.left)) == title);
    }

    SECTION("a drag of another host, or none, names nothing")
    {
        CHECK(f.a->dragTitle(fromAnotherHost(f.paneOf(f.left))).isEmpty());
        CHECK(f.a->dragTitle(QStringLiteral("not json")).isEmpty());
    }
}

TEST_CASE("WindowController: dropZoneAt answers from the policy", "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const tab = f.splitTab();

    SECTION("a tab from another tab splits on the outer edges and is left alone elsewhere")
    {
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 950, 250, tab) == DropZone::SplitRight);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 50, 250, tab) == DropZone::SplitLeft);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 500, 20, tab) == DropZone::SplitTop);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 500, 480, tab) == DropZone::SplitBottom);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 500, 250, tab) == DropZone::PassThrough);
    }

    SECTION("a tab of one pane drags as that pane, whatever the card guessed")
    {
        CHECK(zoneAt(*f.a, f.left, DropPayload::Tab, 500, 250, f.a->tabDragPayload(1)) == DropZone::Swap);
    }

    SECTION("a tab from the pane's own tab is never split")
    {
        CHECK(zoneAt(*f.a, f.left, DropPayload::Tab, 950, 250, tab) == DropZone::PassThrough);
        CHECK(zoneAt(*f.a, f.left, DropPayload::Tab, 500, 250, tab) == DropZone::PassThrough);
    }

    SECTION("files and text insert anywhere over the pane")
    {
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Files, 950, 250) == DropZone::InsertText);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Files, 500, 250) == DropZone::InsertText);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Text, 50, 20) == DropZone::InsertText);
    }

    SECTION("a payload that does not parse is no drop, and one naming nothing of ours is taken as a no-op")
    {
        // Review: the JSON comes from whatever started the drag.
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 950, 250, QStringLiteral("not json")) == DropZone::None);
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 950, 250) == DropZone::None);
        auto stale = contour::window::parseNodeDrag(tab).value();
        stale.sessions = { 9998, 9999 };
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Tab, 950, 250, contour::window::writeNodeDrag(stale))
              == DropZone::NoOp);
    }

    SECTION("the strip's old tab payload is no drag any more")
    {
        // The strip drags the node now: what it used to write names a window and a row, and is taken for
        // nothing.
        CHECK(zoneAt(*f.a,
                     f.lone,
                     DropPayload::Tab,
                     950,
                     250,
                     QStringLiteral(R"({"windowId":1,"tabIndex":0,"token":"%1"})")
                         .arg(contour::window::processToken()))
              == DropZone::None);
    }

    SECTION("no session, and nothing out of range, is a target")
    {
        CHECK(
            f.a->dropZoneAt(nullptr, static_cast<int>(DropPayload::Files), 10, 10, PaneWidth, PaneHeight, {})
            == static_cast<int>(DropZone::None));
        CHECK(f.a->dropZoneAt(f.lone, -1, 10, 10, PaneWidth, PaneHeight, {})
              == static_cast<int>(DropZone::None));
        CHECK(f.a->dropZoneAt(f.lone, 99, 10, 10, PaneWidth, PaneHeight, {})
              == static_cast<int>(DropZone::None));
        CHECK(zoneAt(*f.a, f.lone, DropPayload::Files, 1500, 250) == DropZone::None);
    }

    SECTION("the ghost's label is the zone's row, for what is dragged")
    {
        auto const label = [&](DropZone zone, DropPayload payload) {
            return f.a->dropGhostLabel(static_cast<int>(zone), static_cast<int>(payload), f.lone);
        };
        CHECK(label(DropZone::SplitRight, DropPayload::Tab) == QStringLiteral("Tab → split right"));
        CHECK(label(DropZone::SplitTop, DropPayload::Pane) == QStringLiteral("Pane → split up"));
        CHECK(label(DropZone::Swap, DropPayload::Tab).isEmpty());
        CHECK(label(DropZone::InsertText, DropPayload::Files).isEmpty());
        CHECK(f.a->dropGhostLabel(-1, static_cast<int>(DropPayload::Pane), f.lone).isEmpty());
        CHECK(f.a->dropGhostLabel(99, static_cast<int>(DropPayload::Pane), f.lone).isEmpty());
        CHECK(f.a->dropGhostLabel(static_cast<int>(DropZone::SplitLeft), 99, f.lone).isEmpty());
    }

    SECTION("the ghost is the half on the zone's side, and a zone that is not a split has none")
    {
        constexpr auto Inset = 6.0;
        auto const rect = [&](DropZone zone) {
            return f.a->dropGhostRect(static_cast<int>(zone), PaneWidth, PaneHeight, Inset);
        };
        auto const half = contour::window::dropGhostOf(
            DropZone::SplitRight,
            contour::window::DropExtent { .width = PaneWidth, .height = PaneHeight },
            Inset);
        REQUIRE(half.has_value());
        CHECK(rect(DropZone::SplitRight) == QRectF { half->x, half->y, half->width, half->height });
        for (auto const zone:
             { DropZone::SplitLeft, DropZone::SplitRight, DropZone::SplitTop, DropZone::SplitBottom })
            CHECK_FALSE(rect(zone).isNull());
        for (auto const zone: { DropZone::None, DropZone::InsertText, DropZone::PassThrough })
            CHECK(rect(zone).isNull());
        CHECK(f.a->dropGhostRect(-1, PaneWidth, PaneHeight, Inset).isNull());
        CHECK(f.a->dropGhostRect(99, PaneWidth, PaneHeight, Inset).isNull());
    }
}

TEST_CASE("WindowController: dropNodeOnPane moves on a split or swap zone and changes nothing otherwise",
          "[contour][multiwindow][drop]")
{
    auto f = TwoWindows {};
    auto const tab = f.splitTab();
    auto const loneId = f.lone->modelSessionId();

    SECTION("a split zone grafts the dragged tab on that side")
    {
        f.a->dropNodeOnPane(f.lone, static_cast<int>(DropZone::SplitRight), tab);
        REQUIRE(f.a->count() == 1);
        auto const* const merged = f.manager.model().window(f.a.id)->activeTab();
        REQUIRE(merged != nullptr);
        REQUIRE(merged->rootPane()->splitState() == vtworkspace::SplitState::Vertical);
        CHECK(merged->rootPane()->first()->session() == loneId);
    }

    SECTION("the zone names the side")
    {
        f.a->dropNodeOnPane(f.lone, static_cast<int>(DropZone::SplitTop), tab);
        auto const* const merged = f.manager.model().window(f.a.id)->activeTab();
        REQUIRE(merged != nullptr);
        REQUIRE(merged->rootPane()->splitState() == vtworkspace::SplitState::Horizontal);
        CHECK(merged->rootPane()->second()->session() == loneId);
    }

    SECTION("a zone that moves nothing changes nothing")
    {
        for (auto const zone: { DropZone::None,
                                DropZone::InsertText,
                                DropZone::PassThrough,
                                DropZone::NoOp,
                                DropZone::Refused,
                                DropZone::Swap })
            f.a->dropNodeOnPane(f.lone, static_cast<int>(zone), tab);
        f.a->dropNodeOnPane(f.lone, -1, tab);
        f.a->dropNodeOnPane(f.lone, 99, tab);
        f.a->dropNodeOnPane(f.lone, static_cast<int>(DropZone::SplitRight), QStringLiteral("{}"));
        f.a->dropNodeOnPane(nullptr, static_cast<int>(DropZone::SplitRight), tab);
        f.a->dropNodeOnPane(f.lone, static_cast<int>(DropZone::SplitRight), fromAnotherHost(tab));
        CHECK(f.a->count() == 2);
        CHECK(f.manager.model().window(f.a.id)->tabAt(1)->paneCount() == 1);
    }
}

TEST_CASE("WindowController: a pane that has ended takes no files", "[contour][multiwindow][drop]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* const factory = factoryOwned.get();
    TestApp app(std::move(factoryOwned));
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    auto* const live = window->activeSession();

    // The next tab's session fails to start, which ends the pane where it stands.
    factory->makePty = [](vtbackend::PageSize size) {
        return std::make_unique<contour::test::ConfigurableStartPty>(
            size, contour::test::ConfigurableStartPty::StartBehavior::Fail);
    };
    window->createNewTab();
    auto* const ended = window->activeSession();
    REQUIRE(ended != nullptr);
    ended->start(); // a display would have started it
    REQUIRE(ended->getSessionEnd() == static_cast<int>(contour::session::EndReason::StartFailed));

    CHECK(zoneAt(*window, live, DropPayload::Files, 500, 250) == DropZone::InsertText);
    CHECK(zoneAt(*window, ended, DropPayload::Files, 500, 250) == DropZone::None);
    CHECK(zoneAt(*window, ended, DropPayload::Text, 500, 250) == DropZone::None);
    // A pane may still be moved beside an ended pane: nothing is typed to it.
    CHECK(zoneAt(*window, ended, DropPayload::Pane, 950, 250, window->paneDragPayload(live))
          == DropZone::SplitRight);

    factory->makePty = nullptr;
    contour::test::closeAllTabs(*window);
}

TEST_CASE("WindowController: a refused drop tells the window", "[contour][multiwindow][drop]")
{
    TestApp app(std::make_unique<contour::test::MockPtySessionFactory>());
    auto& manager = app.manager();
    ScopedController window { manager };
    window->createNewTab();
    auto* const pane = window->activeSession();
    REQUIRE(window->notices().toastText().isEmpty());

    pane->insertDroppedUrls({ QUrl(QStringLiteral("file:///tmp/a%0Ab")) });
    CHECK(window->notices().toastText()
          == QStringLiteral("Not inserted: a file name holds a control or text-direction character."));

    // A text drop with a line break onto a pane without bracketed paste says why nothing was typed.
    pane->insertDroppedText(QStringLiteral("a\nb"));
    CHECK(window->notices().toastText().contains(QStringLiteral("line break")));

    // The toast names what was dropped: a link is not a file name.
    pane->insertDroppedUrls({ QUrl(QStringLiteral("https://a/b;c")) });
    CHECK(window->notices().toastText()
          == QStringLiteral("Not inserted: a link holds a character that could run as a command, and this "
                            "pane's shell is not known."));
    pane->insertDroppedUrls({ QUrl(QStringLiteral("https://a/b;c")), QUrl(QStringLiteral("file:///tmp/x")) });
    CHECK(window->notices().toastText()
          == QStringLiteral("Not inserted: a name holds a character that could run as a command, and this "
                            "pane's shell is not known."));

    contour::test::closeAllTabs(*window);
}

// }}}
