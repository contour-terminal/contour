// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/test/HotkeyFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MotionPreference;
using contour::config::TransparencyPreference;
using contour::platform::PlatformAppearance;

TEST_CASE("The null sources report the defaults", "[appearance-source]")
{
    CHECK(contour::platform::NullAccessibilityPreferenceSource {}.current() == AccessibilityPreferences {});
    CHECK(contour::platform::NullPlatformAppearance {}.current() == PlatformAppearance {});
    // A platform that publishes no scroller style leaves the scrollbar to the profile's mode.
    CHECK_FALSE(contour::platform::NullPlatformAppearance {}.current().scrollBars.has_value());
}

TEST_CASE("nullAppearanceServices builds silent default sources", "[appearance-source]")
{
    auto const services = contour::platform::nullAppearanceServices();
    auto accessibilityCalls = 0;
    auto appearanceCalls = 0;
    auto const accessibility =
        services.accessibility([&](AccessibilityPreferences) { ++accessibilityCalls; });
    auto const appearance = services.appearance([&](PlatformAppearance const&) { ++appearanceCalls; });
    REQUIRE(accessibility != nullptr);
    REQUIRE(appearance != nullptr);
    CHECK(accessibility->current() == AccessibilityPreferences {});
    CHECK(appearance->current() == PlatformAppearance {});
    CHECK(accessibilityCalls == 0);
    CHECK(appearanceCalls == 0);
}

TEST_CASE("appearanceServicesWith drives sources from the switches", "[appearance-source]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    auto const services = contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch);

    auto received = std::vector<AccessibilityPreferences> {};
    auto const source =
        services.accessibility([&](AccessibilityPreferences next) { received.push_back(next); });
    REQUIRE(source != nullptr);
    CHECK(source->current() == AccessibilityPreferences {});
    CHECK(received.empty());

    auto const reduced = AccessibilityPreferences { .motion = MotionPreference::Reduced,
                                                    .transparency = TransparencyPreference::Allowed,
                                                    .contrast = ContrastPreference::Standard };
    accessibilitySwitch.set(reduced);
    CHECK(source->current().motion == MotionPreference::Reduced);
    REQUIRE(received.size() == 1);
    CHECK(received.front() == reduced);

    auto appearances = std::vector<PlatformAppearance> {};
    auto const appearance =
        services.appearance([&](PlatformAppearance const& next) { appearances.push_back(next); });
    REQUIRE(appearance != nullptr);
    auto const accented = PlatformAppearance { .accent = vtbackend::RGBColor { 0x0A, 0x84, 0xFF },
                                               .windowCornerRadius = 10,
                                               .scrollBars = contour::config::ScrollBarMode::Always,
                                               .colorScheme = std::nullopt,
                                               .buttonLayout = std::nullopt };
    appearanceSwitch.set(accented);
    CHECK(appearance->current() == accented);
    REQUIRE(appearances.size() == 1);
    CHECK(appearances.front() == accented);
}

TEST_CASE("A destroyed switched source no longer receives changes", "[appearance-source]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    auto const services = contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch);
    auto accessibilityCalls = 0;
    auto appearanceCalls = 0;
    auto accessibility = services.accessibility([&](AccessibilityPreferences) { ++accessibilityCalls; });
    auto appearance = services.appearance([&](PlatformAppearance const&) { ++appearanceCalls; });
    accessibility.reset();
    appearance.reset();

    accessibilitySwitch.set({ .motion = MotionPreference::Reduced,
                              .transparency = TransparencyPreference::Allowed,
                              .contrast = ContrastPreference::Standard });
    appearanceSwitch.set({ .accent = vtbackend::RGBColor { 0x0A, 0x84, 0xFF },
                           .windowCornerRadius = 10,
                           .scrollBars = contour::config::ScrollBarMode::Overlay,
                           .colorScheme = std::nullopt,
                           .buttonLayout = std::nullopt });
    CHECK(accessibilityCalls == 0);
    CHECK(appearanceCalls == 0);
    CHECK_FALSE(accessibilitySwitch.notify);
    CHECK_FALSE(appearanceSwitch.notify);
    CHECK(accessibilitySwitch.preferences.motion == MotionPreference::Reduced);
}

