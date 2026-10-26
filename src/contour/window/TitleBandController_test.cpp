// SPDX-License-Identifier: Apache-2.0
//
// The controller's half of the title band: what it asks the material for, how often, and the geometry it
// publishes, live across Reduce Transparency, the chrome scheme, a scale change, full screen and
// show_title_bar. The material is a recording one, measuring a 28-pt title bar that rests 24 pt down in
// full screen. On a host whose frame is not the full-size content view (Linux, Windows) nothing is ever
// applied -- which is how these cases prove those hosts unaffected.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/Config.hpp>
#include <contour/platform/NativeWindowFrame.hpp>
#include <contour/platform/TitleBandMaterial.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/TitleBandGeometry.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QMargins>
#include <QtGui/QExposeEvent>
#include <QtGui/QRegion>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{

using contour::config::BandBackdrop;
using contour::config::ChromeDensity;
using contour::config::TabBarVisibility;
using contour::config::TitleBarGrowth;
using contour::platform::TitleBandRequest;
using contour::window::BarCentre;

/// How long an offscreen window-state change may take to arrive, in milliseconds.
constexpr auto WindowStateTimeoutMs = 2000;

/// @return Whether this host dresses a client-band window with the title-band material (macOS).
[[nodiscard]] bool hostDressesTitleBand()
{
    return contour::platform::framePolicyFor(contour::platform::currentFramePlatform(),
                                             contour::platform::WindowDecoration::Client)
               .shadow
           == contour::platform::FrameShadowStrategy::FullSizeContent;
}

/// How a TitleBandFixture leaves its window.
enum class FixtureWindow : uint8_t
{
    CreatedAndBound = 0, //!< A platform window exists and the controller is bound to it.
    Unbound,             //!< Neither: the case creates and binds it in the order it tests.
};

/// A test app over switched appearance sources and a recording material, and one window.
struct TitleBandFixture
{
    contour::test::TitleBandRecord record { .metrics = { .height = 28, .fullScreenTop = 24 } };
    contour::test::AccessibilitySwitch accessibility {};
    contour::test::AppearanceSwitch appearance {};
    contour::test::TestApp app { nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 contour::test::TestApp::testHostIdentity(),
                                 nullptr,
                                 {},
                                 nullptr,
                                 contour::test::appearanceServicesWith(accessibility, appearance, record) };
    contour::test::ScopedControllerWithOsWindow window { app.manager() };

    /// @param state How the window is left; created and bound by default.
    explicit TitleBandFixture(FixtureWindow state = FixtureWindow::CreatedAndBound)
    {
        if (state == FixtureWindow::Unbound)
            return;
        // A platform window, as an exposed one has: the controller applies nothing to a window without one.
        window.osWindow->create();
        window->bindWindow(window.osWindow.get());
    }

    /// Sends the window an expose, as the platform does once it is mapped.
    void expose() const
    {
        auto event = QExposeEvent { QRegion { 0, 0, 640, 400 } };
        QCoreApplication::sendEvent(window.osWindow.get(), &event);
    }

    /// @return The window's controller.
    [[nodiscard]] contour::window::WindowController& controller() const { return *window.controller; }

    /// Hands the title area to the client band, as show_title_bar: false does.
    void useClientBand() const { controller().setTitleBarVisible(false); }

    /// Puts the window in @p states and waits for the controller to hear of it.
    void setWindowStates(Qt::WindowStates states) const
    {
        window.osWindow->setWindowStates(states);
        REQUIRE(
            QTest::qWaitFor([&] { return window.osWindow->windowStates() == states; }, WindowStateTimeoutMs));
        QCoreApplication::processEvents();
    }
};

} // namespace

TEST_CASE("an expose applies once, and a resize re-applies nothing", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    fixture.controller().refreshTitleBand();
    auto const expected = hostDressesTitleBand() ? 1U : 0U;
    REQUIRE(fixture.record.applied.size() == expected);

    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
    {
        auto expose = QExposeEvent { QRegion { 0, 0, 640, 400 } };
        QCoreApplication::sendEvent(fixture.window.osWindow.get(), &expose);
    }
    for (auto const step: std::views::iota(0, 20))
        fixture.window.osWindow->resize(640 + step, 400 + step);
    QCoreApplication::processEvents();

    CHECK(fixture.record.applied.size() == expected);
}

