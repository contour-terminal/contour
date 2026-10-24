// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

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

TEST_CASE("ColorPalette.foldMarker.keepsThreeToOneContrastAtRest", "[ColorPalette]")
{
    // WCAG 1.4.11's floor for a control a user must make out: the resting fade stops where it would go
    // under it, which the solarized schemes reached at 2.4:1 (dark) and 2.0:1 (light). The default scheme,
    // at 4.3:1, keeps its colour (@see defaultSchemeMatchesTheDocumentedColors).
    for (auto const name: defaultColorPaletteNames())
    {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        INFO("scheme: " << name);

        auto const rest = palette.foldMarkerColors();
        if (contrastRatio(palette.defaultForeground, palette.defaultBackground)
            >= ColorPalette::FoldMarkerRestMinimumContrast)
            CHECK(contrastRatio(rest.foreground, rest.background)
                  >= ColorPalette::FoldMarkerRestMinimumContrast);

        // Still quieter than the text it sits beside.
        CHECK(contrastRatio(rest.foreground, rest.background)
              <= contrastRatio(palette.defaultForeground, palette.defaultBackground));
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

TEST_CASE("ColorPalette.blockStatus.derivesWhenUnset", "[ColorPalette]")
{
    auto const palette = ColorPalette {};

    // A success looks like the fold column always did; a failure and a running command take the
    // palette's own red and blue, which every scheme already tunes to read on its background.
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == palette.foldMarkerColors().foreground);
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == palette.normalColor(1));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == palette.normalColor(4));

    // Line numbers and timestamps are as quiet as the resting fold column.
    CHECK(palette.gutterTextColor() == palette.foldMarkerColors().foreground);
}

TEST_CASE("ColorPalette.blockStatus.followsThePalettesOwnColours", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    palette.palette[1] = RGBColor(0x11, 0x22, 0x33);
    palette.palette[4] = RGBColor(0x44, 0x55, 0x66);
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == RGBColor(0x11, 0x22, 0x33));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == RGBColor(0x44, 0x55, 0x66));
}

TEST_CASE("ColorPalette.blockStatus.followsTheSchemesForegroundAndBackground", "[ColorPalette]")
{
    // The default scheme alone cannot tell a derivation from a constant that happens to equal it, so
    // move the scheme's own text and page colours and require the unset slots to move with them.
    auto palette = ColorPalette {};
    palette.defaultForeground = RGBColor(0x20, 0x20, 0x20);
    palette.defaultBackground = RGBColor(0xF0, 0xF0, 0xF0);
    auto const faded =
        mixColor(palette.defaultForeground, palette.defaultBackground, ColorPalette::FoldMarkerRestFade);

    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == faded);
    CHECK(palette.gutterTextColor() == faded);
}

TEST_CASE("ColorPalette.blockStatus.successFollowsAConfiguredFoldColumn", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    palette.foldMarker =
        RGBColorPair { .foreground = RGBColor(0x0D, 0x0E, 0x0F), .background = palette.defaultBackground };

    // A success is drawn as the fold column is, so it follows the scheme's choice for that column ...
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == RGBColor(0x0D, 0x0E, 0x0F));

    // ... while the gutter's text stays the faded foreground: a coloured fold column is not a request
    // to colour the line numbers and timestamps too.
    CHECK(
        palette.gutterTextColor()
        == mixColor(palette.defaultForeground, palette.defaultBackground, ColorPalette::FoldMarkerRestFade));
}

TEST_CASE("ColorPalette.blockStatus.explicitColorsBypassTheDerivation", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    // Indexed by the outcome: success, failure, running.
    palette.blockStatus = { RGBColor(0x01, 0x02, 0x03),
                            RGBColor(0x04, 0x05, 0x06),
                            RGBColor(0x07, 0x08, 0x09) };
    palette.gutterText = RGBColor(0x0A, 0x0B, 0x0C);

    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == RGBColor(0x01, 0x02, 0x03));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == RGBColor(0x04, 0x05, 0x06));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == RGBColor(0x07, 0x08, 0x09));
    CHECK(palette.gutterTextColor() == RGBColor(0x0A, 0x0B, 0x0C));
}

