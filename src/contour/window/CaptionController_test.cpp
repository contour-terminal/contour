// SPDX-License-Identifier: Apache-2.0
//
// The controller's half of the caption buttons the OS drives (Snap Layouts): when it attaches the window
// to the caption adapter, and what the binding it lends answers and does. The adapter is a recording one,
// and the frame platform is injected, so the kept Windows frame's decisions run on every OS.

#include <contour/config/WindowControlStyle.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/platform/CaptionHitTest.hpp>
#include <contour/platform/NativeWindowFrame.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtGui/QExposeEvent>
#include <QtGui/QPlatformSurfaceEvent>
#include <QtGui/QRegion>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{

using contour::config::WindowControlKind;
using contour::platform::CaptionButtonVisual;
using contour::platform::FramePlatform;
using contour::platform::LogicalRect;

/// How long an offscreen window-state change may take to arrive, in milliseconds.
constexpr auto WindowStateTimeoutMs = 2000;

/// The maximise button's slot in a CaptionLayout.
constexpr auto MaximizeSlot = static_cast<size_t>(WindowControlKind::Maximize);

/// @param platform The frame platform the app follows.
/// @return The appearance services of @p record's fixture, following @p platform's frame policy.
[[nodiscard]] contour::platform::AppearanceServices servicesFor(
    contour::test::AccessibilitySwitch& accessibility,
    contour::test::AppearanceSwitch& appearance,
    contour::test::TitleBandRecord& titleBand,
    contour::test::CaptionRecord& record,
    FramePlatform platform)
{
    auto services = contour::test::appearanceServicesWith(accessibility, appearance, titleBand, record);
    services.framePlatform = platform;
    return services;
}

/// A test app over a recording caption adapter, following one frame platform's policy, and one window.
struct CaptionFixture
{
    contour::test::CaptionRecord record {};
    contour::test::TitleBandRecord titleBand {};
    contour::test::AccessibilitySwitch accessibility {};
    contour::test::AppearanceSwitch appearance {};
    contour::test::TestApp app;
    contour::test::ScopedControllerWithOsWindow window { app.manager() };

    /// @param platform The frame platform the app follows; Windows (the kept frame) by default.
    explicit CaptionFixture(FramePlatform platform = FramePlatform::Windows):
        app { nullptr,
              nullptr,
              nullptr,
              nullptr,
              contour::test::TestApp::testHostIdentity(),
              nullptr,
              {},
              nullptr,
              servicesFor(accessibility, appearance, titleBand, record, platform) }
    {
    }

    /// @return The window's controller.
    [[nodiscard]] contour::window::WindowController& controller() const { return *window.controller; }

    /// Hands the title area to the client band, as show_title_bar: false does, and binds the window.
    void bindClientWindow() const
    {
        controller().setTitleBarVisible(false);
        controller().bindWindow(window.osWindow.get());
    }
};

} // namespace

TEST_CASE("a Windows client frame attaches, others do not", "[contour][caption]")
{
    SECTION("the kept Windows frame attaches once")
    {
        auto fixture = CaptionFixture { FramePlatform::Windows };
        fixture.bindClientWindow();
        // A repeated expose re-decides and finds it attached already.
        auto expose = QExposeEvent { QRegion { 0, 0, 640, 400 } };
        QCoreApplication::sendEvent(fixture.window.osWindow.get(), &expose);

        REQUIRE(fixture.record.attached.size() == 1);
        CHECK(fixture.record.attached.front() == fixture.window.osWindow.get());
        CHECK(fixture.record.binding.has_value());
    }

    SECTION("the server frame does not attach")
    {
        auto fixture = CaptionFixture { FramePlatform::Windows };
        fixture.controller().setTitleBarVisible(true);
        fixture.controller().bindWindow(fixture.window.osWindow.get());
        CHECK(fixture.record.attached.empty());
    }

    SECTION("macOS and the Linux desktops do not attach")
    {
        for (auto const platform: std::array { FramePlatform::MacOS, FramePlatform::Other })
        {
            auto fixture = CaptionFixture { platform };
            fixture.bindClientWindow();
            CHECK(fixture.record.attached.empty());
        }
    }

    SECTION("the build's own platform attaches exactly where its client frame is the kept one")
    {
        auto fixture = CaptionFixture { contour::platform::currentFramePlatform() };
        fixture.bindClientWindow();
        auto const keepsFrame = contour::platform::framePolicyFor(contour::platform::currentFramePlatform(),
                                                                  contour::platform::WindowDecoration::Client)
                                    .shadow
                                == contour::platform::FrameShadowStrategy::NativeFrameKept;
        CHECK(fixture.record.attached.size() == (keepsFrame ? 1U : 0U));
    }
}

TEST_CASE("the controller forwards hover, press and toggle", "[contour][caption]")
{
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    auto& controller = fixture.controller();
    auto const binding = controller.captionBinding();
    auto spy = QSignalSpy { &controller, &contour::window::WindowController::nativeMaximizeVisualChanged };

    REQUIRE(controller.nativeMaximizeVisual() == CaptionButtonVisual::Rest);
    binding.visual(CaptionButtonVisual::Hover);
    CHECK(controller.nativeMaximizeVisual() == CaptionButtonVisual::Hover);
    CHECK(controller.nativeMaximizeHovered());
    CHECK_FALSE(controller.nativeMaximizePressed());
    CHECK(spy.count() == 1);

    binding.visual(CaptionButtonVisual::Hover);
    CHECK(spy.count() == 1);

    binding.visual(CaptionButtonVisual::Pressed);
    CHECK(controller.nativeMaximizeHovered());
    CHECK(controller.nativeMaximizePressed());
    CHECK(spy.count() == 2);

    auto* const osWindow = fixture.window.osWindow.get();
    binding.toggleMaximize();
    CHECK(QTest::qWaitFor([&] { return osWindow->windowStates().testFlag(Qt::WindowMaximized); },
                          WindowStateTimeoutMs));
    binding.toggleMaximize();
    CHECK(QTest::qWaitFor([&] { return !osWindow->windowStates().testFlag(Qt::WindowMaximized); },
                          WindowStateTimeoutMs));
}

