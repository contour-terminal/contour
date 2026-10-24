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

    /// Reads the system accent colour and observes the system colour change notification.
    class CocoaPlatformAppearance final: public IPlatformAppearance
    {
      public:
        explicit CocoaPlatformAppearance(PlatformAppearanceChanged onChange):
            _onChange { std::move(onChange) },
            // Posted on the main thread, so with queue:nil the block runs on the GUI thread. The block
            // captures `this`; the destructor removes the observer before it dangles.
            _observer { [NSNotificationCenter.defaultCenter
                addObserverForName:NSSystemColorsDidChangeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          if (_onChange)
                              _onChange(current());
                        }] }
        {
        }

        CocoaPlatformAppearance(CocoaPlatformAppearance const&) = delete;
        CocoaPlatformAppearance& operator=(CocoaPlatformAppearance const&) = delete;
        CocoaPlatformAppearance(CocoaPlatformAppearance&&) = delete;
        CocoaPlatformAppearance& operator=(CocoaPlatformAppearance&&) = delete;

        ~CocoaPlatformAppearance() override { [NSNotificationCenter.defaultCenter removeObserver:_observer]; }

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
            if (srgb == nil)
                return PlatformAppearance { .accent = std::nullopt, .windowCornerRadius = std::nullopt };
            // macOS publishes no window corner radius; the style's row carries the measured constant.
            return PlatformAppearance {
                .accent = vtbackend::RGBColor { toChannel(srgb.redComponent),
                                                toChannel(srgb.greenComponent),
                                                toChannel(srgb.blueComponent) },
                .windowCornerRadius = std::nullopt,
            };
        }

      private:
        PlatformAppearanceChanged _onChange;
        id<NSObject> __strong _observer = nil; ///< ARC keeps the observation token until removal.
    };
} // namespace

std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange)
{
    return std::make_unique<CocoaPlatformAppearance>(std::move(onChange));
}
} // namespace contour::platform
