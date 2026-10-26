// SPDX-License-Identifier: Apache-2.0
//
// AppKit's title-band material on a real NSWindow: a shown 900 x 600 window whose content fills the title
// bar under Qt's ExpandedClientAreaHint, with the client frame applied, as WindowController leaves it. Qt's
// NoTitleBarBackgroundHint is set here where the controller would set it (the client backdrop), and the
// material does the rest. Needs the cocoa platform plugin, so every case runs in a display run
// (CONTOUR_TEST_DISPLAY=1) alone.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/platform/CocoaWindowFrame.hpp>
#include <contour/platform/TitleBandMaterial.hpp>
#include <contour/test/TestEnvironment.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/TitleBandSurface.hpp>

#include <core/Environment.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QWindow>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>

#import <AppKit/AppKit.h>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using contour::config::BandBackdrop;
using contour::config::TitleBarGrowth;
using contour::platform::NativeTitleBarMetrics;
using contour::platform::TitleBandRequest;
using contour::platform::WindowDecoration;

namespace
{

#define REQUIRE_COCOA_OR_SKIP()                                                 \
    do                                                                          \
    {                                                                           \
        if (QGuiApplication::platformName() != QStringLiteral("cocoa")          \
            || !contour::test::displayTestsEnabled(core::LiveEnvironment {}))   \
            SKIP("Needs CONTOUR_TEST_DISPLAY=1 and the cocoa platform plugin"); \
    } while (false)

/// How long AppKit gets to finish a full-screen transition, in milliseconds.
constexpr auto FullScreenTimeoutMs = 5000;

/// A band at the window's top under the hint: a tint with three tab labels on it.
/// `clearsBackground` adds Qt's NoTitleBarBackgroundHint, as the controller does for the client backdrop.
constexpr auto BandWindow = R"(
import QtQuick

Window {
    id: root
    property bool clearsBackground: false
    width: 900
    height: 600
    color: "transparent"
    flags: Qt.Window | Qt.ExpandedClientAreaHint | (clearsBackground ? Qt.NoTitleBarBackgroundHint : 0)
    Rectangle {
        id: band
        objectName: "band"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 40
        color: "#2b2d3a"
        Row {
            x: 96
            anchors.verticalCenter: parent.verticalCenter
            spacing: 24
            Repeater {
                model: [ "zsh - ~/projects/contour", "vim Main.qml", "htop" ]
                Text { text: modelData; color: "#f0f0f0"; font.pixelSize: 13 }
            }
        }
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: band.bottom
        anchors.bottom: parent.bottom
        color: "#1e1e1e"
    }
}
)";

/// The window built from BandWindow, shown, exposed and given the client frame.
struct ShownBand
{
    QQmlEngine engine {};
    std::unique_ptr<QQuickWindow> window {};
    std::unique_ptr<contour::platform::NativeWindowFrame> frame = contour::platform::makeCocoaWindowFrame();
    std::unique_ptr<contour::platform::ITitleBandMaterial> material =
        contour::platform::makeTitleBandMaterial();

    ShownBand()
    {
        auto component = QQmlComponent { &engine };
        component.setData(QByteArray { BandWindow }, QUrl {});
        INFO(component.errorString().toStdString());
        window.reset(qobject_cast<QQuickWindow*>(component.create()));
        REQUIRE(window != nullptr);
        window->show();
        REQUIRE(QTest::qWaitForWindowExposed(window.get()));
        REQUIRE(frame != nullptr);
        frame->apply(*window, WindowDecoration::Client);
    }

    /// @return The NSWindow behind the shown window.
    [[nodiscard]] NSWindow* nsWindow() const
    {
        // Under the cocoa plugin a WId is the NSView*; ARC wants the bridge from an untyped pointer.
        auto* const handle = reinterpret_cast<void*>(window->winId());
        auto* const view = (__bridge NSView*) handle;
        return view.window;
    }

    /// Applies @p request, with Qt's hint set as the controller sets it for the request's backdrop.
    void apply(TitleBandRequest const& request) const
    {
        window->setProperty("clearsBackground", request.backdrop == BandBackdrop::Client);
        QCoreApplication::processEvents();
        material->apply(*window, request);
    }

