// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure content-scale helpers (display/ContentScale.h): the kcmfonts forced-DPI
// parse rule (extracted from the filesystem-coupled provider) and the null-argument fallbacks of the
// screen/window scale resolvers.

#include <contour/display/ContentScale.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using contour::display::contentScaleForScreen;
using contour::display::contentScaleForWindow;
using contour::display::parseForcedFontDpi;
using namespace std::string_view_literals;

TEST_CASE("parseForcedFontDpi extracts a forcing DPI for the matching key", "[contentscale]")
{
    auto const doc = "[General]\nforceFontDPI=144\nforceFontDPIWayland=120\nother=5\n"sv;

    auto const x11 = parseForcedFontDpi(doc, "forceFontDPI"sv);
    REQUIRE(x11.has_value());
    CHECK(*x11 == 144.0);

    auto const wayland = parseForcedFontDpi(doc, "forceFontDPIWayland"sv);
    REQUIRE(wayland.has_value());
    CHECK(*wayland == 120.0);
}

TEST_CASE("parseForcedFontDpi treats sub-96 and missing values as no-forcing", "[contentscale]")
{
    // < 96 means "no forcing" (resolveContentScale ignores it) -> reported absent.
    CHECK_FALSE(parseForcedFontDpi("forceFontDPI=72\n"sv, "forceFontDPI"sv).has_value());
    // Exactly 96 is the boundary and forces.
    CHECK(parseForcedFontDpi("forceFontDPI=96\n"sv, "forceFontDPI"sv) == 96.0);
    // Missing key.
    CHECK_FALSE(parseForcedFontDpi("somethingElse=200\n"sv, "forceFontDPI"sv).has_value());
    // Non-integer value parses as 0 -> absent.
    CHECK_FALSE(parseForcedFontDpi("forceFontDPI=abc\n"sv, "forceFontDPI"sv).has_value());
    // Empty document.
    CHECK_FALSE(parseForcedFontDpi(""sv, "forceFontDPI"sv).has_value());
}

TEST_CASE("content-scale resolvers fall back to 1.0 for null screen/window without a provider",
          "[contentscale]")
{
    // No screen/window and no forced-DPI provider: the resolver has nothing to scale by and returns
    // the identity scale, which the pre-window headless sizing path relies on.
    CHECK(contentScaleForScreen(nullptr, nullptr) == 1.0);
    CHECK(contentScaleForWindow(nullptr, nullptr) == 1.0);
}