TEST_CASE("Reduce Transparency leaves the solid band as it is", "[contour][titleband][controller]")
{
    // Ruling S6: the macOS row is already the client's own paint, which is what Reduce Transparency asks
    // for, so the preference changes no request and applies nothing again.
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    auto const height = fixture.controller().titleBandHeight();
    auto const applied = fixture.record.applied.size();
    REQUIRE(applied == (hostDressesTitleBand() ? 1U : 0U));
    CHECK(fixture.controller().clearsTitleBarBackground() == hostDressesTitleBand());
    if (hostDressesTitleBand())
        CHECK(fixture.record.applied.back().backdrop == BandBackdrop::Client);

    fixture.accessibility.set({ .transparency = contour::config::TransparencyPreference::Reduced });
    CHECK(fixture.record.applied.size() == applied);
    CHECK(fixture.controller().titleBandRequest().backdrop == BandBackdrop::Client);
    CHECK(fixture.controller().clearsTitleBarBackground() == hostDressesTitleBand());
    CHECK(fixture.controller().titleBandHeight() == height);

    fixture.accessibility.set({});
    CHECK(fixture.record.applied.size() == applied);
    CHECK(fixture.controller().titleBandRequest().backdrop == BandBackdrop::Client);
    CHECK(fixture.controller().clearsTitleBarBackground() == hostDressesTitleBand());
    CHECK(fixture.controller().titleBandHeight() == height);
}

TEST_CASE("the request follows the applied chrome scheme", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    auto& app = fixture.app.app();

    app.applyGuiTheme(contour::config::GuiTheme::Light, app.defaultProfileColors());
    QCoreApplication::processEvents();
    CHECK(fixture.controller().titleBandRequest().scheme == Qt::ColorScheme::Light);
    if (hostDressesTitleBand())
        CHECK(fixture.record.applied.back().scheme == Qt::ColorScheme::Light);

    app.applyGuiTheme(contour::config::GuiTheme::Dark, app.defaultProfileColors());
    QCoreApplication::processEvents();
    CHECK(fixture.controller().titleBandRequest().scheme == Qt::ColorScheme::Dark);
    if (hostDressesTitleBand())
        CHECK(fixture.record.applied.back().scheme == Qt::ColorScheme::Dark);

    // Back to the OS palette, so no later case inherits a forced one.
    app.applyGuiTheme(contour::config::GuiTheme::System, app.defaultProfileColors());
    QCoreApplication::processEvents();
}

TEST_CASE("a scale change measures the title bar again", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    fixture.record.metrics.height = 38.25;
    auto const measured = fixture.record.measured;

    auto change = QEvent { QEvent::DevicePixelRatioChange };
    QCoreApplication::sendEvent(fixture.window.osWindow.get(), &change);

    auto const devicePixels =
        fixture.controller().titleBandHeight() * fixture.window.osWindow->devicePixelRatio();
    CHECK(devicePixels == std::floor(devicePixels));
    if (hostDressesTitleBand())
    {
        CHECK(fixture.record.measured > measured);
        CHECK(fixture.controller().titleBandHeight() >= 38.25);
    }
    else
        CHECK(fixture.record.measured == measured);
}

TEST_CASE("full screen keeps the solid band and moves only its resting place",
          "[contour][titleband][controller]")
{
    // Ruling S6: windowed and full screen ask for the same client backdrop, so entering full screen applies
    // nothing and Main.qml's `flags` do not toggle during AppKit's transition; only the reveal top moves.
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    auto const windowed = fixture.controller().titleBandRequest();
    auto const windowedInset = fixture.controller().nativeControlsInset();
    auto const applied = fixture.record.applied.size();
    auto changes = QSignalSpy { &fixture.controller(), &contour::window::WindowController::titleBandChanged };

    fixture.setWindowStates(Qt::WindowFullScreen);
    CHECK(fixture.controller().titleBandRequest() == windowed);
    CHECK(fixture.record.applied.size() == applied);
    CHECK(fixture.controller().clearsTitleBarBackground() == hostDressesTitleBand());
    // Below AppKit's revealed title bar: its top plus its height.
    CHECK(fixture.controller().titleBandRevealTop() == (hostDressesTitleBand() ? 24 + 28 : 0));
    // The traffic lights move into AppKit's revealed strip: their inset is still published, and Main.qml,
    // which rests the band below that strip, applies none in full screen.
    CHECK(fixture.controller().nativeControlsInset() == windowedInset);
    CHECK(changes.count() == (hostDressesTitleBand() ? 1 : 0));

    fixture.setWindowStates(Qt::WindowNoState);
    CHECK(fixture.controller().titleBandRequest() == windowed);
    CHECK(fixture.record.applied.size() == applied);
    CHECK(fixture.controller().clearsTitleBarBackground() == hostDressesTitleBand());
    CHECK(fixture.controller().titleBandRevealTop() == 0);
    CHECK(changes.count() == (hostDressesTitleBand() ? 2 : 0));
}

