// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowResizeGrid.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

TEST_CASE("CocoaWindowResizeGrid.snapsAtEndOfLiveResize", "[row-resize][native]")
{
    using namespace contour;
    if (QGuiApplication::platformName() != "cocoa")
        SKIP("Requires the native Cocoa platform.");

    QWindow window;
    window.resize(800, 600);
    window.create();
    auto* view = reinterpret_cast<NSView*>(window.winId());
    auto* nativeWindow = view.window;
    REQUIRE(nativeWindow != nil);
    auto grid = std::optional { geometry::rowResizeGridFor(
        23, {}, window.devicePixelRatio(), geometry::Chrome { .height = 33 }, 100) };
    auto subscription = platform::makeWindowResizeGrid(window, [&] { return grid; });
    REQUIRE(subscription != nullptr);

    auto const propose = [&] {
        auto frame = nativeWindow.frame;
        frame.size.height += 593 - view.bounds.size.height;
        [nativeWindow setFrame:frame display:NO];
        return nativeWindow.frame;
    };
    auto const notify = [&] {
        [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidEndLiveResizeNotification
                                                          object:nativeWindow];
    };
    auto const before = propose();
    auto const expected = geometry::snapLogicalHeightToRows(593, window.devicePixelRatio(), *grid);
    REQUIRE(expected != 593);
    notify();
    CHECK(view.bounds.size.height == expected);
    CHECK(NSMaxY(nativeWindow.frame) == NSMaxY(before));
    CHECK(nativeWindow.frame.size.width == before.size.width);

    grid.reset();
    auto const disabled = propose();
    notify();
    CHECK(NSEqualRects(nativeWindow.frame, disabled));

    grid =
        geometry::rowResizeGridFor(23, {}, window.devicePixelRatio(), geometry::Chrome { .height = 33 }, 100);
    subscription.reset();
    notify();
    CHECK(NSEqualRects(nativeWindow.frame, disabled));
}
