// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/BDFTestFont.hpp>
#include <text_shaper/FontLocator.hpp>
#include <text_shaper/MockFontLocator.hpp>
#include <text_shaper/OneFileTestLocator.hpp>
#include <text_shaper/OpenShaper.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <vector>

using namespace std;
using namespace text;
using namespace text::test;

namespace
{

constexpr auto Ornament = char32_t { 0x276F };     ///< The prompt ornament from issue #1939.
constexpr auto NoBreakSpace = char32_t { 0x00A0 }; ///< Follows the ornament, and does not break the run.
constexpr auto Snowman = char32_t { 0x2603 };      ///< Stands in for a codepoint no font in the chain has.
constexpr auto Acute = char32_t { 0x0301 };        ///< Combining acute accent.
constexpr auto Replacement = char32_t { 0xFFFD };

/// Describes a test font the way the *config* asks for it.
///
/// A terminal always asks for monospace. Whether the font that turns up actually is monospaced is a
/// separate question, answered by the face itself -- which is precisely the gap issue #1939 falls
/// through, and why BDFFont's spacing is set independently of this.
[[nodiscard]] FontDescription descriptionFor(string familyName, bool strictSpacing = false)
{
    auto description = FontDescription {};
    description.familyName = std::move(familyName);
    description.weight = FontWeight::Normal;
    description.slant = FontSlant::Normal;
    description.spacing = FontSpacing::Mono;
    description.strictSpacing = strictSpacing;
    return description;
}

/// A font source whose bytes are not a font at all, so that FreeType refuses to load it.
class BrokenFont
{
  public:
    [[nodiscard]] FontSource source() noexcept
    {
        return FontMemoryRef { .identifier = "broken", .data = gsl::span<uint8_t> { _bytes } };
    }

  private:
    vector<uint8_t> _bytes { 'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't' };
};

// Spelled out at each construction site, where a bare true/false would say nothing.
constexpr auto Monospaced = true;
constexpr auto Proportional = false;

/// One synthetic font: the family the config asks for, and what the face actually turns out to be.
struct FontSpec
{
    string name;
    bool monospace {};
    vector<BDFGlyph> glyphs;
};

/// A shaper over a chain of synthetic fonts, given in chain order: the primary first, its fallbacks after.
///
/// Owns the font bytes, which FontMemoryRef only borrows, and resets the mock locator's process-global
/// registry on destruction so that cases cannot bleed into one another.
class FallbackEnv
{
  public:
    explicit FallbackEnv(vector<FontSpec> const& specs, bool strictSpacing = false):
        _strictSpacing { strictSpacing }
    {
        _fonts.reserve(specs.size()); // no reallocation: the sources below borrow into these

        auto registry = vector<FontDescriptionAndSource> {};
        for (auto const& spec: specs)
        {
            _fonts.emplace_back(spec.name, spec.monospace, spec.glyphs);
            registry.emplace_back(FontDescriptionAndSource {
                .description = descriptionFor(spec.name, strictSpacing), .source = _fonts.back().source() });
        }

        MockFontLocator::configure(std::move(registry));
    }

    FallbackEnv(FallbackEnv const&) = delete;
    FallbackEnv(FallbackEnv&&) = delete;
    FallbackEnv& operator=(FallbackEnv const&) = delete;
    FallbackEnv& operator=(FallbackEnv&&) = delete;

    ~FallbackEnv() { MockFontLocator::configure({}); }

    [[nodiscard]] OpenShaper& shaper() noexcept { return _shaper; }

    /// Loads the named font and returns its key, failing the test if it will not load.
    [[nodiscard]] FontKey key(string const& name)
    {
        auto const fontKey = _shaper.loadFont(descriptionFor(name, _strictSpacing), BDFFont::Size);
        REQUIRE(fontKey.has_value());
        return *fontKey;
    }

  private:
    bool _strictSpacing;
    vector<BDFFont> _fonts; ///< Declared before _shaper: the shaper borrows these bytes.
    MockFontLocator _locator {};
    OpenShaper _shaper { BDFFont::Dpi, _locator };
};

/// Shapes @p codepoints as a run of consecutive cells: one cluster per codepoint.
[[nodiscard]] ShapeResult shapeCells(OpenShaper& shaper, FontKey font, u32string const& codepoints)
{
    auto clusters = vector<unsigned> {};
    for (auto const i: views::iota(size_t { 0 }, codepoints.size()))
        clusters.emplace_back(static_cast<unsigned>(i));

    auto result = ShapeResult {};
    shaper.shape(font,
                 codepoints,
                 gsl::span<unsigned> { clusters },
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 result);
    return result;
}

/// Shapes @p codepoints as a single cell, so that they form one indivisible cluster.
[[nodiscard]] ShapeResult shapeOneCell(OpenShaper& shaper, FontKey font, u32string const& codepoints)
{
    auto clusters = vector<unsigned>(codepoints.size(), 0U);

    auto result = ShapeResult {};
    shaper.shape(font,
                 codepoints,
                 gsl::span<unsigned> { clusters },
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 result);
    return result;
}

} // namespace

// {{{ font fallback

// {{{ font fallback

TEST_CASE("OpenShaper.fallback.only_uncovered_codepoints_fall_back", "[OpenShaper][fallback]")
{
    // Issue #1939. The primary is a monospaced terminal font holding the letters but not the prompt
    // ornament -- the SF Mono situation. The fallback covers the ornament, the non-breaking space AND the
    // letters, precisely as DejaVu Sans does; that total coverage is what let the old whole-run fallback
    // "succeed" and drag the letters into the wrong font along with the ornament.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { U'B', 8 } } },
        { .name = "fallback",
          .monospace = Proportional,
          .glyphs = { { Ornament, 20 }, { NoBreakSpace, 3 }, { U'A', 7 }, { U'B', 7 } } },
    } };

    auto const primary = env.key("primary");
    auto const fallback = env.key("fallback");
    REQUIRE(primary != fallback);

    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament, NoBreakSpace, U'A', U'B' });
    REQUIRE(result.size() == 4);

    // The ornament and the non-breaking space are absent from the primary, so they fall back...
    CHECK(result[0].glyph.font == fallback);
    CHECK(result[1].glyph.font == fallback);

    // ...but the letters, which the primary renders perfectly well, must stay with the primary.
    // Before the fix, the whole run was re-shaped in the fallback and these came back as the fallback's.
    CHECK(result[2].glyph.font == primary);
    CHECK(result[3].glyph.font == primary);

    for (auto const& gpos: result)
        CHECK(gpos.glyph.index.value != 0);
}

