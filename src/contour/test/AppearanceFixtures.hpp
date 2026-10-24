// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for the operating-system appearance sources: a switch a test flips, and services
/// whose sources read it and are notified through it.

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <memory>
#include <utility>

namespace contour::test
{

/// The accessibility preferences a test sets, and the change callback of the source reading them.
struct AccessibilitySwitch
{
    config::AccessibilityPreferences preferences {};     ///< What the source reports.
    platform::AccessibilityPreferencesChanged notify {}; ///< The callback the factory was handed.

    /// Stores the preferences and, when a source is attached, reports them as an OS change would.
    /// @param next The new preferences.
    void set(config::AccessibilityPreferences next)
    {
        preferences = next;
        if (notify)
            notify(preferences);
    }
};

/// The platform appearance a test sets, and the change callback of the source reading it.
struct AppearanceSwitch
{
    platform::PlatformAppearance appearance {};    ///< What the source reports.
    platform::PlatformAppearanceChanged notify {}; ///< The callback the factory was handed.

    /// Stores the appearance and, when a source is attached, reports it as an OS change would.
    /// @param next The new appearance.
    void set(platform::PlatformAppearance next)
    {
        appearance = next;
        if (notify)
            notify(appearance);
    }
};

/// An accessibility source that reads an AccessibilitySwitch.
class SwitchedAccessibilityPreferenceSource final: public platform::IAccessibilityPreferenceSource
{
  public:
    /// @param source The switch to read and to detach from on destruction; it must outlive this source.
    explicit SwitchedAccessibilityPreferenceSource(AccessibilitySwitch& source): _source { source } {}

    SwitchedAccessibilityPreferenceSource(SwitchedAccessibilityPreferenceSource const&) = delete;
    SwitchedAccessibilityPreferenceSource& operator=(SwitchedAccessibilityPreferenceSource const&) = delete;
    SwitchedAccessibilityPreferenceSource(SwitchedAccessibilityPreferenceSource&&) = delete;
    SwitchedAccessibilityPreferenceSource& operator=(SwitchedAccessibilityPreferenceSource&&) = delete;

    /// Detaches from the switch, so a later set() reports to nobody, as a destroyed OS source would.
    ~SwitchedAccessibilityPreferenceSource() override { _source.notify = nullptr; }

    /// @return The switch's current preferences.
    [[nodiscard]] config::AccessibilityPreferences current() const override { return _source.preferences; }

  private:
    AccessibilitySwitch& _source;
};

/// A platform appearance source that reads an AppearanceSwitch.
class SwitchedPlatformAppearance final: public platform::IPlatformAppearance
{
  public:
    /// @param source The switch to read and to detach from on destruction; it must outlive this source.
    explicit SwitchedPlatformAppearance(AppearanceSwitch& source): _source { source } {}

    SwitchedPlatformAppearance(SwitchedPlatformAppearance const&) = delete;
    SwitchedPlatformAppearance& operator=(SwitchedPlatformAppearance const&) = delete;
    SwitchedPlatformAppearance(SwitchedPlatformAppearance&&) = delete;
    SwitchedPlatformAppearance& operator=(SwitchedPlatformAppearance&&) = delete;

    /// Detaches from the switch, so a later set() reports to nobody, as a destroyed OS source would.
    ~SwitchedPlatformAppearance() override { _source.notify = nullptr; }

    /// @return The switch's current appearance.
    [[nodiscard]] platform::PlatformAppearance current() const override { return _source.appearance; }

  private:
    AppearanceSwitch& _source;
};

/// Services whose factories attach to the given switches: each stores the callback it is handed into
/// its switch and returns a source that reads that switch.
/// @param accessibility The accessibility switch; it must outlive the services and their sources.
/// @param appearance The appearance switch; it must outlive the services and their sources.
/// @return The services.
[[nodiscard]] inline platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch& accessibility,
                                                                         AppearanceSwitch& appearance)
{
    return platform::AppearanceServices {
        .accessibility = [&accessibility](platform::AccessibilityPreferencesChanged onChange)
            -> std::unique_ptr<platform::IAccessibilityPreferenceSource> {
            accessibility.notify = std::move(onChange);
            return std::make_unique<SwitchedAccessibilityPreferenceSource>(accessibility);
        },
        .appearance = [&appearance](platform::PlatformAppearanceChanged onChange)
            -> std::unique_ptr<platform::IPlatformAppearance> {
            appearance.notify = std::move(onChange);
            return std::make_unique<SwitchedPlatformAppearance>(appearance);
        },
    };
}

} // namespace contour::test
