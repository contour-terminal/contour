// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/bdf_test_font.h>
#include <text_shaper/font_locator.h>
#include <text_shaper/mock_font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

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
/// through, and why bdf_font's spacing is set independently of this.
[[nodiscard]] font_description descriptionFor(string familyName, bool strictSpacing = false)
{
    auto description = font_description {};
    description.familyName = std::move(familyName);
    description.weight = font_weight::normal;
    description.slant = font_slant::normal;
    description.spacing = font_spacing::mono;
    description.strictSpacing = strictSpacing;
    return description;
}

/// A font source whose bytes are not a font at all, so that FreeType refuses to load it.
class broken_font
{
  public:
    [[nodiscard]] font_source source() noexcept
    {
        return font_memory_ref { .identifier = "broken", .data = gsl::span<uint8_t> { _bytes } };
    }

  private:
    vector<uint8_t> _bytes { 'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't' };
};

// Spelled out at each construction site, where a bare true/false would say nothing.
constexpr auto Monospaced = true;
constexpr auto Proportional = false;

/// One synthetic font: the family the config asks for, and what the face actually turns out to be.
struct font_spec
{
    string name;
    bool monospace {};
    vector<bdf_glyph> glyphs;
};

/// A shaper over a chain of synthetic fonts, given in chain order: the primary first, its fallbacks after.
///
/// Owns the font bytes, which font_memory_ref only borrows, and resets the mock locator's process-global
/// registry on destruction so that cases cannot bleed into one another.
class fallback_env
{
  public:
    explicit fallback_env(vector<font_spec> const& specs, bool strictSpacing = false):
        _strictSpacing { strictSpacing }
    {
        _fonts.reserve(specs.size()); // no reallocation: the sources below borrow into these

        auto registry = vector<font_description_and_source> {};
        for (auto const& spec: specs)
        {
            _fonts.emplace_back(spec.name, spec.monospace, spec.glyphs);
            registry.emplace_back(font_description_and_source {
                .description = descriptionFor(spec.name, strictSpacing), .source = _fonts.back().source() });
        }

        mock_font_locator::configure(std::move(registry));
    }

    fallback_env(fallback_env const&) = delete;
    fallback_env(fallback_env&&) = delete;
    fallback_env& operator=(fallback_env const&) = delete;
    fallback_env& operator=(fallback_env&&) = delete;

    ~fallback_env() { mock_font_locator::configure({}); }

    [[nodiscard]] open_shaper& shaper() noexcept { return _shaper; }

    /// Loads the named font and returns its key, failing the test if it will not load.
    [[nodiscard]] font_key key(string const& name)
    {
        auto const fontKey = _shaper.load_font(descriptionFor(name, _strictSpacing), bdf_font::Size);
        REQUIRE(fontKey.has_value());
        return *fontKey;
    }