TEST_CASE("OpenShaper.fallback.every_glyph_keeps_the_cluster_it_was_shaped_for", "[OpenShaper][fallback]")
{
    // The renderer draws each glyph on the cell its cluster names (issue #452), so a glyph that came
    // from a fallback font must carry its cell exactly as one from the primary does.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { U'B', 8 } } },
        { .name = "fallback", .monospace = Proportional, .glyphs = { { Ornament, 20 } } },
    } };

    auto const result = shapeCells(env.shaper(), env.key("primary"), u32string { U'A', Ornament, U'B' });

    REQUIRE(result.size() == 3);
    CHECK(result[0].cluster == 0);
    CHECK(result[1].cluster == 1);
    CHECK(result[2].cluster == 2);
}

TEST_CASE("OpenShaper.fallback.an_empty_run_shapes_to_nothing", "[OpenShaper][fallback]")
{
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };

    // No codepoints means no glyphs, and nothing to fall back on.
    CHECK(shapeCells(env.shaper(), env.key("primary"), u32string {}).empty());
}

TEST_CASE("OpenShaper.fallback.covered_run_never_consults_a_fallback", "[OpenShaper][fallback]")
{
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { U'B', 8 } } },
        { .name = "fallback", .monospace = Proportional, .glyphs = { { U'A', 3 }, { U'B', 3 } } },
    } };

    auto const primary = env.key("primary");
    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', U'B' });
    REQUIRE(result.size() == 2);

    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == primary);
        CHECK(gpos.advance.x == 8); // the primary's advance, not the fallback's
    }
}

TEST_CASE("OpenShaper.fallback.missing_span_at_each_boundary", "[OpenShaper][fallback]")
{
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { U'B', 8 } } },
        { .name = "fallback",
          .monospace = Monospaced,
          .glyphs = { { Ornament, 8 }, { U'A', 8 }, { U'B', 8 } } },
    } };

    auto const primary = env.key("primary");
    auto const fallback = env.key("fallback");

    SECTION("at the start")
    {
        auto const result = shapeCells(env.shaper(), primary, u32string { Ornament, U'A', U'B' });
        REQUIRE(result.size() == 3);
        CHECK(result[0].glyph.font == fallback);
        CHECK(result[1].glyph.font == primary);
        CHECK(result[2].glyph.font == primary);
    }

    SECTION("in the middle")
    {
        auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Ornament, U'B' });
        REQUIRE(result.size() == 3);
        CHECK(result[0].glyph.font == primary);
        CHECK(result[1].glyph.font == fallback);
        CHECK(result[2].glyph.font == primary);
    }

    SECTION("at the end")
    {
        auto const result = shapeCells(env.shaper(), primary, u32string { U'A', U'B', Ornament });
        REQUIRE(result.size() == 3);
        CHECK(result[0].glyph.font == primary);
        CHECK(result[1].glyph.font == primary);
        CHECK(result[2].glyph.font == fallback);
    }

    SECTION("two disjoint spans")
    {
        auto const result = shapeCells(env.shaper(), primary, u32string { Ornament, U'A', Ornament, U'B' });
        REQUIRE(result.size() == 4);
        CHECK(result[0].glyph.font == fallback);
        CHECK(result[1].glyph.font == primary);
        CHECK(result[2].glyph.font == fallback);
        CHECK(result[3].glyph.font == primary);
    }
}

TEST_CASE("OpenShaper.fallback.walks_further_down_the_chain", "[OpenShaper][fallback]")
{
    // No single fallback covers both missing codepoints, so the walk has to reach past the first one.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
        { .name = "first", .monospace = Monospaced, .glyphs = { { Ornament, 8 } } },
        { .name = "second", .monospace = Monospaced, .glyphs = { { Snowman, 8 } } },
    } };

    auto const primary = env.key("primary");
    auto const first = env.key("first");
    auto const second = env.key("second");

    // The ornament and the snowman are separated by 'A', so they form two independent spans, each
    // answered by a different font.
    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament, U'A', Snowman });
    REQUIRE(result.size() == 3);

    CHECK(result[0].glyph.font == first);
    CHECK(result[1].glyph.font == primary);
    CHECK(result[2].glyph.font == second);

    for (auto const& gpos: result)
        CHECK(gpos.glyph.index.value != 0);
}