TEST_CASE("show_title_bar applies no material, and turning it off applies again",
          "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    auto& controller = fixture.controller();
    controller.setTitleBarVisible(true);
    controller.refreshTitleBand();
    CHECK(fixture.record.applied.empty());
    CHECK_FALSE(controller.clearsTitleBarBackground());

    auto insets = QSignalSpy { &controller, &contour::window::WindowController::nativeControlsInsetChanged };
    controller.setTitleBarVisible(false);
    if (!hostDressesTitleBand())
    {
        CHECK(fixture.record.applied.empty());
        CHECK_FALSE(controller.clearsTitleBarBackground());
        return;
    }
    REQUIRE(fixture.record.applied.size() == 1);
    // The toolbar moves the traffic lights, so the band's leading slot reads its inset again (F9).
    CHECK(insets.count() >= 1);

    // Back to the native title bar: the default request takes the toolbar and the appearance back once,
    // and nothing is applied under the native frame after that.
    controller.setTitleBarVisible(true);
    REQUIRE(fixture.record.applied.size() == 2);
    CHECK(fixture.record.applied.back() == TitleBandRequest {});
    controller.refreshTitleBand();
    CHECK(fixture.record.applied.size() == 2);
    CHECK_FALSE(controller.clearsTitleBarBackground());
}

TEST_CASE("the band is at least the token tall", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    fixture.controller().refreshTitleBand();
    auto const tokens = fixture.app.app().titleBandTokens(fixture.controller().titleBand().centre);

    if (hostDressesTitleBand())
    {
        CHECK(tokens.style == contour::config::ChromeStyle::MacOS);
        CHECK(fixture.controller().titleBandHeight() == std::max(tokens.height, 28.0));
    }
    else
        CHECK(fixture.controller().titleBandHeight() == tokens.height);
}

TEST_CASE("the first expose applies a request the bind could not", "[contour][titleband][controller]")
{
    // The seed may land before the platform window exists; then Expose is the one path to the first apply.
    auto fixture = TitleBandFixture { FixtureWindow::Unbound };
    fixture.window->bindWindow(fixture.window.osWindow.get());
    fixture.useClientBand();
    CHECK(fixture.record.applied.empty());

    fixture.window.osWindow->create();
    fixture.expose();

    CHECK(fixture.record.applied.size() == (hostDressesTitleBand() ? 1U : 0U));
}

TEST_CASE("a window publishes its band request at bind, before it is shown",
          "[contour][titleband][controller]")
{
    // Seeded before the bind, as a session's show_title_bar can be: the bind publishes the request, so
    // Main.qml's `flags` carry the background hint before the first frame -- without applying anything to a
    // window that has no platform window yet.
    auto fixture = TitleBandFixture { FixtureWindow::Unbound };
    fixture.useClientBand();
    auto changes = QSignalSpy { &fixture.controller(), &contour::window::WindowController::titleBandChanged };

    fixture.window->bindWindow(fixture.window.osWindow.get());

    CHECK(changes.count() == 1);
    CHECK(fixture.controller().clearsTitleBarBackground() == hostDressesTitleBand());
    CHECK(fixture.record.applied.empty());
}

TEST_CASE("a request the OS answers during its apply is applied once", "[contour][titleband][controller]")
{
    // Attaching the toolbar changes Qt's safe area, and Qt may say so before apply() returns: the nested
    // refresh must see the request as applied already.
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
    SKIP("QWindow::safeAreaMarginsChanged is Qt 6.9's");
#else
    auto fixture = TitleBandFixture {};
    fixture.record.duringApply = [&] {
        emit fixture.window.osWindow->safeAreaMarginsChanged(QMargins { 0, 40, 0, 0 });
    };
    auto changes = QSignalSpy { &fixture.controller(), &contour::window::WindowController::titleBandChanged };
    auto insets =
        QSignalSpy { &fixture.controller(), &contour::window::WindowController::nativeControlsInsetChanged };

    fixture.useClientBand();

    if (!hostDressesTitleBand())
    {
        CHECK(fixture.record.applied.empty());
        return;
    }
    CHECK(fixture.record.applied.size() == 1);
    CHECK(changes.count() == 1);
    CHECK(insets.count() == 1);
#endif
}