  private:
    bool _strictSpacing;
    vector<bdf_font> _fonts; ///< Declared before _shaper: the shaper borrows these bytes.
    mock_font_locator _locator {};
    open_shaper _shaper { bdf_font::Dpi, _locator };
};

/// Shapes @p codepoints as a run of consecutive cells: one cluster per codepoint.
[[nodiscard]] shape_result shapeCells(open_shaper& shaper, font_key font, u32string const& codepoints)
{
    auto clusters = vector<unsigned> {};
    for (auto const i: views::iota(size_t { 0 }, codepoints.size()))
        clusters.emplace_back(static_cast<unsigned>(i));

    auto result = shape_result {};
    shaper.shape(font,
                 codepoints,
                 gsl::span<unsigned> { clusters },
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 result);
    return result;
}

/// Shapes @p codepoints as a single cell, so that they form one indivisible cluster.
[[nodiscard]] shape_result shapeOneCell(open_shaper& shaper, font_key font, u32string const& codepoints)
{
    auto clusters = vector<unsigned>(codepoints.size(), 0U);

    auto result = shape_result {};
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

TEST_CASE("open_shaper.fallback.only_uncovered_codepoints_fall_back", "[open_shaper][fallback]")
{
    // Issue #1939. The primary is a monospaced terminal font holding the letters but not the prompt
    // ornament -- the SF Mono situation. The fallback covers the ornament, the non-breaking space AND the
    // letters, precisely as DejaVu Sans does; that total coverage is what let the old whole-run fallback
    // "succeed" and drag the letters into the wrong font along with the ornament.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.an_empty_run_shapes_to_nothing", "[open_shaper][fallback]")
{
    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };

    // No codepoints means no glyphs, and nothing to fall back on.
    CHECK(shapeCells(env.shaper(), env.key("primary"), u32string {}).empty());
}

TEST_CASE("open_shaper.fallback.covered_run_never_consults_a_fallback", "[open_shaper][fallback]")
{
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.missing_span_at_each_boundary", "[open_shaper][fallback]")
{
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.walks_further_down_the_chain", "[open_shaper][fallback]")
{
    // No single fallback covers both missing codepoints, so the walk has to reach past the first one.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.unresolvable_glyph_gets_the_primary_replacement", "[open_shaper][fallback]")
{
    // Nothing in the chain has the snowman. It must come back as the *primary* face's replacement
    // character, carrying the primary's font key -- so that glyph index and face agree.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.an_unresolvable_glyph_does_not_poison_the_rest_of_the_run",
          "[open_shaper][fallback]")
{
    // The snowman is in no font at all. Before the fix, its unresolved glyph made every *following*
    // cluster look as though it had failed too: each one walked the entire fallback chain and came back
    // shaped by the last font tried, even though the primary rendered it perfectly well.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.a_combining_mark_travels_with_its_base", "[open_shaper][fallback]")
{
    // One cell holding a base plus a combining accent. The primary has the base but not the accent, so
    // the whole cluster -- base included -- must move to the fallback. Splitting it would shape the base
    // in one font and its mark in another, and the mark would no longer sit over the glyph it belongs to.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.respects_the_fallback_limit", "[open_shaper][fallback]")
{
    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "first", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
        { .name = "second", .monospace = Monospaced, .glyphs = { { Ornament, 8 } } },
    } };

    SECTION("a limit of zero disables fallback entirely")
    {
        env.shaper().set_font_fallback_limit(0); // must precede the first load_font

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
        env.shaper().set_font_fallback_limit(-1);

        auto const primary = env.key("primary");
        auto const second = env.key("second");

        auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
        REQUIRE(result.size() == 1);
        CHECK(result[0].glyph.font == second);
    }
}

TEST_CASE("open_shaper.fallback.extends_the_chain_on_demand", "[open_shaper][fallback]")
{
    // Only the first batch of fallbacks is loaded up front; the rest arrive as the walk runs off the end
    // of the list. Park the one font that can help well beyond that first batch.
    auto specs = vector<font_spec> {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    };
    for (auto const i: views::iota(1, 11))
        specs.emplace_back(
            font_spec { .name = format("filler{}", i), .monospace = Monospaced, .glyphs = { { U'A', 8 } } });
    specs.emplace_back(
        font_spec { .name = "rescuer", .monospace = Monospaced, .glyphs = { { Ornament, 8 } } });

    auto env = fallback_env { specs };

    auto const primary = env.key("primary");
    auto const rescuer = env.key("rescuer");

    auto const result = shapeCells(env.shaper(), primary, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == rescuer);
    CHECK(result[0].glyph.index.value != 0);
}

TEST_CASE("open_shaper.fallback.skips_a_font_that_will_not_load", "[open_shaper][fallback]")
{
    // Hand-rolled rather than built on fallback_env, because the broken entry is deliberately not a font.
    auto const _ = crispy::finally { [] { mock_font_locator::configure({}); } };

    auto primary = bdf_font { "primary", Monospaced, { { U'A', 8 } } };
    auto broken = broken_font {};
    auto rescuer = bdf_font { "rescuer", Monospaced, { { Ornament, 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("broken"), .source = broken.source() },
        { .description = descriptionFor("rescuer"), .source = rescuer.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };

    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const rescuerKey = shaper.load_font(descriptionFor("rescuer"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(rescuerKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == *rescuerKey);
}

TEST_CASE("open_shaper.fallback.non_monotone_clusters_fall_back_on_the_whole_run", "[open_shaper][fallback]")
{
    // Splitting a run into fallback spans rests on cluster values being monotone. Should they ever not
    // be -- a caller breaking the contract, or a shaper configuration that reorders -- there is no
    // trustworthy mapping from glyphs back to codepoints, so the run must not be split. It is offered to
    // the fallback whole instead, which is what every run used to get, rather than being mis-split.
    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { U'B', 8 } } },
        { .name = "fallback",
          .monospace = Monospaced,
          .glyphs = { { Ornament, 8 }, { U'A', 8 }, { U'B', 8 } } },
    } };

    auto const primary = env.key("primary");
    auto const fallback = env.key("fallback");

    auto const codepoints = u32string { Ornament, U'A', U'B' };
    auto clusters = vector<unsigned> { 2, 0, 1 }; // deliberately out of order

    auto result = shape_result {};
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

TEST_CASE("open_shaper.fallback.prefers_a_monospaced_fallback", "[open_shaper][fallback][spacing]")
{
    // Both fallbacks cover the ornament, but only the second is monospaced. A terminal wants that one,
    // even though it comes later in the chain: a proportional face's advances do not line up with the
    // cell grid.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.accepts_a_proportional_fallback_rather_than_render_nothing",
          "[open_shaper][fallback][spacing]")
{
    // Preferring monospace must never harden into refusing everything else: emoji, CJK and symbol
    // fallbacks are frequently proportional, and a glyph that renders today must not become a box.
    auto env = fallback_env { {
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

TEST_CASE("open_shaper.fallback.strict_spacing_refuses_a_proportional_fallback",
          "[open_shaper][fallback][spacing]")
{
    // strict_spacing is the opt-in hard filter: the user has said they would rather see nothing at all
    // than a glyph from a font that does not fit the grid.
    auto env = fallback_env {
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

TEST_CASE("open_shaper.COLRv1", "[open_shaper]")
{
    auto const fontPath = filesystem::path("/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf");
    if (!filesystem::exists(fontPath))
    {
        WARN("Test skipped. Font file not found: " << fontPath << "\n");
        return;
    }

    struct test_font_locator: public font_locator
    {
        filesystem::path path;
        explicit test_font_locator(filesystem::path p): path(std::move(p)) {}

        font_source_list locate(font_description const& /*fd*/) override
        {
            return { font_path { .value = path.string() } };
        }

        font_source_list all() override { return {}; }

        font_source_list resolve(gsl::span<char32_t const /*codepoints*/>) override { return {}; }
    };

    auto testLocator = test_font_locator { fontPath };
    auto shaper = open_shaper(DPI { .x = 96, .y = 96 }, testLocator);

    auto const fd = font_description { .familyName = "Noto Color Emoji" };
    auto const fontSize = text::font_size { 12.0 };

    // Explicitly testing load_font which uses locate()
    auto const fontKeyOpt = shaper.load_font(fd, fontSize);
    REQUIRE(fontKeyOpt.has_value());

    auto const fontKey = *fontKeyOpt;
    auto const codepoint = char32_t { 0x1F600 }; // Grinning Face

    auto const glyphPosOpt = shaper.shape(fontKey, codepoint);
    REQUIRE(glyphPosOpt.has_value());

    auto const& glyphPos = *glyphPosOpt;
    auto const rasterizedGlyphOpt = shaper.rasterize(glyphPos.glyph, render_mode::color);
    REQUIRE(rasterizedGlyphOpt.has_value());

    auto const& glyph = *rasterizedGlyphOpt;

    // COLRv1 with Cairo rendering should produce an RGBA bitmap
    CHECK(glyph.format == text::bitmap_format::rgba);
    CHECK(glyph.bitmapSize.width > vtbackend::Width(0));
    CHECK(glyph.bitmapSize.height > vtbackend::Height(0));
    CHECK(glyph.bitmap.size() > 0);
}

TEST_CASE("open_shaper.coverage.resolves_a_codepoint_the_chain_cannot", "[open_shaper][fallback]")
{
    // The fallback chain is ordered by how well each font matches the DESCRIPTION, so the only face
    // holding a script can sort far past any length worth walking eagerly -- on a stock Fedora install
    // the first CJK face is 83rd of 201, against a chain limit of 16. Once the chain is exhausted the
    // locator is asked which font actually covers the codepoint, and that answer is used.
    auto coverageFont = bdf_font { "coverage", Monospaced, { { Snowman, 8 } } };

    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
        { .name = "fallback", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };
    mock_font_locator::configureCoverage({ coverageFont.source() });

    auto const primary = env.key("primary");
    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);

    CHECK(result[0].glyph.font == primary);
    // The snowman came from neither the primary nor its chain, and it is a real glyph, not .notdef.
    CHECK(result[1].glyph.font != primary);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("open_shaper.coverage.cache_does_not_outlive_the_keys_it_stores", "[open_shaper][fallback]")
{
    // The coverage cache answers with a font_key, and those keys are owned by the maps clear_cache()
    // empties. Surviving that call it would hand back a key nothing can resolve -- and the shaping
    // path REQUIREs the lookup to succeed, so the next frame drawing that codepoint aborted. Any
    // non-DPI font change reaches this: applyFontDescriptions() calls clear_cache().
    auto coverageFont = bdf_font { "coverage", Monospaced, { { Snowman, 8 } } };

    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    mock_font_locator::configureCoverage({ coverageFont.source() });

    // Populate the coverage cache.
    REQUIRE(shapeCells(env.shaper(), env.key("primary"), u32string { Snowman }).size() == 1);

    env.shaper().clear_cache();

    // Re-loading gives a fresh key; shaping the same codepoint must not reach for the stale one.
    auto const primary = env.key("primary");
    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("open_shaper.coverage.is_spent_once_per_span", "[open_shaper][fallback]")
{
    // The coverage lookup answers with a font chosen BECAUSE it covers the codepoints. If shaping
    // against that font still comes back .notdef, asking again would return the same font forever --
    // so the attempt is spent once and the span then settles for the replacement glyph. This case
    // hands it a font that does NOT have the snowman, which is what makes the guard observable: with
    // the attempt unbounded it does not terminate.
    auto uselessFont = bdf_font { "useless", Monospaced, { { U'B', 8 } } };

    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 }, { Replacement, 8 } } },
    } };
    mock_font_locator::configureCoverage({ uselessFont.source() });

    auto const primary = env.key("primary");
    auto const replacement = env.shaper().shape(primary, Replacement);
    REQUIRE(replacement.has_value());

    auto const result = shapeCells(env.shaper(), primary, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);
    CHECK(result[1].glyph.index.value == replacement->glyph.index.value);
}

TEST_CASE("open_shaper.resize_font.reports_not_resized_by_returning_the_key_it_was_given",
          "[open_shaper][resize]")
{
    // The synthetic fonts here are BDF, which ships fixed strikes and cannot be opened at an
    // arbitrary size -- the same position a user's bitmap font is in when `OSC 66` asks for one. The
    // contract that has to hold then is that resize_font() says so by handing back the key it was
    // given, which TextRenderer::rasterizeAtBlockSize() reads as "not resized" and answers by
    // magnifying the raster it already has rather than rasterizing at the wrong size.
    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };
    auto const primary = env.key("primary");

    CHECK(env.shaper().resize_font(primary, font_size { bdf_font::Size.pt * 2 }) == primary);

    // The size it is already at is not a resize at all, and takes the same answer.
    CHECK(env.shaper().resize_font(primary, bdf_font::Size) == primary);
}

TEST_CASE("open_shaper.resize_font.a_size_the_face_lacks_does_not_retire_the_font", "[open_shaper][resize]")
{
    // A source is blacklisted to mean "this file is not a font I can use", and nothing clears that
    // list -- not even clear_cache(). Concluding it from a failure to open a size would let one
    // `OSC 66` scaled write retire a bitmap font for the rest of the session, including at the size
    // it was rendering perfectly well at a moment earlier.
    auto env = fallback_env { {
        { .name = "primary", .monospace = Monospaced, .glyphs = { { U'A', 8 } } },
    } };
    auto const primary = env.key("primary");

    for (auto const i: std::views::iota(1, 8))
        (void) env.shaper().resize_font(primary, font_size { bdf_font::Size.pt + double(i) });

    // Still shapes, and still resolves to a real glyph rather than .notdef.
    auto const result = shapeOneCell(env.shaper(), primary, u32string { U'A' });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.index.value != 0);

    // And the font still loads at its own size, which the blacklist would have prevented.
    CHECK(env.key("primary") == primary);
}
