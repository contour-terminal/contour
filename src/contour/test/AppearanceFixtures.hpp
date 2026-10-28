// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for the operating-system appearance sources: a switch a test flips, and services
/// whose sources read it and are notified through it.

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/NativeCaptionButtons.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/TitleBandMaterial.hpp>
#include <contour/test/SecureInputFixtures.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
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
    /// Run inside apply(), after the request is recorded: what the OS may do synchronously in reply, such
    /// as Qt reporting a new safe area while the toolbar attaches. Empty by default.
    std::function<void()> duringApply {};
    /// What apply() answers a request for the native material; nothing (success) by default.
    std::optional<platform::TitleBandMaterialError> failWith {};
};

/// A title-band material that records into a TitleBandRecord instead of reaching the OS.
class RecordingTitleBandMaterial final: public platform::ITitleBandMaterial
{
  public:
    /// @param record Where to record; it must outlive this material.
    explicit RecordingTitleBandMaterial(TitleBandRecord& record): _record { record } {}

    /// Appends @p request to the record.
    /// @return The record's failWith for a request for the native material, else success.
    [[nodiscard]] std::expected<void, platform::TitleBandMaterialError> apply(
        QWindow& /*window*/, platform::TitleBandRequest const& request) override
    {
        _record.applied.push_back(request);
        if (_record.duringApply)
            _record.duringApply();
        if (_record.failWith && request.backdrop == config::BandBackdrop::NativeMaterial)
            return std::unexpected { *_record.failWith };
        return {};
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

/// What a RecordingCaptionButtons was asked.
struct CaptionRecord
{
    std::vector<QWindow*> attached {};                  ///< Every window attached, in order.
    std::vector<QWindow*> detached {};                  ///< Every window detached, in order.
    std::optional<platform::CaptionBinding> binding {}; ///< The binding of the last attach.
};

/// A caption adapter that records into a CaptionRecord instead of reaching the OS.
class RecordingCaptionButtons final: public platform::INativeCaptionButtons
{
  public:
    /// @param record Where to record; it must outlive this adapter.
    explicit RecordingCaptionButtons(CaptionRecord& record): _record { record } {}

    /// Appends @p window to the record and keeps @p binding.
    void attach(QWindow& window, platform::CaptionBinding binding) override
    {
        _record.attached.push_back(&window);
        _record.binding = std::move(binding);
    }

    /// Appends @p window to the record and drops the binding.
    void detach(QWindow& window) override
    {
        _record.detached.push_back(&window);
        _record.binding.reset();
    }

  private:
    CaptionRecord& _record;
};

/// Services whose factories attach to the given switches: each stores the callback it is handed into
/// its switch and returns a source that reads that switch. Menus get the null presenter, the menu
/// bar the null integration, the title band the null material, secure input the null one, and the caption the
/// null adapter; the frame platform is the build's own and the host is detected.
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
        // The null secure input, so a fixture-built app never turns on the system's Secure Keyboard Entry.
        .secureInput = []() -> std::unique_ptr<platform::ISecureInput> {
            return std::make_unique<platform::NullSecureInput>();
        },
        // The null adapter, so a fixture-built app never answers a window's non-client messages.
        .caption = []() -> std::unique_ptr<platform::INativeCaptionButtons> {
            return std::make_unique<platform::NullNativeCaptionButtons>();
        },
        .framePlatform = platform::currentFramePlatform(),
        .hostPlatform = std::nullopt,
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

/// The services of appearanceServicesWith(AccessibilitySwitch&, AppearanceSwitch&), with a secure input
/// that counts into @p secureInput.
/// @param accessibility The accessibility switch; it must outlive the services and their sources.
/// @param appearance The appearance switch; it must outlive the services and their sources.
/// @param secureInput The record; it must outlive the services and their secure input.
/// @return The services.
[[nodiscard]] inline platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch& accessibility,
                                                                         AppearanceSwitch& appearance,
                                                                         SecureInputRecord& secureInput)
{
    auto services = appearanceServicesWith(accessibility, appearance);
    services.secureInput = [&secureInput]() -> std::unique_ptr<platform::ISecureInput> {
        return std::make_unique<RecordingSecureInput>(secureInput);
    };
    return services;
}

/// The services of appearanceServicesWith(AccessibilitySwitch&, AppearanceSwitch&, TitleBandRecord&), with a
/// caption adapter that records into @p caption.
/// @param accessibility The accessibility switch; it must outlive the services and their sources.
/// @param appearance The appearance switch; it must outlive the services and their sources.
/// @param titleBand The title-band record; it must outlive the services and their material.
/// @param caption The caption record; it must outlive the services and their adapter.
/// @return The services.
[[nodiscard]] inline platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch& accessibility,
                                                                         AppearanceSwitch& appearance,
                                                                         TitleBandRecord& titleBand,
                                                                         CaptionRecord& caption)
{
    auto services = appearanceServicesWith(accessibility, appearance, titleBand);
    services.caption = [&caption]() -> std::unique_ptr<platform::INativeCaptionButtons> {
        return std::make_unique<RecordingCaptionButtons>(caption);
    };
    return services;
}

} // namespace contour::test
