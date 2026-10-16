// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TaskbarProgressSink.hpp>

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
