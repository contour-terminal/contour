// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/PlatformAppearance.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#import <AppKit/AppKit.h>

namespace contour::platform
{
namespace
{
    /// @return The colour channel as an 8-bit value.
    [[nodiscard]] uint8_t toChannel(CGFloat value)
    {
        return static_cast<uint8_t>(std::lround(std::clamp(value, CGFloat { 0 }, CGFloat { 1 }) * 255.0));
    }

    /// The scroller style AppKit prefers now: Overlay for NSScrollerStyleOverlay, Always for the legacy one.
    /// @pre Called on the main thread.
    [[nodiscard]] config::ScrollBarMode preferredScrollBars()
    {
        return NSScroller.preferredScrollerStyle == NSScrollerStyleOverlay ? config::ScrollBarMode::Overlay
                                                                            : config::ScrollBarMode::Always;
    }

    /// Reads the system accent colour and scroller style, and observes the notifications that change them.
    class CocoaPlatformAppearance final: public IPlatformAppearance
    {
      public:
        explicit CocoaPlatformAppearance(PlatformAppearanceChanged onChange):
            _onChange { std::move(onChange) },
            _colorsObserver { observe(NSSystemColorsDidChangeNotification) },
            // "Show scroll bars: Automatically" flips the style when a mouse is plugged in or removed.
            _scrollerObserver { observe(NSPreferredScrollerStyleDidChangeNotification) }
        {
        }

        CocoaPlatformAppearance(CocoaPlatformAppearance const&) = delete;
        CocoaPlatformAppearance& operator=(CocoaPlatformAppearance const&) = delete;
        CocoaPlatformAppearance(CocoaPlatformAppearance&&) = delete;
        CocoaPlatformAppearance& operator=(CocoaPlatformAppearance&&) = delete;

        ~CocoaPlatformAppearance() override
        {
            [NSNotificationCenter.defaultCenter removeObserver:_colorsObserver];
            [NSNotificationCenter.defaultCenter removeObserver:_scrollerObserver];
        }

        [[nodiscard]] PlatformAppearance current() const override
        {
            // controlAccentColor is a dynamic colour: resolve it under the application's effective
            // (light or dark) appearance, when there is an application, so the value is the one the
            // chrome is drawn against.
            __block NSColor* srgb = nil;
            auto const resolve = ^{
              srgb = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            };
            if (NSApp != nil)
                [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:resolve];
            else
                resolve();
            // macOS publishes no window corner radius; the style's row carries the measured constant.
            return PlatformAppearance {
                .accent = srgb == nil ? std::nullopt
                                      : std::optional { vtbackend::RGBColor { toChannel(srgb.redComponent),
                                                                              toChannel(srgb.greenComponent),
                                                                              toChannel(srgb.blueComponent) } },
                .windowCornerRadius = std::nullopt,
                .scrollBars = preferredScrollBars(),
                .materials = config::MaterialSupport::Unknown,
                .contrastTheme = ContrastTheme::None,
            };
        }

      private:
        /// Observes @p name, reporting current() through the change callback.
        /// @return The observation token, to remove in the destructor.
        id<NSObject> observe(NSNotificationName name)
        {
            // Posted on the main thread, so with queue:nil the block runs on the GUI thread. The block
            // captures `this`; the destructor removes the observer before it dangles.
            return [NSNotificationCenter.defaultCenter addObserverForName:name
                                                                   object:nil
                                                                    queue:nil
                                                               usingBlock:^(NSNotification*) {
                                                                 if (_onChange)
                                                                     _onChange(current());
                                                               }];
        }

        PlatformAppearanceChanged _onChange;
        id<NSObject> __strong _colorsObserver = nil;   ///< ARC keeps the observation token until removal.
        id<NSObject> __strong _scrollerObserver = nil; ///< Likewise, for the scroller style.
    };
} // namespace

std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange)
{
    return std::make_unique<CocoaPlatformAppearance>(std::move(onChange));
}
} // namespace contour::platform