TEST_CASE("OpenShaper.fallback.unresolvable_glyph_gets_the_primary_replacement", "[OpenShaper][fallback]")
{
    // Nothing in the chain has the snowman. It must come back as the *primary* face's replacement
    // character, carrying the primary's font key -- so that glyph index and face agree.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "fallback", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };

    auto const primary = env.key("primary");
    auto const replacement = env.shaper().shape(primary, Replacement);
    REQUIRE(replacement.has_value());

    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);

    CHECK(result[0].glyph.font == primary);
    CHECK(result[1].glyph.font == primary);
    CHECK(result[1].glyph.index.value == replacement->glyph.index.value);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("OpenShaper.fallback.an_unresolvable_glyph_does_not_poison_the_rest_of_the_run",
          "[OpenShaper][fallback]")
{
    // The snowman is in no font at all. Before the fix, its unresolved glyph made every *following*
    // cluster look as though it had failed too: each one walked the entire fallback chain and came back
    // shaped by the last font tried, even though the primary rendered it perfectly well.
    auto env = FallbackEnv { {
        { .name = "primary",
          .monospace = Monospaced,
          .glyphs = { { U'A', 8 }, { U'B', 8 }, { Replacement, 8 } } },
        { .name = "fallback", .monospace = Monospaced, .glyphs = { { U'A', 3 }, { U'B', 3 } } },
    } };

    auto const primary = env.key("primary");
    auto const result = shapeCells(env.shaper(), primary, u32string { Snowman, U'A', U'B' });
    REQUIRE(result.size() == 3);

    CHECK(result[1].glyph.font == primary);
    CHECK(result[2].glyph.font == primary);
    CHECK(result[1].advance.x == 8); // the primary's advance, so genuinely the primary's glyph
    CHECK(result[2].advance.x == 8);
}

TEST_CASE("OpenShaper.fallback.a_combining_mark_travels_with_its_base", "[OpenShaper][fallback]")
{
    // One cell holding a base plus a combining accent. The primary has the base but not the accent, so
    // the whole cluster -- base included -- must move to the fallback. Splitting it would shape the base
    // in one font and its mark in another, and the mark would no longer sit over the glyph it belongs to.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
        { .name = "fallback", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Acute, 0 } } },
    } };

    auto const primary = env.key("primary");
    auto const fallback = env.key("fallback");

    auto const result = shapeOneCell(env.shaper(), primary, u32string { U'A', Acute });
    REQUIRE(result.size() == 2);

    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == fallback);
        CHECK(gpos.glyph.index.value != 0);
    }
}

TEST_CASE("OpenShaper.fallback.respects_the_fallback_limit", "[OpenShaper][fallback]")
{
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "first", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
        { .name = "second", .monospace = Monospaced, .glyphs = { { Ornament, 8 } } },
    } };

    SECTION("a limit of zero disables fallback entirely")
    {
        env.shaper().setFontFallbackLimit(0); // must precede the first loadFont

        auto const primary = env.key("primary");
        auto const replacement = env.shaper().shape(primary, Replacement);
        REQUIRE(replacement.has_value());

        auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
        REQUIRE(result.size() == 1);
        CHECK(result[0].glyph.font == primary);
        CHECK(result[0].glyph.index.value == replacement->glyph.index.value);
    }

    SECTION("an unlimited chain reaches the last font")
    {
        env.shaper().setFontFallbackLimit(-1);

        auto const primary = env.key("primary");
        auto const second = env.key("second");

        auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
        REQUIRE(result.size() == 1);
        CHECK(result[0].glyph.font == second);
    }
}

TEST_CASE("OpenShaper.fallback.extends_the_chain_on_demand", "[OpenShaper][fallback]")
{
    // Only the first batch of fallbacks is loaded up front; the rest arrive as the walk runs off the end
    // of the list. Park the one font that can help well beyond that first batch.
    auto specs = vector<FontSpec> {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    };
    for (auto const i: views::iota(1, 11))
        specs.emplace_back(
            FontSpec { .name = format("filler{}", i), .monospace = Monospaced, .glyphs = { { U'A', 8 } } });
    specs.emplace_back(
        FontSpec { .name = "rescuer", .monospace = Monospaced, .glyphs = { { Ornament, 8 } } });

    auto env = FallbackEnv { specs };

    auto const primary = env.key("primary");
    auto const rescuer = env.key("rescuer");

    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == rescuer);
    CHECK(result[0].glyph.index.value != 0);
}

TEST_CASE("OpenShaper.fallback.skips_a_font_that_will_not_load", "[OpenShaper][fallback]")
{
    // Hand-rolled rather than built on FallbackEnv, because the broken entry is deliberately not a font.
    auto const _ = core::Finally { [] { MockFontLocator::configure({}); } };

    auto primary = BDFFont { "primary", Monospaced, { { U'A', 8 } } };
    auto broken = BrokenFont {};
    auto rescuer = BDFFont { "rescuer", Monospaced, { { Ornament, 8 } } };

    MockFontLocator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("broken"), .source = broken.source() },
        { .description = descriptionFor("rescuer"), .source = rescuer.source() },
    });

    auto locator = MockFontLocator {};
    auto shaper = OpenShaper { BDFFont::Dpi, locator };

    auto const primaryKey = shaper.loadFont(descriptionFor("primary"), BDFFont::Size);
    auto const rescuerKey = shaper.loadFont(descriptionFor("rescuer"), BDFFont::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(rescuerKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == *rescuerKey);
}