TEST_CASE("a recreated platform window is dressed again", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();
    auto const applied = fixture.record.applied.size();

    fixture.window.osWindow->destroy();
    fixture.window.osWindow->create();
    fixture.expose();

    CHECK(fixture.record.applied.size() == applied + (hostDressesTitleBand() ? 1U : 0U));
}

TEST_CASE("another screen and a new safe area measure the band again", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.useClientBand();

    auto measured = fixture.record.measured;
    // QWindow::safeAreaMarginsChanged is Qt 6.9's (the controller listens only there); the screen half holds
    // on every Qt.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    fixture.record.metrics.height = 40;
    emit fixture.window.osWindow->safeAreaMarginsChanged(QMargins { 0, 40, 0, 0 });
    if (hostDressesTitleBand())
    {
        CHECK(fixture.record.measured > measured);
        CHECK(fixture.controller().titleBandHeight() == 40);
    }
#endif

    measured = fixture.record.measured;
    fixture.record.metrics.height = 44;
    emit fixture.window.osWindow->screenChanged(fixture.window.osWindow->screen());
    if (hostDressesTitleBand())
    {
        CHECK(fixture.record.measured > measured);
        CHECK(fixture.controller().titleBandHeight() == 44);
    }
    else
        CHECK(fixture.record.measured == measured);
}

TEST_CASE("Minimal's bare band detaches the toolbar and is as tall as the bare title bar",
          "[contour][titleband][controller]")
{
    // Ruling P3: on macOS the bare band (one tab under Minimal) is AppKit's bare title bar, 32 pt, without
    // the compact toolbar; the toolbar comes back with the tabs, and each change moves the traffic lights.
    auto fixture = TitleBandFixture {};
    fixture.app.app().config().chromeDensity = ChromeDensity::Minimal;
    fixture.record.metrics.height = 32;
    auto& controller = fixture.controller();
    controller.setTabBarVisibility(TabBarVisibility::Multiple);
    REQUIRE_FALSE(controller.tabBarShouldShow());
    fixture.useClientBand();

    if (!hostDressesTitleBand())
    {
        // The Flat rows' band is the strip: Minimal never empties it, and nothing is applied.
        CHECK(controller.titleBand().centre == BarCentre::Tabs);
        CHECK(fixture.record.applied.empty());
        return;
    }

    auto const bare = fixture.app.app().titleBandTokens(BarCentre::Empty);
    CHECK(bare.height == 32);
    CHECK(controller.titleBand().centre == BarCentre::Empty);
    REQUIRE_FALSE(fixture.record.applied.empty());
    CHECK(fixture.record.applied.back().growth == TitleBarGrowth::None);
    CHECK(fixture.record.applied.back().backdrop == BandBackdrop::Client);
    CHECK(controller.titleBandHeight() == 32);

    // The tabs show: the toolbar is attached again, the traffic lights move, and the band is measured anew.
    auto insets = QSignalSpy { &controller, &contour::window::WindowController::nativeControlsInsetChanged };
    fixture.record.metrics.height = 40;
    controller.setTabBarVisibility(TabBarVisibility::Always);
    CHECK(controller.titleBand().centre == BarCentre::Tabs);
    CHECK(fixture.record.applied.back().growth == TitleBarGrowth::CompactToolbar);
    CHECK(insets.count() == 1);
    CHECK(controller.titleBandHeight() == 40);

    // And one tab again: bare once more.
    fixture.record.metrics.height = 32;
    controller.setTabBarVisibility(TabBarVisibility::Multiple);
    CHECK(fixture.record.applied.back().growth == TitleBarGrowth::None);
    CHECK(insets.count() == 2);
    CHECK(controller.titleBandHeight() == 32);
}

TEST_CASE("Comfortable keeps the toolbar with one tab", "[contour][titleband][controller]")
{
    auto fixture = TitleBandFixture {};
    fixture.controller().setTabBarVisibility(TabBarVisibility::Multiple);
    fixture.useClientBand();
    if (!hostDressesTitleBand())
        return;
    CHECK(fixture.controller().titleBand().centre == BarCentre::Title);
    REQUIRE_FALSE(fixture.record.applied.empty());
    CHECK(fixture.record.applied.back().growth == TitleBarGrowth::CompactToolbar);
}
