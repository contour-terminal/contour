// SPDX-License-Identifier: Apache-2.0
//
// The quick terminal's window as the app builds it: its role, flags, density, and what is never remembered.
// Headless: tabs are minted straight through the model, windows are bare QQuickWindows.

#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/ChromeStyleHost.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/WindowController.hpp>
#include <contour/window/WindowRole.hpp>

#include <QtCore/Qt>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

using contour::platform::HotkeyError;
using contour::test::ScopedController;
using contour::test::TestApp;
using contour::window::WindowRole;

TEST_CASE("QuickTerminalWindow.aStagedRoleIsTheNextWindowsAndOnlyThat", "[quickterminal][window]")
{
    auto app = TestApp {};
    app.manager().stageWindowRole(WindowRole::QuickTerminal);
    auto const quick = ScopedController { app.manager() };
    CHECK(quick.controller->role() == WindowRole::QuickTerminal);
    CHECK(quick.controller->windowRoleForQml() == 1);
    CHECK(quick.controller->roleFlags() == static_cast<int>(Qt::Tool | Qt::FramelessWindowHint));

    auto const normal = ScopedController { app.manager() };
    CHECK(normal.controller->role() == WindowRole::Normal);
    CHECK(normal.controller->roleFlags() == 0);
}

TEST_CASE("QuickTerminalWindow.theQuickWindowIsNeverRemembered", "[quickterminal][window]")
{
    auto app = TestApp {};
    app.app().config().rememberWindowPosition = true;
    app.app().config().configFile = "test-config/contour.yml";
    auto& store = static_cast<contour::test::InMemoryWindowPositionStore&>(app.app().windowPositionStore());
    store.value = contour::window::WindowPosition { .position = { 20, 20 }, .monitor = 0 };

    app.manager().stageWindowRole(WindowRole::QuickTerminal);
    auto quick = ScopedController { app.manager() };
    auto osWindow = QQuickWindow {};
    osWindow.resize(200, 100);
    quick.controller->bindWindow(&osWindow);
    quick.controller->showInitial();
    osWindow.setPosition(80, 90);
    quick.controller->closeWindow();

    // Not restored from the store, and not saved to it.
    CHECK(store.loads == 0);
    CHECK(store.saves == 0);
    REQUIRE(store.value);
    CHECK(store.value->position == QPoint(20, 20));
}

TEST_CASE("QuickTerminalWindow.aTakenHotkeyPostsANoticeInTheFocusedWindow", "[quickterminal][window]")
{
    auto app = TestApp {};
    auto const window = ScopedController { app.manager() };
    REQUIRE(window.controller->notices().toastText().isEmpty());

    app.app().reportQuickTerminalHotkeyError(HotkeyError::AlreadyTaken);
    CHECK(window.controller->notices().toastText().toStdString()
          == contour::window::hotkeyUnavailableNoticeText(HotkeyError::AlreadyTaken));
}

TEST_CASE("QuickTerminalWindow.aChildContextsChromeStyleShadowsTheRootsDensity", "[quickterminal][window]")
{
    using contour::config::ChromeDensity;
    using contour::window::ChromeInputs;
    using contour::window::ChromeStyleHost;

    auto engine = QQmlEngine {};
    auto const configured =
        ChromeInputs { .row = contour::config::ChromeStyle::MacOS, .density = ChromeDensity::Comfortable };
    auto const rootHost = ChromeStyleHost { *engine.rootContext(), configured };
    auto* const child = new QQmlContext(engine.rootContext(), engine.rootContext());
    auto quickInputs = configured;
    quickInputs.density = contour::window::chromeDensityFor(WindowRole::QuickTerminal, configured.density);
    auto const childHost = ChromeStyleHost { *child, quickInputs };

    auto const densityIn = [&](QQmlContext* context) {
        auto expression = QQmlExpression { context, nullptr, QStringLiteral("chromeStyle.density") };
        return expression.evaluate().toString();
    };
    CHECK(densityIn(engine.rootContext()) == QStringLiteral("comfortable"));
    CHECK(densityIn(child) == QStringLiteral("minimal"));
}
