// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ColorPalette.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("ColorPalette.foldMarker.derivesWhenUnset", "[ColorPalette]")
{
    auto const palette = ColorPalette {};
    REQUIRE_FALSE(palette.foldMarker.has_value());
    REQUIRE_FALSE(palette.foldMarkerHover.has_value());

    auto const rest = palette.foldMarkerColors();

    // At rest the column sits on the page's own background, EXACTLY -- BackgroundRenderer::renderCell
    // skips a cell whose background is the default one, so the gutter paints no rectangle and a
    // transparent window stays transparent behind it.
    CHECK(rest.background == palette.defaultBackground);

    // ... and the glyph is faded off the text colour without reaching the page behind it, which is
    // what pins FoldMarkerRestFade strictly between the two.
    CHECK(rest.foreground != palette.defaultForeground);
    CHECK(rest.foreground != palette.defaultBackground);
}

TEST_CASE("ColorPalette.foldMarker.hoverRecolorizesRatherThanInverts", "[ColorPalette]")
{
    auto const palette = ColorPalette {};
    auto const rest = palette.foldMarkerColors();
    auto const hover = palette.foldMarkerHoverColors();

    // The regression this derivation exists for. Hovering used to swap the pair, drawing the run as a
    // solid bar with the marks punched out of it; a return to that puts defaultBackground here, so
    // this one assertion is the whole anti-inversion guard.
    CHECK(hover.foreground == palette.defaultForeground);

    // Only the glyph moves. The background stays the page's own in BOTH states, so the gutter never
    // paints a rectangle and a transparent window stays transparent under the pointer too.
    CHECK(hover.background == rest.background);
    CHECK(hover.background == palette.defaultBackground);

    // ... and the glyph moving is what the hover has to be carried by, so it has to be a real step.
    CHECK(distance(hover.foreground, rest.foreground) > 45.0);
}

TEST_CASE("ColorPalette.foldMarker.explicitColorsBypassTheDerivation", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    auto const configured = RGBColorPair { .foreground = 0x010203_rgb, .background = 0x040506_rgb };

    SECTION("the resting pair alone")
    {
        palette.foldMarker = configured;
        CHECK(palette.foldMarkerColors().foreground == configured.foreground);
        CHECK(palette.foldMarkerColors().background == configured.background);
        // The other stays derived: the two are independent options, not one block.
        CHECK(palette.foldMarkerHoverColors().foreground == palette.defaultForeground);
    }

    SECTION("the hovered pair alone")
    {
        palette.foldMarkerHover = configured;
        CHECK(palette.foldMarkerHoverColors().foreground == configured.foreground);
        CHECK(palette.foldMarkerHoverColors().background == configured.background);
        CHECK(palette.foldMarkerColors().background == palette.defaultBackground);
    }
}

TEST_CASE("ColorPalette.foldMarker.isLegibleInTheBuiltinSchemes", "[ColorPalette]")
{
    // Every built-in scheme, asked for rather than re-typed, so a scheme added tomorrow is covered.
    // The light ones are the point: they leave defaultForegroundBright at the struct's dark-theme
    // white, so a hover derived from it would be invisible on them.
    auto const names = defaultColorPaletteNames();
    REQUIRE(names.size() >= 10);

    for (auto const name: names)
    {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));

        INFO("scheme: " << name);

        // Sourcing the hover from defaultForegroundBright would fail exactly here: one-light,
        // gruvbox-light, solarized-light and papercolor-light never set it, so it stays 0xFFFFFF and
        // the hovered glyph would be white on a near-white page.
        auto const hover = palette.foldMarkerHoverColors();
        CHECK(distance(hover.foreground, hover.background) > 90.0);

        // And FoldMarkerRestFade has to leave the resting glyph visible, however far it fades it --
        // while still stepping far enough off the hovered one to be seen changing, which is the whole
        // hover cue now that the background no longer moves.
        auto const rest = palette.foldMarkerColors();
        CHECK(distance(rest.foreground, rest.background) > 45.0);
        CHECK(distance(rest.foreground, hover.foreground) > 45.0);
    }
}

TEST_CASE("ColorPalette.foldMarker.defaultSchemeMatchesTheDocumentedColors", "[ColorPalette]")
{
    // Three places outside this header transcribe what the default scheme derives, as literals: the
    // commented example in ConfigDocumentation.hpp's FoldMarkerConfig -- which promises in so many
    // words that uncommenting it changes nothing -- and both the fold_marker section and the default
    // scheme dump in docs/configuration/colors.md.
    //
    // Nothing else checks that promise, and Config_test.cpp cannot: it tests literal PARSING, so it
    // stays green while the documented colours drift. Retuning FoldMarkerRestFade fails
    // here instead, naming the files that then have to be updated.
    auto const palette = ColorPalette {};
    auto const rest = palette.foldMarkerColors();
    auto const hover = palette.foldMarkerHoverColors();

    CHECK(rest.foreground == 0x7e7c7c_rgb);
    CHECK(rest.background == 0x1a1716_rgb);
    CHECK(hover.foreground == 0xd0d0d0_rgb);
    CHECK(hover.background == 0x1a1716_rgb);
}
