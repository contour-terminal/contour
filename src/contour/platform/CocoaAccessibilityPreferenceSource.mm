// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AccessibilityPreferenceSource.hpp>

#include <utility>

#import <AppKit/AppKit.h>

namespace contour::platform
{
namespace
{
    /// Reads NSWorkspace's accessibility display options and observes their change notification.
    class CocoaAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource
    {
      public:
        explicit CocoaAccessibilityPreferenceSource(AccessibilityPreferencesChanged onChange):
            _onChange { std::move(onChange) },
            // NSWorkspace posts this on the main thread, so with queue:nil the block runs on the GUI
            // thread. The block captures `this`; the destructor removes the observer before it dangles.
            _observer { [NSWorkspace.sharedWorkspace.notificationCenter
                addObserverForName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          if (_onChange)
                              _onChange(current());
                        }] }
        {
        }

        CocoaAccessibilityPreferenceSource(CocoaAccessibilityPreferenceSource const&) = delete;
        CocoaAccessibilityPreferenceSource& operator=(CocoaAccessibilityPreferenceSource const&) = delete;
        CocoaAccessibilityPreferenceSource(CocoaAccessibilityPreferenceSource&&) = delete;
        CocoaAccessibilityPreferenceSource& operator=(CocoaAccessibilityPreferenceSource&&) = delete;

        ~CocoaAccessibilityPreferenceSource() override
        {
            [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:_observer];
        }

        [[nodiscard]] config::AccessibilityPreferences current() const override
        {
            auto* const workspace = NSWorkspace.sharedWorkspace;
            return config::AccessibilityPreferences {
                .motion = workspace.accessibilityDisplayShouldReduceMotion ? config::MotionPreference::Reduced
                                                                           : config::MotionPreference::Full,
                .transparency = workspace.accessibilityDisplayShouldReduceTransparency
                                    ? config::TransparencyPreference::Reduced
                                    : config::TransparencyPreference::Allowed,
                .contrast = workspace.accessibilityDisplayShouldIncreaseContrast
                                ? config::ContrastPreference::Increased
                                : config::ContrastPreference::Standard,
            };
        }

      private:
        AccessibilityPreferencesChanged _onChange;
        id<NSObject> __strong _observer = nil; ///< ARC keeps the observation token until removal.
    };
} // namespace

std::unique_ptr<IAccessibilityPreferenceSource> makeAccessibilityPreferenceSource(
    AccessibilityPreferencesChanged onChange)
{
    return std::make_unique<CocoaAccessibilityPreferenceSource>(std::move(onChange));
}
} // namespace contour::platform