TEST_CASE("OpenShaper.fallback.non_monotone_clusters_fall_back_on_the_whole_run", "[OpenShaper][fallback]")
{
    // Splitting a run into fallback spans rests on cluster values being monotone. Should they ever not
    // be -- a caller breaking the contract, or a shaper configuration that reorders -- there is no
    // trustworthy mapping from glyphs back to codepoints, so the run must not be split. It is offered to
    // the fallback whole instead, which is what every run used to get, rather than being mis-split.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { U'B', 8 } } },
        { .name = "fallback",
          .monospace = Monospaced,
          .glyphs = { { Ornament, 8 }, { U'A', 8 }, { U'B', 8 } } },
    } };

    auto const primary = env.key("primary");
    auto const fallback = env.key("fallback");

    auto const codepoints = u32string { Ornament, U'A', U'B' };
    auto clusters = vector<unsigned> { 2, 0, 1 }; // deliberately out of order

    auto result = ShapeResult {};
    env.shaper().shape(primary,
                       codepoints,
                       gsl::span<unsigned> { clusters },
                       unicode::Script::Latin,
                       unicode::PresentationStyle::Text,
                       result);

    REQUIRE(result.size() == 3);

    // The whole run went to the fallback, and every glyph still resolved. The point is that this
    // terminates and renders, not that it splits well.
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == fallback);
        CHECK(gpos.glyph.index.value != 0);
    }
}

// }}}

// {{{ fallback spacing preference

TEST_CASE("OpenShaper.fallback.prefers_a_monospaced_fallback", "[OpenShaper][fallback][spacing]")
{
    // Both fallbacks cover the ornament, but only the second is monospaced. A terminal wants that one,
    // even though it comes later in the chain: a proportional face's advances do not line up with the
    // cell grid.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
        { .name = "proportional", .monospace = Proportional, .glyphs = { { Ornament, 3 } } },
        { .name = "monospaced", .monospace = Monospaced, .glyphs = { { Ornament, 8 } } },
    } };

    auto const primary = env.key("primary");
    auto const monospaced = env.key("monospaced");

    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == monospaced);
    CHECK(result[0].advance.x == 8);
}

TEST_CASE("OpenShaper.fallback.accepts_a_proportional_fallback_rather_than_render_nothing",
          "[OpenShaper][fallback][spacing]")
{
    // Preferring monospace must never harden into refusing everything else: emoji, CJK and symbol
    // fallbacks are frequently proportional, and a glyph that renders today must not become a box.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "proportional", .monospace = Proportional, .glyphs = { { Ornament, 3 } } },
    } };

    auto const primary = env.key("primary");
    auto const proportional = env.key("proportional");

    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == proportional);
    CHECK(result[0].glyph.index.value != 0);
}

TEST_CASE("OpenShaper.fallback.strict_spacing_refuses_a_proportional_fallback",
          "[OpenShaper][fallback][spacing]")
{
    // strict_spacing is the opt-in hard filter: the user has said a glyph from a font that does not fit
    // the grid is worse than none. This is the MatchDescription policy, which the fontconfig locator and the
    // mock keep; the native Windows and macOS locators permit any face for the last-resort lookup.
    auto env = FallbackEnv {
        {
            { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
            { .name = "proportional", .monospace = Proportional, .glyphs = { { Ornament, 3 } } },
        },
        /* strictSpacing */ true
    };

    auto const primary = env.key("primary");
    auto const replacement = env.shaper().shape(primary, Replacement);
    REQUIRE(replacement.has_value());

    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == primary);
    CHECK(result[0].glyph.index.value == replacement->glyph.index.value);
}

// }}}

TEST_CASE("OpenShaper.COLRv1", "[OpenShaper]")
{
    auto const fontPath = filesystem::path("/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf");
    if (!filesystem::exists(fontPath))
    {
        WARN("Test skipped. Font file not found: " << fontPath << "\n");
        return;
    }

    auto testLocator = OneFileLocator { fontPath.string() };
    auto shaper = OpenShaper(DPI { .x = 96, .y = 96 }, testLocator);

    auto const fd = FontDescription { .familyName = "Noto Color Emoji" };
    auto const fontSize = text::FontSize { 12.0 };

    // Explicitly testing loadFont which uses locate()
    auto const fontKeyOpt = shaper.loadFont(fd, fontSize);
    REQUIRE(fontKeyOpt.has_value());

    auto const fontKey = *fontKeyOpt;
    auto const codepoint = char32_t { 0x1F600 }; // Grinning Face

    auto const glyphPosOpt = shaper.shape(fontKey, codepoint);
    REQUIRE(glyphPosOpt.has_value());

    auto const& glyphPos = *glyphPosOpt;
    auto const rasterizedGlyphOpt = shaper.rasterize(glyphPos.glyph, RenderMode::Color);
    REQUIRE(rasterizedGlyphOpt.has_value());

    auto const& glyph = *rasterizedGlyphOpt;

    // COLRv1 with Cairo rendering should produce an RGBA bitmap
    CHECK(glyph.format == text::BitmapFormat::RGBA);
    CHECK(glyph.bitmapSize.width > vtbackend::Width(0));
    CHECK(glyph.bitmapSize.height > vtbackend::Height(0));
    CHECK(!glyph.bitmap.empty());
}