TEST_CASE("ColorPalette.gutterTextColors.putTheTextOnThePagesOwnBackground", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    palette.gutterText = RGBColor(0x0A, 0x0B, 0x0C);
    palette.defaultBackground = RGBColor(0xF0, 0xF0, 0xF0);
    CHECK(palette.gutterTextColors()
          == RGBColorPair { .foreground = RGBColor(0x0A, 0x0B, 0x0C),
                            .background = RGBColor(0xF0, 0xF0, 0xF0) });
}

TEST_CASE("ColorPalette.blockStatus.defaultSchemeMatchesTheDocumentedColors", "[ColorPalette]")
{
    // The commented examples `contour generate config` writes (ConfigDocumentation.hpp's BlockStatusConfig
    // and GutterTextConfig) quote these values, and Config_test.cpp holds them to this palette. Naming the
    // colours here as well means retuning a derivation fails in vtbackend's own suite too.
    auto const palette = ColorPalette {};
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == RGBColor(0x7E, 0x7C, 0x7C));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == RGBColor(0xC6, 0x39, 0x39));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == RGBColor(0x4D, 0x79, 0xFF));
    CHECK(palette.gutterTextColor() == RGBColor(0x7E, 0x7C, 0x7C));
}

TEST_CASE("ColorPalette.stickyHeader.derivesWhenUnset", "[ColorPalette][stickyheader]")
{
    auto const palette = ColorPalette {};
    REQUIRE_FALSE(palette.stickyHeaderBackground.has_value());
    REQUIRE_FALSE(palette.stickyHeaderSeparator.has_value());

    // A band, not the page: lifted off the background toward the text colour, and an edge further still.
    CHECK(palette.stickyHeaderBackgroundColor() != palette.defaultBackground);
    CHECK(palette.stickyHeaderSeparatorColor() != palette.stickyHeaderBackgroundColor());

    // The commented example `contour generate config` writes (ConfigDocumentation.hpp's
    // StickyHeaderColorsConfig) and docs/configuration/colors.md quote these values, and Config_test.cpp
    // holds the former to this palette. Naming them here as well means retuning a lift fails in vtbackend's
    // own suite too.
    CHECK(palette.stickyHeaderBackgroundColor() == 0x2c2928_rgb);
    CHECK(palette.stickyHeaderSeparatorColor() == 0x595757_rgb);
}

TEST_CASE("ColorPalette.stickyHeader.explicitColorsBypassTheDerivation", "[ColorPalette][stickyheader]")
{
    auto palette = ColorPalette {};
    palette.stickyHeaderBackground = 0x010203_rgb;
    CHECK(palette.stickyHeaderBackgroundColor() == 0x010203_rgb);
    // The other stays derived: two independent slots, not one block.
    CHECK(palette.stickyHeaderSeparatorColor() == 0x595757_rgb);
}

TEST_CASE("ColorPalette.stickyHeader.isDistinctInTheBuiltinSchemes", "[ColorPalette][stickyheader]")
{
    for (auto const name: defaultColorPaletteNames())
    {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        INFO("scheme: " << name);

        // The band must read as a band on the page, and the separator as an edge on the band.
        CHECK(distance(palette.stickyHeaderBackgroundColor(), palette.defaultBackground) > 6.0);
        CHECK(distance(palette.stickyHeaderSeparatorColor(), palette.stickyHeaderBackgroundColor()) > 15.0);
    }
}

namespace
{
/// Resolves the built-in scheme @p name on top of a default-constructed palette.
ColorPalette schemeNamed(std::string_view name)
{
    auto palette = ColorPalette {};
    REQUIRE(defaultColorPalettes(name, palette));
    return palette;
}
} // namespace

TEST_CASE("ColorPalette.contourSchemesAreReadable", "[ColorPalette]")
{
    for (auto const name: { "contour-dark"sv, "contour-light"sv })
    {
        INFO("scheme: " << name);
        auto const p = schemeNamed(name);
        CHECK(contrastRatio(p.defaultForeground, p.defaultBackground) >= 4.5);
        for (auto const i: { 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14 })
        {
            INFO("palette index: " << i);
            CHECK(contrastRatio(p.palette[i], p.defaultBackground) >= 4.5);
        }
    }
    CHECK(schemeNamed("contour-dark").defaultBackground == 0x1C1E24_rgb);
    CHECK(schemeNamed("contour-dark").defaultForeground == 0xD9DCE3_rgb);
    CHECK(schemeNamed("contour-light").defaultBackground == 0xFAFAF8_rgb);
    CHECK(schemeNamed("contour-light").palette[4] == 0x1F5FBF_rgb);
    CHECK(schemeNamed("contour-dark").palette[4] == 0x6FA8F5_rgb);
    CHECK(schemeNamed("contour-dark").palette[8] == 0x848DA2_rgb);
    CHECK(schemeNamed("contour-light").palette[15] == 0x3A3F48_rgb);
    // Bright black on dark is allowed at 5.0.
    CHECK(contrastRatio(schemeNamed("contour-dark").palette[8], 0x1C1E24_rgb) >= 5.0);
}