    /// @return The title bar's height as AppKit lays it out: the frame above the content layout rectangle.
    [[nodiscard]] qreal appKitTitleBarHeight() const
    {
        NSWindow* const ns = nsWindow();
        return static_cast<qreal>(NSHeight(ns.frame) - NSHeight(ns.contentLayoutRect));
    }

    /// Waits until Qt's safe-area top has caught up with AppKit's title bar, which it learns asynchronously.
    void waitForSafeArea() const
    {
        std::ignore = QTest::qWaitFor(
            [this] { return std::abs(window->safeAreaMargins().top() - appKitTitleBarHeight()) <= 0.5; });
    }
};

/// @return What the shipped macOS row asks for in @p mode: its resolved title-band material through the
///         surface table (ruling S6: the Solid row, the client's paint over the compact toolbar).
[[nodiscard]] TitleBandRequest selectedRequest(contour::window::WindowMode mode)
{
    using contour::config::ChromeStyle;
    auto const surface = contour::window::resolveTitleBandSurface({
        .style = ChromeStyle::MacOS,
        .material = contour::config::resolveChromeTokens(ChromeStyle::MacOS, {}).material.titleBand,
        .frame = contour::window::WindowFrame::ClientBand,
        .mode = mode,
    });
    return TitleBandRequest { .backdrop = surface.backdrop, .growth = surface.growth };
}

[[nodiscard]] bool isFullScreen(NSWindow* window)
{
    return (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

[[nodiscard]] NSAppearanceName effectiveAppearanceName(NSWindow* window)
{
    return [window.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
}

/// Takes @p window into full screen (@p wanted) or out of it, and waits for AppKit to finish. A toggle that
/// arrives while the previous window's Space animation is still running can be dropped, so one that has
/// not taken effect within the timeout is sent once more.
/// @return Whether the window reached the wanted state.
[[nodiscard]] bool toggleFullScreenAndWait(NSWindow* window, bool wanted)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 2))
    {
        [window toggleFullScreen:nil];
        if (QTest::qWaitFor([&] { return isFullScreen(window) == wanted; }, FullScreenTimeoutMs))
        {
            // The transition's last frames settle after the style mask flips.
            QTest::qWait(1000);
            return true;
        }
    }
    return false;
}

/// The revealed full-screen title bar's own window, if AppKit has one on screen.
[[nodiscard]] NSWindow* fullScreenTitleBarWindow()
{
    auto const index = [NSApp.windows indexOfObjectPassingTest:^BOOL(NSWindow* candidate, NSUInteger, BOOL*) {
      return [NSStringFromClass([candidate class]) isEqualToString:@"NSToolbarFullScreenWindow"]
             && candidate.isVisible;
    }];
    return index != NSNotFound ? NSApp.windows[index] : nil;
}

/// @return Whether AppKit would deliver a click at @p point (window-logical, from the top-left) to Qt's
///         content view rather than to a title-bar view of its own: the view the theme frame hit-tests there.
[[nodiscard]] bool contentReceivesClickAt(NSWindow* window, QPointF point)
{
    NSView* const themeFrame = window.contentView.superview;
    auto const location =
        [themeFrame convertPoint:NSMakePoint(point.x(), NSHeight(window.frame) - point.y()) fromView:nil];
    NSView* const hit = [themeFrame hitTest:location];
    UNSCOPED_INFO("AppKit hit-tests (" << point.x() << ", " << point.y() << ") to "
                                       << NSStringFromClass([hit class]).UTF8String);
    return hit != nil && [hit isDescendantOf:window.contentView];
}

} // namespace

TEST_CASE("the native backdrop shows AppKit's material and keeps the shadow", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};
    NSWindow* const nsWindow = band.nsWindow();
    REQUIRE(nsWindow != nil);

    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::CompactToolbar });
    CHECK(nsWindow.titlebarAppearsTransparent == NO);
    CHECK(nsWindow.hasShadow == YES);

    band.apply({ .backdrop = BandBackdrop::Client, .growth = TitleBarGrowth::CompactToolbar });
    CHECK(nsWindow.titlebarAppearsTransparent == YES);
    CHECK(nsWindow.hasShadow == YES);
}