TEST_CASE("OpenShaper.color_font", "[OpenShaper]")
{
    auto const fontPath = filesystem::path("C:/Windows/Fonts/seguiemj.ttf");
    if (!filesystem::exists(fontPath))
    {
        WARN("Test skipped. Font file not found: " << fontPath << "\n");
        return;
    }

    auto locator = OneFileLocator { fontPath.string() };
    auto shaper = OpenShaper(DPI { .x = 96, .y = 96 }, locator);
    auto const fontKey =
        shaper.loadFont(FontDescription { .familyName = "Segoe UI Emoji" }, FontSize { 12.0 });
    REQUIRE(fontKey.has_value());

    auto const rasterize = [&](char32_t codepoint) {
        auto const position = shaper.shape(*fontKey, codepoint);
        REQUIRE(position.has_value());
        auto glyph = shaper.rasterize(position->glyph, RenderMode::Gray);
        REQUIRE(glyph.has_value());
        return std::move(*glyph);
    };

    SECTION("a glyph without colors of its own is drawn like any other glyph")
    {
        // Issue #452: Segoe UI Emoji is a COLR v1 font, but its U+21B3 is a plain outline with no color
        // definition at all. Sent down the color path anyway, it was painted in Cairo's default source
        // color, opaque black, into a color bitmap -- which is never tinted, so it drew as a black arrow
        // whatever the text's color was. A mask takes the text's color.
        auto const glyph = rasterize(0x21B3);
        CHECK(glyph.format == BitmapFormat::AlphaMask);
        CHECK(std::ranges::any_of(glyph.bitmap, [](uint8_t coverage) { return coverage != 0; }));
    }

    SECTION("a glyph with colors of its own stays a color bitmap")
    {
        CHECK(rasterize(0x23F1).format == BitmapFormat::RGBA); // STOPWATCH
    }
}

TEST_CASE("OpenShaper.coverage.resolves_a_codepoint_the_chain_cannot", "[OpenShaper][fallback]")
{
    // The fallback chain is ordered by how well each font matches the DESCRIPTION, so the only face
    // holding a script can sort far past any length worth walking eagerly -- on a stock Fedora install
    // the first CJK face is 83rd of 201, against a chain limit of 16. Once the chain is exhausted the
    // locator is asked which font actually covers the codepoint, and that answer is used.
    auto coverageFont = BDFFont { "coverage", Monospaced, { { Snowman, 8 } } };

    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "fallback", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };
    MockFontLocator::configureCoverage({ coverageFont.source() });

    auto const primary = env.key("primary");
    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);

    CHECK(result[0].glyph.font == primary);
    // The snowman came from neither the primary nor its chain, and it is a real glyph, not .notdef.
    CHECK(result[1].glyph.font != primary);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("OpenShaper.coverage.cache_does_not_outlive_the_keys_it_stores", "[OpenShaper][fallback]")
{
    // The coverage cache answers with a FontKey, and those keys are owned by the maps clearCache()
    // empties. Surviving that call it would hand back a key nothing can resolve -- and the shaping
    // path requires the lookup to succeed, so the next frame drawing that codepoint aborted. Any
    // non-DPI font change reaches this: applyFontDescriptions() calls clearCache().
    auto coverageFont = BDFFont { "coverage", Monospaced, { { Snowman, 8 } } };

    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    MockFontLocator::configureCoverage({ coverageFont.source() });

    // Populate the coverage cache.
    REQUIRE(shapeCells(env.shaper(), env.key("primary"), u32string { Snowman }).size() == 1);

    env.shaper().clearCache();

    // Re-loading gives a fresh key; shaping the same codepoint must not reach for the stale one.
    auto const primary = env.key("primary");
    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("OpenShaper.coverage.is_spent_once_per_span", "[OpenShaper][fallback]")
{
    // The coverage lookup answers with a font chosen BECAUSE it covers the codepoints. If shaping
    // against that font still comes back .notdef, asking again would return the same font forever --
    // so the attempt is spent once and the span then settles for the replacement glyph. This case
    // hands it a font that does NOT have the snowman, which is what makes the guard observable: with
    // the attempt unbounded it does not terminate.
    auto uselessFont = BDFFont { "useless", Monospaced, { { U'B', 8 } } };

    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    MockFontLocator::configureCoverage({ uselessFont.source() });

    auto const primary = env.key("primary");
    auto const replacement = env.shaper().shape(primary, Replacement);
    REQUIRE(replacement.has_value());

    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);
    CHECK(result[1].glyph.index.value == replacement->glyph.index.value);
}

TEST_CASE("OpenShaper.resizeFont.reports_not_resized_by_returning_the_key_it_was_given",
          "[OpenShaper][resize]")
{
    // The synthetic fonts here are BDF, which ships fixed strikes and cannot be opened at an
    // arbitrary size -- the same position a user's bitmap font is in when `OSC 66` asks for one. The
    // contract that has to hold then is that resizeFont() says so by handing back the key it was
    // given, which TextRenderer::rasterizeAtBlockSize() reads as "not resized" and answers by
    // magnifying the raster it already has rather than rasterizing at the wrong size.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };
    auto const primary = env.key("primary");

    CHECK(env.shaper().resizeFont(primary, FontSize { BDFFont::Size.pt * 2 }) == primary);

    // The size it is already at is not a resize at all, and takes the same answer.
    CHECK(env.shaper().resizeFont(primary, BDFFont::Size) == primary);
}

TEST_CASE("OpenShaper.resizeFont.a_size_the_face_lacks_does_not_retire_the_font", "[OpenShaper][resize]")
{
    // A source is blacklisted to mean "this file is not a font I can use", and nothing clears that
    // list -- not even clearCache(). Concluding it from a failure to open a size would let one
    // `OSC 66` scaled write retire a bitmap font for the rest of the session, including at the size
    // it was rendering perfectly well at a moment earlier.
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };
    auto const primary = env.key("primary");

    for (auto const i: std::views::iota(1, 8))
        (void) env.shaper().resizeFont(primary, FontSize { BDFFont::Size.pt + double(i) });

    // Still shapes, and still resolves to a real glyph rather than .notdef.
    auto const result = shapeOneCell(env.shaper(), primary, u32string { U'A' });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.index.value != 0);

    // And the font still loads at its own size, which the blacklist would have prevented.
    CHECK(env.key("primary") == primary);
}