TEST_CASE("A switch with no source attached only stores", "[appearance-source]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    accessibilitySwitch.set({ .motion = MotionPreference::Full,
                              .transparency = TransparencyPreference::Reduced,
                              .contrast = ContrastPreference::Increased });
    CHECK(accessibilitySwitch.preferences.contrast == ContrastPreference::Increased);
}

TEST_CASE("Both service sets build a null menu presenter", "[appearance-source]")
{
    using contour::platform::MenuPresentError;
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    for (auto const& services:
         { contour::platform::nullAppearanceServices(),
           contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch) })
    {
        auto const presenter = services.menus();
        REQUIRE(presenter != nullptr);
        auto picks = 0;
        auto const result = presenter->present(
            contour::command::MenuTree {}, contour::platform::MenuAnchor {}, [&](std::size_t) { ++picks; });
        CHECK(result == std::unexpected(MenuPresentError::Unsupported));
        CHECK(picks == 0);
    }
}

TEST_CASE("Both service sets build a null menu bar integration", "[appearance-source]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    for (auto const& services:
         { contour::platform::nullAppearanceServices(),
           contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch) })
    {
        auto const integration = services.menuBar();
        REQUIRE(integration != nullptr);
        CHECK(dynamic_cast<contour::platform::NullMenuBarIntegration const*>(integration.get()) != nullptr);
    }
}

TEST_CASE("Both service sets build a null secure input", "[appearance-source][secureinput]")
{
    // A fixture-built app must never reach the OS's secure keyboard entry: the system-wide state would leak
    // out of the test run.
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    for (auto const& services:
         { contour::platform::nullAppearanceServices(),
           contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch) })
    {
        auto const secureInput = services.secureInput();
        REQUIRE(secureInput != nullptr);
        CHECK(secureInput->support() == contour::platform::SecureInputSupport::None);
    }
}

TEST_CASE("appearanceServicesWith counts secure input into a record", "[appearance-source][secureinput]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    auto record = contour::test::SecureInputRecord {};
    auto const services =
        contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch, record);
    {
        auto const secureInput = services.secureInput();
        REQUIRE(secureInput != nullptr);
        CHECK(secureInput->support() == contour::platform::SecureInputSupport::Native);
        secureInput->enable();
        secureInput->disable();
    }
    // Still readable after the object is gone, as a test reads it after the app that owned it.
    CHECK(record.enables == 1);
    CHECK(record.disables == 1);
}

TEST_CASE("the null appearance services answer every global hotkey as unsupported",
          "[appearance-source][globalhotkey]")
{
    // A fixture-built app must never register a chord with the real desktop.
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    for (auto const& services:
         { contour::platform::nullAppearanceServices(),
           contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch) })
    {
        auto const hotkey = services.globalHotkey();
        REQUIRE(hotkey != nullptr);
        auto settled = std::optional<contour::platform::HotkeyResult> {};
        auto const result = hotkey->registerHotkey(
            contour::config::HotkeyChord { .modifiers = vtbackend::Modifier::Control, .key = U'`' },
            [](std::string const&) {},
            [&settled](contour::platform::HotkeyResult answer) { settled = answer; });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == contour::platform::HotkeyError::Unsupported);
        REQUIRE(settled.has_value());
        CHECK_FALSE(settled->has_value());
        hotkey->unregisterHotkey();
    }
}

TEST_CASE("a recording hotkey records successes and delivers presses", "[globalhotkey]")
{
    auto hotkey = contour::test::RecordingGlobalHotkey {};
    auto tokens = std::vector<std::string> {};
    auto const chord =
        contour::config::HotkeyChord { .modifiers = vtbackend::Modifier::Control, .key = U'`' };
    REQUIRE(
        hotkey.registerHotkey(chord, [&tokens](std::string token) { tokens.push_back(std::move(token)); }, {})
            .has_value());
    hotkey.press("tok");
    CHECK(tokens == std::vector<std::string> { "tok" });
    CHECK(hotkey.registered == std::vector { chord });

    hotkey.unregisterHotkey();
    hotkey.press("late");
    CHECK(tokens.size() == 1);
    CHECK(hotkey.unregisters == 1);

    hotkey.result = std::unexpected(contour::platform::HotkeyError::AlreadyTaken);
    auto const taken = hotkey.registerHotkey(chord, [](std::string const&) {}, {});
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error() == contour::platform::HotkeyError::AlreadyTaken);
    CHECK(hotkey.registered.size() == 1);
}