TEST_CASE("a light request gives a light window under a dark app", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    NSAppearance* const appAppearance = NSApp.appearance;
    NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    auto band = ShownBand {};
    NSWindow* const nsWindow = band.nsWindow();
    REQUIRE(nsWindow != nil);

    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .scheme = Qt::ColorScheme::Light });
    CHECK([effectiveAppearanceName(nsWindow) isEqualToString:NSAppearanceNameAqua]);

    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .scheme = Qt::ColorScheme::Unknown });
    CHECK([effectiveAppearanceName(nsWindow) isEqualToString:NSAppearanceNameDarkAqua]);

    NSApp.appearance = appAppearance;
}

TEST_CASE("metrics measure the title bar", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};

    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::None });
    band.waitForSafeArea();
    auto const bare = band.material->metrics(*band.window);
    CHECK(bare.height > 0);
    CHECK(bare.height == Catch::Approx(band.appKitTitleBarHeight()).margin(0.5));
    CHECK(bare.fullScreenTop == 0);

    // The spike's toolbar: the title bar grows to 40 pt around centred traffic lights (ruling S2).
    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::CompactToolbar });
    band.waitForSafeArea();
    auto const grown = band.material->metrics(*band.window);
    CHECK(grown.height >= 37.5);
    CHECK(grown.height == Catch::Approx(band.appKitTitleBarHeight()).margin(0.5));
}

TEST_CASE("the toolbar widens the traffic lights' inset, and detaching it restores it",
          "[display][cocoa][titleband]")
{
    // F9: the band's leading slot reads the frame's measured inset, which the toolbar moves (78 -> 84 pt).
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};
    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::None });
    band.waitForSafeArea();
    auto const bare = band.frame->nativeControlsInset(*band.window);
    REQUIRE(bare > 0);

    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::CompactToolbar });
    band.waitForSafeArea();
    CHECK(band.frame->nativeControlsInset(*band.window) > bare);

    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::None });
    band.waitForSafeArea();
    CHECK(band.frame->nativeControlsInset(*band.window) == Catch::Approx(bare));
    CHECK(band.nsWindow().toolbar == nil);
}

TEST_CASE("the shipped band takes its own clicks, windowed and in full screen", "[display][cocoa][titleband]")
{
    // F13 on the selected row: with the title bar's background cleared, AppKit hit-tests the band to Qt's
    // view, so a tab label (centred near y 20) gets its click.
    REQUIRE_COCOA_OR_SKIP();
    using contour::window::WindowMode;
    auto band = ShownBand {};
    NSWindow* const nsWindow = band.nsWindow();
    REQUIRE(nsWindow != nil);
    REQUIRE(selectedRequest(WindowMode::Windowed).backdrop == BandBackdrop::Client);
    band.apply(selectedRequest(WindowMode::Windowed));
    band.waitForSafeArea();
    CHECK(contentReceivesClickAt(nsWindow, QPointF { 300, 20 }));

    REQUIRE(toggleFullScreenAndWait(nsWindow, true));
    band.apply(selectedRequest(WindowMode::FullScreen));
    CHECK(contentReceivesClickAt(nsWindow, QPointF { 300, 20 }));

    REQUIRE(toggleFullScreenAndWait(nsWindow, false));
}

// Hidden ([.]): the record of why the native-material row is unselected (ruling S6) -- under
// titlebarAppearsTransparent = NO the NSTitlebarView claims the band's top 32 pt. Run it by name.
TEST_CASE("under AppKit's material, the title bar takes the band's clicks", "[.][display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};
    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::CompactToolbar });
    band.waitForSafeArea();

    CHECK_FALSE(contentReceivesClickAt(band.nsWindow(), QPointF { 300, 20 }));
}

TEST_CASE("forty resizes keep the shadow and the title-bar setting", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};
    NSWindow* const nsWindow = band.nsWindow();
    REQUIRE(nsWindow != nil);
    band.apply(selectedRequest(contour::window::WindowMode::Windowed));

    for (auto const step: std::views::iota(0, 40))
    {
        auto frame = nsWindow.frame;
        frame.size.width += step % 2 == 0 ? 7 : -7;
        frame.size.height += step % 2 == 0 ? 7 : -7;
        [nsWindow setFrame:frame display:YES];
        QCoreApplication::processEvents();
    }

    CHECK(nsWindow.hasShadow == YES);
    CHECK(nsWindow.titlebarAppearsTransparent == YES);
}