TEST_CASE("ColorPalette.contourSchemes.cursorAndSelection", "[ColorPalette]")
{
    auto const dark = schemeNamed("contour-dark");
    auto const light = schemeNamed("contour-light");
    CHECK(dark.cursor.color == CellRGBColor { 0xE8C26E_rgb });
    CHECK(light.cursor.color == CellRGBColor { 0x1F5FBF_rgb });
    CHECK(dark.selection.background == CellRGBColor { 0x3A4A6B_rgb });
    CHECK(light.selection.background == CellRGBColor { 0xC8D8F5_rgb });
}

TEST_CASE("ColorPalette.contourSchemes.indicatorStatusLinesAreReadable", "[ColorPalette]")
{
    for (auto const name: { "contour-dark"sv, "contour-light"sv })
    {
        INFO("scheme: " << name);
        auto const p = schemeNamed(name);
        // Palette white on dark; on light that white is a mid gray (2.3:1 on black), so the page color.
        auto const idleText = name == "contour-dark"sv ? p.palette[7] : p.defaultBackground;
        CHECK(p.indicatorStatusLineInactive.foreground == idleText);
        CHECK(p.indicatorStatusLineInactive.background == p.palette[0]);
        CHECK(p.indicatorStatusLineInsertMode.foreground == idleText);
        CHECK(p.indicatorStatusLineInsertMode.background == p.palette[0]);
        CHECK(p.indicatorStatusLineNormalMode.foreground == p.defaultBackground);
        CHECK(p.indicatorStatusLineNormalMode.background == p.palette[4]);
        CHECK(p.indicatorStatusLineVisualMode.foreground == p.defaultBackground);
        CHECK(p.indicatorStatusLineVisualMode.background == p.palette[5]);

        for (auto const& pair: { p.indicatorStatusLineInactive,
                                 p.indicatorStatusLineInsertMode,
                                 p.indicatorStatusLineNormalMode,
                                 p.indicatorStatusLineVisualMode })
            CHECK(contrastRatio(pair.foreground, pair.background) >= 4.5);
    }
}

TEST_CASE("ColorPalette.contourSchemes.runningBlockIsBlue", "[ColorPalette]")
{
    // Yellow means search, so a running command is blue, by fallback and by explicit scheme choice.
    CHECK(ColorPalette {}.blockStatusColor(CommandBlockOutcome::Running) == ColorPalette {}.normalColor(4));
    for (auto const name: { "contour-dark"sv, "contour-light"sv })
    {
        auto const p = schemeNamed(name);
        REQUIRE(p.blockStatus[static_cast<size_t>(CommandBlockOutcome::Running)].has_value());
        CHECK(p.blockStatusColor(CommandBlockOutcome::Running) == p.palette[4]);
    }
}

TEST_CASE("ColorPalette.classicIsTodaysPalette", "[ColorPalette]")
{
    // The old palette survives under both names.
    auto const today = ColorPalette {};
    CHECK(schemeNamed("contour-classic").palette == today.palette);
    CHECK(schemeNamed("contour").palette == today.palette);
    CHECK(schemeNamed("contour-classic").defaultBackground == today.defaultBackground);
    CHECK(schemeNamed("contour-classic").indicatorStatusLineNormalMode
          == today.indicatorStatusLineNormalMode);
}

TEST_CASE("ColorPalette.contourSchemes.areListed", "[ColorPalette]")
{
    auto const names = defaultColorPaletteNames();
    for (auto const name: { "contour-dark"sv, "contour-light"sv, "contour-classic"sv })
        CHECK(std::ranges::find(names, name) != names.end());
}