TEST_CASE("OpenShaper.coverage.respects_disabled_fallback", "[OpenShaper][fallback]")
{
    auto coverage = BDFFont { "coverage", Monospaced, { { Snowman, 8 } } };
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    MockFontLocator::configureCoverage({ coverage.source() });
    auto description = descriptionFor("primary");
    SECTION("none policy")
    {
        description.fontFallback = FontFallbackNone {};
    }
    SECTION("zero limit")
    {
        env.shaper().setFontFallbackLimit(0);
    }
    SECTION("zero limit after successful coverage lookup")
    {
        auto const enabled = env.key("primary");
        REQUIRE(shapeCells(env.shaper(), enabled, u32string { Snowman }).front().glyph.font != enabled);
        env.shaper().setFontFallbackLimit(0);
    }
    auto const primary = env.shaper().loadFont(description, BDFFont::Size);
    REQUIRE(primary);
    auto const replacement = env.shaper().shape(*primary, Replacement);
    REQUIRE(replacement);
    auto const result = shapeCells(env.shaper(), *primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);
    CHECK(result[0].glyph.font == *primary);
    CHECK(result[1].glyph.font == *primary);
    CHECK(result[1].glyph.index.value == replacement->glyph.index.value);
}

TEST_CASE("OpenShaper.coverage.respects_spacing", "[OpenShaper][fallback]")
{
    auto proportional = BDFFont { "proportional", Proportional, { { Snowman, 3 } } };
    auto monospaced = BDFFont { "monospaced", Monospaced, { { Snowman, 8 } } };
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    auto description = descriptionFor("primary");
    description.strictSpacing = true;
    MockFontLocator::configureCoverage({ proportional.source() });
    auto expected = FontSpacing::Proportional;
    SECTION("reject proportional coverage")
    {
    }
    SECTION("continue past incompatible coverage")
    {
        MockFontLocator::configureCoverage({ proportional.source(), monospaced.source() });
        expected = FontSpacing::Mono;
    }
    auto const primary = env.shaper().loadFont(description, BDFFont::Size);
    REQUIRE(primary);
    auto const result = shapeCells(env.shaper(), *primary, u32string { Snowman });
    REQUIRE(result.size() == 1);
    if (expected == FontSpacing::Mono)
    {
        CHECK(result[0].glyph.font != *primary);
        CHECK(result[0].glyph.index.value != 0);
    }
    else
    {
        auto const replacement = env.shaper().shape(*primary, Replacement);
        REQUIRE(replacement);
        CHECK(result[0].glyph.font == *primary);
        CHECK(result[0].glyph.index.value == replacement->glyph.index.value);
    }
}

TEST_CASE("OpenShaper.coverage.cache_isolated_by_spacing_policy", "[OpenShaper][fallback]")
{
    auto coverage = BDFFont { "coverage", Proportional, { { Snowman, 3 } } };
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    MockFontLocator::configureCoverage({ coverage.source() });
    auto description = descriptionFor("primary");
    SECTION("permissive before strict")
    {
    }
    SECTION("strict before permissive")
    {
        description.strictSpacing = true;
    }
    // Reusing one primary face/key must not let a permissive cached answer leak into a strict
    // description, or a strict negative answer suppress later permissive recovery.
    for (auto const iteration: std::views::iota(0, 3))
    {
        auto const primary = env.shaper().loadFont(description, BDFFont::Size);
        REQUIRE(primary);
        auto const result = shapeCells(env.shaper(), *primary, u32string { Snowman });
        REQUIRE(result.size() == 1);
        CAPTURE(iteration, description.strictSpacing);
        CHECK((result[0].glyph.font == *primary) == description.strictSpacing);
        description.strictSpacing = !description.strictSpacing;
    }
}

TEST_CASE("OpenShaper.fallback.zero_limit_stops_an_already_loaded_chain", "[OpenShaper][fallback]")
{
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "fallback", .monospace = Monospaced, .glyphs = { { Snowman, 8 } } },
    } };
    auto const primary = env.key("primary");
    auto const enabled = shapeCells(env.shaper(), primary, u32string { Snowman });
    REQUIRE(enabled.size() == 1);
    CHECK(enabled.front().glyph.font != primary);
    env.shaper().setFontFallbackLimit(0);
    auto const disabled = shapeCells(env.shaper(), primary, u32string { Snowman });
    REQUIRE(disabled.size() == 1);
    CHECK(disabled.front().glyph.font == primary);
    env.shaper().setFontFallbackLimit(16);
    auto const restored = shapeCells(env.shaper(), primary, u32string { Snowman });
    REQUIRE(restored.size() == 1);
    CHECK(restored.front().glyph.font == enabled.front().glyph.font);
}

// {{{ emoji sequences a font cannot combine (#2381)

