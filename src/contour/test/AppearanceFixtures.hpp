// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for the operating-system appearance sources: a switch a test flips, and services
/// whose sources read it and are notified through it.

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <memory>
#include <utility>
#include <vector>

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

/// What a RecordingTitleBandMaterial was asked, and what it answers.
struct TitleBandRecord
{
    std::vector<platform::TitleBandRequest> applied {}; ///< Every request applied, in order.
    platform::NativeTitleBarMetrics metrics {};         ///< What metrics() answers.
    int measured = 0;                                   ///< How often metrics() was called.
};

/// A title-band material that records into a TitleBandRecord instead of reaching the OS.
class RecordingTitleBandMaterial final: public platform::ITitleBandMaterial
{
  public:
    /// @param record Where to record; it must outlive this material.
    explicit RecordingTitleBandMaterial(TitleBandRecord& record): _record { record } {}

    /// Appends @p request to the record.
    void apply(QWindow& /*window*/, platform::TitleBandRequest const& request) override
    {
        _record.applied.push_back(request);
    }

    /// Counts the call.
    /// @return The record's metrics.
    [[nodiscard]] platform::NativeTitleBarMetrics metrics(QWindow& /*window*/) const override
    {
        ++_record.measured;
        return _record.metrics;
    }

  private:
    TitleBandRecord& _record;
};

/// Services whose factories attach to the given switches: each stores the callback it is handed into
/// its switch and returns a source that reads that switch. Menus get the null presenter, the menu
/// bar the null integration, and the title band the null material.
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
        // The null presenter, so a fixture-built app never reaches the native factory.
        .menus = []() -> std::unique_ptr<platform::INativeMenuPresenter> {
            return std::make_unique<platform::NullNativeMenuPresenter>();
        },
        // The null integration, so a fixture-built app never hands a menu to AppKit.
        .menuBar = []() -> std::unique_ptr<platform::IMenuBarIntegration> {
            return std::make_unique<platform::NullMenuBarIntegration>();
        },
        // The null material, so a fixture-built app never dresses an NSWindow.
        .titleBand = []() -> std::unique_ptr<platform::ITitleBandMaterial> {
            return std::make_unique<platform::NullTitleBandMaterial>();
        },
    };
}

/// The services of appearanceServicesWith(AccessibilitySwitch&, AppearanceSwitch&), with a title-band
/// material that records into @p titleBand.
/// @param accessibility The accessibility switch; it must outlive the services and their sources.
/// @param appearance The appearance switch; it must outlive the services and their sources.
/// @param titleBand The record; it must outlive the services and their material.
/// @return The services.
[[nodiscard]] inline platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch& accessibility,
                                                                         AppearanceSwitch& appearance,
                                                                         TitleBandRecord& titleBand)
{
    auto services = appearanceServicesWith(accessibility, appearance);
    services.titleBand = [&titleBand]() -> std::unique_ptr<platform::ITitleBandMaterial> {
        return std::make_unique<RecordingTitleBandMaterial>(titleBand);
    };
    return services;
}

} // namespace contour::test
