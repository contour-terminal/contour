// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TaskbarProgressSink.hpp>

#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

TEST_CASE("Dock progress restores existing content and leaves badges alone",
          "[contour][taskbar-native][display]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        SKIP("Requires CONTOUR_TEST_DISPLAY=1 and the Cocoa Qt platform.");
    auto* tile = NSApp.dockTile;
    NSView* const original = tile.contentView;
    NSString* const badge = tile.badgeLabel;
    auto sink = contour::platform::makeTaskbarProgressSink();
    REQUIRE(sink->isSupported());
    CHECK(sink->scope() == contour::platform::TaskbarProgressScope::Application);
    for (auto const state: { vtbackend::ProgressState::Normal,
                             vtbackend::ProgressState::Error,
                             vtbackend::ProgressState::Paused,
                             vtbackend::ProgressState::Indeterminate })
    {
        sink->setApplicationProgress({ .state = state, .percentage = 60 });
        REQUIRE(tile.contentView != nil);
        CHECK(tile.contentView != original);
        // Exercise drawing into a bitmap as well as the Dock's display path.
        auto* image = [tile.contentView bitmapImageRepForCachingDisplayInRect:tile.contentView.bounds];
        REQUIRE(image != nil);
        [tile.contentView cacheDisplayInRect:tile.contentView.bounds toBitmapImageRep:image];
        CHECK((tile.badgeLabel == badge || [tile.badgeLabel isEqualToString:badge]));
    }
    sink->setApplicationProgress({});
    CHECK(tile.contentView == original);
    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Normal, .percentage = 0 });
    CHECK(tile.contentView != original);
    sink.reset();
    CHECK(tile.contentView == original);
}

TEST_CASE("Dock progress restores custom content and preserves a nonempty badge",
          "[contour][taskbar-native][display]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        SKIP("Requires CONTOUR_TEST_DISPLAY=1 and the Cocoa Qt platform.");
    auto* tile = NSApp.dockTile;
    NSView* const original = tile.contentView;
    NSString* const badge = tile.badgeLabel;
    auto* custom = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, tile.size.width, tile.size.height)];
    tile.contentView = custom;
    tile.badgeLabel = @"7";
    auto sink = contour::platform::makeTaskbarProgressSink();
    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Normal, .percentage = 60 });
    CHECK(tile.contentView != custom);
    CHECK([tile.badgeLabel isEqualToString:@"7"]);
    sink->setApplicationProgress({});
    CHECK(tile.contentView == custom);
    CHECK([tile.badgeLabel isEqualToString:@"7"]);

    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Normal, .percentage = 60 });
    // Another owner may replace the tile while our indicator is active; clearing must leave it alone.
    tile.contentView = custom;
    sink.reset();
    CHECK(tile.contentView == custom);
    tile.contentView = original;
    tile.badgeLabel = badge;
    [tile display];
}

TEST_CASE("Dock busy progress animates and determinate progress stops animating",
          "[contour][taskbar-native][display]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        SKIP("Requires CONTOUR_TEST_DISPLAY=1 and the Cocoa Qt platform.");
    auto sink = contour::platform::makeTaskbarProgressSink();
    auto const snapshot = [] {
        auto* view = NSApp.dockTile.contentView;
        auto* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
        return [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    };
    auto const advanceAnimation = [] {
        QEventLoop loop;
        QTimer::singleShot(250, &loop, &QEventLoop::quit);
        loop.exec();
    };
    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Indeterminate });
    auto* before = snapshot();
    REQUIRE(before != nil);
    advanceAnimation();
    CHECK_FALSE([before isEqualToData:snapshot()]);
    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Normal, .percentage = 60 });
    before = snapshot();
    advanceAnimation();
    CHECK([before isEqualToData:snapshot()]);
}