namespace
{

constexpr auto Person = char32_t { 0x1F9D1 };
constexpr auto MediumSkin = char32_t { 0x1F3FD };
constexpr auto LightSkin = char32_t { 0x1F3FB };
constexpr auto MediumLightSkin = char32_t { 0x1F3FC };
constexpr auto Zwj = char32_t { 0x200D };
constexpr auto Rabbit = char32_t { 0x1F430 };
constexpr auto FightCloud = char32_t { 0x1FAEF };
constexpr auto Grinning = char32_t { 0x1F600 };

/// @return People with bunny ears: medium skin tone, light skin tone (Emoji 17.0).
[[nodiscard]] u32string bunnyEars()
{
    return { Person, MediumSkin, Zwj, Rabbit, Zwj, Person, LightSkin };
}

/// @return People wrestling: medium-light skin tone, light skin tone (Emoji 17.0), U+1FAEF itself new
/// in 17.0.
[[nodiscard]] u32string wrestling()
{
    return { Person, MediumLightSkin, Zwj, FightCloud, Zwj, Person, LightSkin };
}

/// The pieces of bunnyEars() and a grinning face, each a glyph of its own -- a BDF font has no ligatures, so
/// this is exactly a system emoji font that predates the sequence.
[[nodiscard]] vector<BDFGlyph> emojiPieces()
{
    return {
        { Person, 16 }, { MediumSkin, 16 }, { LightSkin, 16 }, { MediumLightSkin, 16 },
        { Zwj, 0 },     { Rabbit, 16 },     { Grinning, 16 },  { Replacement, 16 },
    };
}

/// Shapes @p codepoints as an emoji run with the given per-codepoint clusters.
[[nodiscard]] ShapeResult shapeEmoji(OpenShaper& shaper,
                                     FontKey font,
                                     u32string const& codepoints,
                                     vector<unsigned> clusters)
{
    auto result = ShapeResult {};
    shaper.shape(font,
                 codepoints,
                 gsl::span<unsigned> { clusters },
                 unicode::Script::Common,
                 unicode::PresentationStyle::Emoji,
                 result);
    return result;
}

/// Shapes @p codepoints as an emoji run in a single cell.
[[nodiscard]] ShapeResult shapeOneCellEmoji(OpenShaper& shaper, FontKey font, u32string const& codepoints)
{
    return shapeEmoji(shaper, font, codepoints, vector<unsigned>(codepoints.size(), 0U));
}

[[nodiscard]] size_t advancingGlyphs(ShapeResult const& result)
{
    return static_cast<size_t>(
        ranges::count_if(result, [](GlyphPosition const& gpos) { return gpos.advance.x != 0; }));
}

#ifdef CONTOUR_EMOJI_FONT_FILE
/// The bundled Noto Color Emoji, which combines the Emoji 17.0 sequences into one glyph each.
[[nodiscard]] FontSource bundledEmojiFont()
{
    return FontPath { .value = CONTOUR_EMOJI_FONT_FILE };
}
#endif

} // namespace

TEST_CASE("OpenShaper.emoji.fragmented_cluster_resolves_to_the_combining_font", "[OpenShaper][emoji]")
{
#ifndef CONTOUR_EMOJI_FONT_FILE
    SKIP("built with CONTOUR_EMOJI_FONT=OFF: no font that combines the sequence");
#else
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };
    MockFontLocator::configureCoverage({ bundledEmojiFont() });

    auto const primary = env.key("system-emoji");
    auto const result = shapeOneCellEmoji(env.shaper(), primary, bunnyEars());

    REQUIRE(advancingGlyphs(result) == 1);
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font != primary);
        CHECK(gpos.glyph.index.value != 0);
    }
#endif
}

TEST_CASE("OpenShaper.emoji.keeps_the_first_fonts_pieces", "[OpenShaper][emoji]")
{
    // No font combines the sequence: the system font's pieces stand, not those of whichever font was
    // tried last, and none of them turns into a replacement glyph. Must terminate.
    auto otherPieces = BDFFont { "other-emoji", Monospaced, emojiPieces() };
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };
    MockFontLocator::configureCoverage({ otherPieces.source() });

    auto const primary = env.key("system-emoji");
    auto const result = shapeOneCellEmoji(env.shaper(), primary, bunnyEars());

    // Five pieces: two people, their two skin tones and the rabbit. (Shaping puts a cluster's whole
    // advance on its first glyph, so the pieces are counted, not their advances.)
    CHECK(result.size() == 5);
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == primary);
        CHECK(gpos.glyph.index.value != 0);
        CHECK(gpos.integrity == ClusterIntegrity::Pieces); // so the renderer fits them into the cell
    }
}

TEST_CASE("OpenShaper.emoji.notdef_member_reaches_the_combining_font", "[OpenShaper][emoji]")
{
#ifndef CONTOUR_EMOJI_FONT_FILE
    SKIP("built with CONTOUR_EMOJI_FONT=OFF: no font that combines the sequence");
#else
    // The system font has no U+1FAEF at all, and the system's first coverage answer has only part of the
    // sequence: it must be passed over, because it does not resolve the cluster, for the bundled font.
    auto partial = BDFFont { "partial", Monospaced, { { Person, 16 } } };
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };
    MockFontLocator::configureCoverage({ partial.source(), bundledEmojiFont() });

    auto const primary = env.key("system-emoji");
    auto const result = shapeOneCellEmoji(env.shaper(), primary, wrestling());

    REQUIRE(advancingGlyphs(result) == 1);
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font != primary);
        CHECK(gpos.glyph.index.value != 0);
    }
#endif
}

TEST_CASE("OpenShaper.emoji.only_the_fragmented_cluster_falls_back", "[OpenShaper][emoji]")
{
#ifndef CONTOUR_EMOJI_FONT_FILE
    SKIP("built with CONTOUR_EMOJI_FONT=OFF: no font that combines the sequence");
#else
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };
    MockFontLocator::configureCoverage({ bundledEmojiFont() });

    auto const primary = env.key("system-emoji");
    auto codepoints = bunnyEars();
    codepoints += Grinning;
    auto clusters = vector<unsigned>(bunnyEars().size(), 0U);
    clusters.emplace_back(2U);
    auto const result = shapeEmoji(env.shaper(), primary, codepoints, clusters);

    REQUIRE(advancingGlyphs(result) == 2);
    auto const grinning =
        ranges::find_if(result, [](GlyphPosition const& gpos) { return gpos.cluster == 2; });
    REQUIRE(grinning != result.end());
    CHECK(grinning->glyph.font == primary);
    for (auto const& gpos: result)
        if (gpos.cluster == 0)
            CHECK(gpos.glyph.font != primary);