TEST_CASE("the reported rectangles are what the layout answers", "[contour][caption]")
{
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    auto& controller = fixture.controller();
    auto const binding = controller.captionBinding();
    auto const maximize = static_cast<int>(WindowControlKind::Maximize);
    auto const expected = LogicalRect { .x = 914, .y = 0, .width = 46, .height = 40 };

    auto owner = QObject {};
    controller.setCaptionButtonRect(
        &owner, maximize, expected.x, expected.y, expected.width, expected.height);
    auto const reported = binding.layout();
    CHECK(reported.buttons[MaximizeSlot] == expected);
    CHECK(reported.windowWidth == fixture.window.osWindow->width());
    CHECK(reported.resizeBorder == contour::geometry::ResizeBorderThickness);

    // Out-of-range kinds change nothing.
    controller.setCaptionButtonRect(
        &owner, static_cast<int>(contour::config::WindowControlCount), 1, 2, 3, 4);
    controller.setCaptionButtonRect(&owner, -1, 1, 2, 3, 4);
    controller.clearCaptionButtonRect(&owner, static_cast<int>(contour::config::WindowControlCount));
    CHECK(binding.layout() == reported);

    controller.clearCaptionButtonRect(&owner, maximize);
    CHECK_FALSE(binding.layout().buttons[MaximizeSlot].has_value());
}

TEST_CASE("only the control that reported a caption rectangle last clears it", "[contour][caption]")
{
    // A change of side builds the new controls before the old ones are destroyed: the old one's clear on its
    // way out must leave the new one's rectangle standing.
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    auto& controller = fixture.controller();
    auto const binding = controller.captionBinding();
    auto const maximize = static_cast<int>(WindowControlKind::Maximize);
    auto const fresh = LogicalRect { .x = 0, .y = 0, .width = 46, .height = 40 };
    auto old = QObject {};
    auto replacement = QObject {};

    controller.setCaptionButtonRect(&old, maximize, 914, 0, 46, 40);
    controller.setCaptionButtonRect(&replacement, maximize, fresh.x, fresh.y, fresh.width, fresh.height);
    controller.clearCaptionButtonRect(&old, maximize);
    CHECK(binding.layout().buttons[MaximizeSlot] == fresh);

    controller.clearCaptionButtonRect(&replacement, maximize);
    CHECK_FALSE(binding.layout().buttons[MaximizeSlot].has_value());
}

TEST_CASE("a scale change keeps the reported rectangles logical", "[contour][caption]")
{
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    auto& controller = fixture.controller();
    auto owner = QObject {};
    controller.setCaptionButtonRect(&owner, static_cast<int>(WindowControlKind::Maximize), 914, 0, 46, 40);
    auto const before = controller.captionBinding().layout();

    auto event = QEvent { QEvent::DevicePixelRatioChange };
    QCoreApplication::sendEvent(fixture.window.osWindow.get(), &event);

    CHECK(controller.captionBinding().layout() == before);
}

TEST_CASE("show_title_bar detaches and re-attaches", "[contour][caption]")
{
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    REQUIRE(fixture.record.attached.size() == 1);
    fixture.controller().captionBinding().visual(CaptionButtonVisual::Hover);

    fixture.controller().setTitleBarVisible(true);
    REQUIRE(fixture.record.detached.size() == 1);
    CHECK(fixture.record.detached.front() == fixture.window.osWindow.get());
    CHECK_FALSE(fixture.record.binding.has_value());
    // Nothing drives the button any more.
    CHECK(fixture.controller().nativeMaximizeVisual() == CaptionButtonVisual::Rest);

    fixture.controller().setTitleBarVisible(false);
    CHECK(fixture.record.attached.size() == 2);
    CHECK(fixture.record.binding.has_value());
}

TEST_CASE("a destroyed surface detaches and the next expose re-attaches", "[contour][caption]")
{
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    REQUIRE(fixture.record.attached.size() == 1);

    auto destroyed = QPlatformSurfaceEvent { QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed };
    QCoreApplication::sendEvent(fixture.window.osWindow.get(), &destroyed);
    CHECK(fixture.record.detached.size() == 1);

    auto expose = QExposeEvent { QRegion { 0, 0, 640, 400 } };
    QCoreApplication::sendEvent(fixture.window.osWindow.get(), &expose);
    CHECK(fixture.record.attached.size() == 2);
}

TEST_CASE("a closing controller detaches its window", "[contour][caption]")
{
    auto fixture = CaptionFixture {};
    fixture.bindClientWindow();
    REQUIRE(fixture.record.attached.size() == 1);

    fixture.app.manager().removeWindowController(fixture.window.id);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    REQUIRE(fixture.record.detached.size() == 1);
    CHECK(fixture.record.detached.front() == fixture.window.osWindow.get());
}
