// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure content-scale helpers (display/ContentScale.h): the kcmfonts forced-DPI
// parse rule (extracted from the filesystem-coupled provider) and the null-argument fallbacks of the
// screen/window scale resolvers.

#include <contour/display/ContentScale.hpp>
#include <contour/geometry/WindowGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using contour::display::contentScaleForScreen;
using contour::display::contentScaleForWindow;
using contour::display::devicePixelRatioForWindow;
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
    CHECK(devicePixelRatioForWindow(nullptr) == 1.0);
}

// {{{ The two scales are different questions (#2040).
//
// contentScale answers "how large should glyphs be rasterized" and lets KDE's forceFontDPI replace the
// device-pixel ratio outright to make text physically bigger. devicePixelRatio answers "how many hardware
// pixels is a logical pixel" — a property of the surface that no font setting may change, and the one Qt
// builds its projection and sizes its render target with.
//
// Conflating them fits the grid into `width * fontScale` pixels while the surface only has `width * dpr`
// of them: too many columns, and the whole grid rescaled onto the surface. With the glyph atlas sampled at
// QRhiSampler::Nearest that drops and duplicates glyph columns rather than merely blurring.
TEST_CASE("a forced font DPI moves the content scale but never the device-pixel ratio", "[contentscale]")
{
    using contour::geometry::resolveContentScale;

    // KDE forcing 144 DPI on a 1.0-DPR screen: glyphs are rasterized 1.5x larger...
    CHECK(resolveContentScale(144.0, 1.0, std::nullopt) == 1.5);

    // ... while the surface still has exactly one hardware pixel per logical pixel. devicePixelRatioForWindow
    // consults no provider at all, which is what guarantees this; with no window it is the identity.
    CHECK(devicePixelRatioForWindow(nullptr) == 1.0);
}

TEST_CASE("without a forced font DPI the two scales agree exactly", "[contentscale]")
{
    using contour::geometry::resolveContentScale;

    // The safety property behind splitting the roles: on every platform that does not force a font DPI
    // (i.e. everything but KDE-with-forcing), the content scale IS the device-pixel ratio, so re-pointing
    // every geometry call site at the DPR changes nothing observable.
    for (auto const dpr: { 1.0, 1.25, 1.5, 1.75, 2.0 })
    {
        INFO("dpr = " << dpr);
        CHECK(resolveContentScale(std::nullopt, dpr, std::nullopt) == dpr);
    }
}

TEST_CASE("a sub-96 forced DPI is not forcing, so the ratio still decides", "[contentscale]")
{
    using contour::geometry::resolveContentScale;

    // parseForcedFontDpi already reports <96 as absent; resolveContentScale ignores it independently, so a
    // value that slipped through cannot silently shrink the grid.
    CHECK(resolveContentScale(72.0, 1.25, std::nullopt) == 1.25);
}
// }}}