#endif
}

TEST_CASE("OpenShaper.emoji.a_neighbour_no_font_combines_does_not_hold_back_a_sequence",
          "[OpenShaper][emoji]")
{
#ifndef CONTOUR_EMOJI_FONT_FILE
    SKIP("built with CONTOUR_EMOJI_FONT=OFF: no font that combines the sequence");
#else
    // Two fragmented clusters side by side: one the bundled font combines, and a ZWJ chain no font defines.
    // Each is its own question; the second's having no answer must not deny the first its font.
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };
    MockFontLocator::configureCoverage({ bundledEmojiFont() });

    auto const primary = env.key("system-emoji");
    auto const nonRgi = u32string { Rabbit, Zwj, Grinning };
    auto clusters = vector<unsigned>(bunnyEars().size(), 0U);
    clusters.insert(clusters.end(), nonRgi.size(), 2U);
    auto const result = shapeEmoji(env.shaper(), primary, bunnyEars() + nonRgi, clusters);

    auto const ofCluster = [&](unsigned cluster) {
        auto glyphs = vector<GlyphPosition> {};
        ranges::copy_if(result, back_inserter(glyphs), [cluster](GlyphPosition const& gpos) {
            return gpos.cluster == cluster;
        });
        return glyphs;
    };
    REQUIRE(ofCluster(0).size() == 1); // combined, by the bundled font
    CHECK(ofCluster(0).front().glyph.font != primary);
    for (auto const& gpos: ofCluster(2)) // still in pieces, the system font's
        CHECK(gpos.glyph.font == primary);
#endif
}

TEST_CASE("OpenShaper.emoji.a_missing_neighbour_does_not_turn_pieces_into_replacements",
          "[OpenShaper][emoji]")
{
    // A fragmented sequence beside an emoji no font has at all. The sequence's pieces are the best anyone
    // can draw; they must not become replacement glyphs because the neighbour is missing.
    auto otherPieces = BDFFont { "other-emoji", Monospaced, emojiPieces() };
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() },
                        { .name = "chain-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };
    MockFontLocator::configureCoverage({ otherPieces.source() });

    auto const primary = env.key("system-emoji");
    auto clusters = vector<unsigned>(bunnyEars().size(), 0U);
    clusters.emplace_back(2U);
    auto const result = shapeEmoji(env.shaper(), primary, bunnyEars() + u32string { FightCloud }, clusters);

    for (auto const& gpos:
         result | views::filter([](GlyphPosition const& gpos) { return gpos.cluster == 0; }))
    {
        CHECK(gpos.glyph.font == primary);
        CHECK(gpos.glyph.index.value != 0);
    }
}

TEST_CASE("OpenShaper.emoji.the_first_fonts_pieces_win_over_a_later_fonts", "[OpenShaper][emoji]")
{
    // The chain holds a second font that also draws the sequence only in pieces. Its pieces are tried
    // and found no better: the system font's stand.
    auto env =
        FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = emojiPieces() },
                        { .name = "chain-emoji", .monospace = Monospaced, .glyphs = emojiPieces() } } };

    auto const primary = env.key("system-emoji");
    auto const result = shapeOneCellEmoji(env.shaper(), primary, bunnyEars());

    REQUIRE(result.size() == 5);
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == primary);
        CHECK(gpos.integrity == ClusterIntegrity::Pieces);
    }
}

TEST_CASE("OpenShaper.emoji.a_sequence_the_font_composes_from_several_glyphs_is_whole", "[OpenShaper][emoji]")
{
    // Segoe UI Emoji composes a family from several narrower glyphs overlapped by the font's offsets;
    // together they advance about one emoji. That is the font drawing the sequence, not pieces of it: no
    // fallback, and nothing for the renderer to pull apart. (The BDF font is 12 px to the em; 8 + 6 px is
    // about 1.2 em.)
    auto composing = vector<BDFGlyph> { { Person, 8 }, { Zwj, 0 }, { Rabbit, 6 } };
    auto other = BDFFont { "other-emoji", Monospaced, composing };
    auto env = FallbackEnv { { { .name = "system-emoji", .monospace = Monospaced, .glyphs = composing } } };
    MockFontLocator::configureCoverage({ other.source() });

    auto const primary = env.key("system-emoji");
    auto const result = shapeOneCellEmoji(env.shaper(), primary, u32string { Person, Zwj, Rabbit });

    REQUIRE(result.size() >= 2);
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == primary);
        CHECK(gpos.integrity == ClusterIntegrity::Whole);
    }
}

TEST_CASE("OpenShaper.emoji.text_runs_are_never_fragmented", "[OpenShaper][emoji]")
{
    // A base and its combining mark legitimately shape to two advancing glyphs in one cluster.
    auto coverage = BDFFont { "coverage", Monospaced, { { U'e', 8 }, { Acute, 8 } } };
    auto env = FallbackEnv { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'e', 8 }, { Acute, 8 } } },
    } };
    MockFontLocator::configureCoverage({ coverage.source() });

    auto const primary = env.key("primary");
    auto const result = shapeOneCell(env.shaper(), primary, u32string { U'e', Acute });

    REQUIRE(result.size() == 2);
    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == primary);
        CHECK(gpos.integrity == ClusterIntegrity::Whole); // a base and its mark are not pieces
    }
}

// }}}