TEST_CASE("full screen and back", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};
    NSWindow* const nsWindow = band.nsWindow();
    REQUIRE(nsWindow != nil);
    band.apply(selectedRequest(contour::window::WindowMode::Windowed));

    REQUIRE(toggleFullScreenAndWait(nsWindow, true));
    band.apply(selectedRequest(contour::window::WindowMode::FullScreen));
    auto const metrics = band.material->metrics(*band.window);
    // Window-relative (F3): 0 where the full-screen window already starts below the notch.
    CHECK(metrics.fullScreenTop >= 0);
    if (NSWindow* const revealed = fullScreenTitleBarWindow())
        CHECK(metrics.fullScreenTop
              == Catch::Approx(std::max(0.0, NSMaxY(nsWindow.frame) - NSMaxY(revealed.frame))).margin(0.5));

    REQUIRE(toggleFullScreenAndWait(nsWindow, false));
    band.apply(selectedRequest(contour::window::WindowMode::Windowed));
    CHECK(nsWindow.hasShadow == YES);
    CHECK(nsWindow.titlebarAppearsTransparent == YES);
    CHECK(band.material->metrics(*band.window).fullScreenTop == 0);
}

TEST_CASE("a second screen measures again", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto band = ShownBand {};
    NSWindow* const nsWindow = band.nsWindow();
    REQUIRE(nsWindow != nil);
    auto const scale = nsWindow.screen.backingScaleFactor;
    auto const index = [NSScreen.screens indexOfObjectPassingTest:^BOOL(NSScreen* screen, NSUInteger, BOOL*) {
      return screen.backingScaleFactor != scale;
    }];
    if (index == NSNotFound)
        SKIP("Needs a second screen of another backing scale factor");
    NSScreen* const other = NSScreen.screens[index];

    auto screenChanged = QSignalSpy { band.window.get(), &QWindow::screenChanged };
    [nsWindow setFrameOrigin:NSMakePoint(NSMinX(other.visibleFrame) + 40, NSMinY(other.visibleFrame) + 40)];
    REQUIRE(screenChanged.wait(FullScreenTimeoutMs));
    band.apply({ .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::CompactToolbar });
    band.waitForSafeArea();

    CHECK(band.material->metrics(*band.window).height
          == Catch::Approx(band.appKitTitleBarHeight()).margin(0.5));
    CHECK(nsWindow.hasShadow == YES);
}

TEST_CASE("no window, no AppKit", "[display][cocoa][titleband]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto window = QWindow {};
    auto const material = contour::platform::makeTitleBandMaterial();

    CHECK(material->metrics(window) == NativeTitleBarMetrics {});
    material->apply(window,
                    { .backdrop = BandBackdrop::NativeMaterial, .growth = TitleBarGrowth::CompactToolbar });
    CHECK(window.handle() == nullptr);
}

TEST_CASE("the shipped band, for the owner's eye", "[display][cocoa][titleband]")
{
    // Owner check 1 under ruling S6: what Qt draws and what AppKit lays over it on the selected row (the
    // client's paint over the compact toolbar), written where CONTOUR_SNAPSHOT_DIR says. AppKit's own render
    // draws no backdrop (spike-decisions.md, limits); it does show its title-bar views and any separator.
    REQUIRE_COCOA_OR_SKIP();
    auto const directory = core::LiveEnvironment {}.get("CONTOUR_SNAPSHOT_DIR");
    if (!directory || directory->empty())
        SKIP("Writes its pictures only where CONTOUR_SNAPSHOT_DIR says");
    auto band = ShownBand {};
    band.apply(selectedRequest(contour::window::WindowMode::Windowed));
    band.waitForSafeArea();
    QTest::qWait(500);

    auto const dir = QDir { QString::fromStdString(*directory) };
    CHECK(band.window->grabWindow().save(dir.filePath(QStringLiteral("titleband-solid.qt.png"))));

    NSView* const themeFrame = band.nsWindow().contentView.superview;
    NSBitmapImageRep* const bitmap = [themeFrame bitmapImageRepForCachingDisplayInRect:themeFrame.bounds];
    REQUIRE(bitmap != nil);
    [themeFrame cacheDisplayInRect:themeFrame.bounds toBitmapImageRep:bitmap];
    NSData* const png =
        [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:[NSDictionary dictionary]];
    auto const path = dir.filePath(QStringLiteral("titleband-solid.appkit.png")).toNSString();
    CHECK([png writeToFile:path atomically:YES]);
}
