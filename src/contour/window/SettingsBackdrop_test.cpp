// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsBackdrop.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::config::BackdropKind;
using contour::config::BandBackdrop;
using contour::config::ChromeStyle;
using contour::config::Material;
using contour::window::settingsBackdropFor;

TEST_CASE("Mica behind the Settings window's title bar on the Fluent row", "[settingsbackdrop]")
{
    for (auto const scheme: { Qt::ColorScheme::Light, Qt::ColorScheme::Dark })
    {
        auto const request =
            settingsBackdropFor({ .row = ChromeStyle::Fluent, .material = Material::Mica, .scheme = scheme });
        REQUIRE(request.has_value());
        CHECK(request->backdrop == BandBackdrop::NativeMaterial);
        // Settings is a single-content window: Mica, not the tabbed window's Mica Alt.
        CHECK(request->kind == BackdropKind::Main);
        CHECK(request->growth == contour::config::TitleBarGrowth::None);
        CHECK(request->scheme == scheme);
    }
}

TEST_CASE("no backdrop for the Settings window on any other row or material", "[settingsbackdrop]")
{
    // Mica taken away (Reduced transparency, a contrast theme, an OS without it) leaves Solid.
    CHECK_FALSE(
        settingsBackdropFor(
            { .row = ChromeStyle::Fluent, .material = Material::Solid, .scheme = Qt::ColorScheme::Dark })
            .has_value());
    // macOS keeps its Settings window as it is (1d R-h).
    CHECK_FALSE(
        settingsBackdropFor(
            { .row = ChromeStyle::MacOS, .material = Material::Glass, .scheme = Qt::ColorScheme::Dark })
            .has_value());
    CHECK_FALSE(
        settingsBackdropFor(
            { .row = ChromeStyle::Terminal, .material = Material::None, .scheme = Qt::ColorScheme::Light })
            .has_value());
    CHECK_FALSE(settingsBackdropFor({ .row = ChromeStyle::ContourLinux,
                                      .material = Material::Mica,
                                      .scheme = Qt::ColorScheme::Light })
                    .has_value());
}
