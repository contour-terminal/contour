// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/bdf_test_font.h>
#include <text_shaper/font_locator.h>
#include <text_shaper/mock_font_locator.h>
#include <text_shaper/open_shaper.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <ranges>
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

/// The mock locator's registry is process-global; reset it so cases cannot bleed into one another.
struct registry_guard
{
    registry_guard() = default;
    registry_guard(registry_guard const&) = delete;
    registry_guard(registry_guard&&) = delete;
    registry_guard& operator=(registry_guard const&) = delete;
    registry_guard& operator=(registry_guard&&) = delete;
    ~registry_guard() { mock_font_locator::configure({}); }
};

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

TEST_CASE("open_shaper.fallback.only_uncovered_codepoints_fall_back", "[open_shaper][fallback]")
{
    // Issue #1939. The primary is a monospaced terminal font holding the letters but not the prompt
    // ornament -- the SF Mono situation.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 } } };

    // The fallback covers the ornament, the non-breaking space AND the letters, precisely as DejaVu Sans
    // does. That total coverage is what let the old whole-run fallback "succeed" and drag the letters
    // into the wrong font along with the ornament.
    auto fallback =
        bdf_font { "fallback", false, { { Ornament, 20 }, { NoBreakSpace, 3 }, { U'A', 7 }, { U'B', 7 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };

    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const fallbackKey = shaper.load_font(descriptionFor("fallback"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(fallbackKey.has_value());
    REQUIRE(*primaryKey != *fallbackKey);

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament, NoBreakSpace, U'A', U'B' });
    REQUIRE(result.size() == 4);

    // The ornament and the non-breaking space are absent from the primary, so they fall back...
    CHECK(result[0].glyph.font == *fallbackKey);
    CHECK(result[1].glyph.font == *fallbackKey);

    // ...but the letters, which the primary renders perfectly well, must stay with the primary.
    // Before the fix, the whole run was re-shaped in the fallback and these came back as *fallbackKey.
    CHECK(result[2].glyph.font == *primaryKey);
    CHECK(result[3].glyph.font == *primaryKey);

    for (auto const& gpos: result)
        CHECK(gpos.glyph.index.value != 0);
}

TEST_CASE("open_shaper.fallback.an_empty_run_shapes_to_nothing", "[open_shaper][fallback]")
{
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());

    // No codepoints means no glyphs, and nothing to fall back on.
    CHECK(shapeCells(shaper, *primaryKey, u32string {}).empty());
}

TEST_CASE("open_shaper.fallback.covered_run_never_consults_a_fallback", "[open_shaper][fallback]")
{
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 } } };
    auto fallback = bdf_font { "fallback", false, { { U'A', 3 }, { U'B', 3 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { U'A', U'B' });
    REQUIRE(result.size() == 2);

    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == *primaryKey);
        CHECK(gpos.advance.x == 8); // the primary's advance, not the fallback's
    }
}

TEST_CASE("open_shaper.fallback.missing_span_at_each_boundary", "[open_shaper][fallback]")
{
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 } } };
    auto fallback = bdf_font { "fallback", true, { { Ornament, 8 }, { U'A', 8 }, { U'B', 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const fallbackKey = shaper.load_font(descriptionFor("fallback"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(fallbackKey.has_value());

    SECTION("at the start")
    {
        auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament, U'A', U'B' });
        REQUIRE(result.size() == 3);
        CHECK(result[0].glyph.font == *fallbackKey);
        CHECK(result[1].glyph.font == *primaryKey);
        CHECK(result[2].glyph.font == *primaryKey);
    }

    SECTION("in the middle")
    {
        auto const result = shapeCells(shaper, *primaryKey, u32string { U'A', Ornament, U'B' });
        REQUIRE(result.size() == 3);
        CHECK(result[0].glyph.font == *primaryKey);
        CHECK(result[1].glyph.font == *fallbackKey);
        CHECK(result[2].glyph.font == *primaryKey);
    }

    SECTION("at the end")
    {
        auto const result = shapeCells(shaper, *primaryKey, u32string { U'A', U'B', Ornament });
        REQUIRE(result.size() == 3);
        CHECK(result[0].glyph.font == *primaryKey);
        CHECK(result[1].glyph.font == *primaryKey);
        CHECK(result[2].glyph.font == *fallbackKey);
    }

    SECTION("two disjoint spans")
    {
        auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament, U'A', Ornament, U'B' });
        REQUIRE(result.size() == 4);
        CHECK(result[0].glyph.font == *fallbackKey);
        CHECK(result[1].glyph.font == *primaryKey);
        CHECK(result[2].glyph.font == *fallbackKey);
        CHECK(result[3].glyph.font == *primaryKey);
    }
}

TEST_CASE("open_shaper.fallback.walks_further_down_the_chain", "[open_shaper][fallback]")
{
    // No single fallback covers both missing codepoints, so the walk has to reach past the first one.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 } } };
    auto first = bdf_font { "first", true, { { Ornament, 8 } } };
    auto second = bdf_font { "second", true, { { Snowman, 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("first"), .source = first.source() },
        { .description = descriptionFor("second"), .source = second.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const firstKey = shaper.load_font(descriptionFor("first"), bdf_font::Size);
    auto const secondKey = shaper.load_font(descriptionFor("second"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(firstKey.has_value());
    REQUIRE(secondKey.has_value());

    // The ornament and the snowman are separated by 'A', so they form two independent spans, each
    // answered by a different font.
    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament, U'A', Snowman });
    REQUIRE(result.size() == 3);

    CHECK(result[0].glyph.font == *firstKey);
    CHECK(result[1].glyph.font == *primaryKey);
    CHECK(result[2].glyph.font == *secondKey);

    for (auto const& gpos: result)
        CHECK(gpos.glyph.index.value != 0);
}

TEST_CASE("open_shaper.fallback.unresolvable_glyph_gets_the_primary_replacement", "[open_shaper][fallback]")
{
    // Nothing in the chain has the snowman. It must come back as the *primary* face's replacement
    // character, carrying the primary's font key -- so that glyph index and face agree.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { Replacement, 8 } } };
    auto fallback = bdf_font { "fallback", true, { { U'A', 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());

    auto const replacement = shaper.shape(*primaryKey, Replacement);
    REQUIRE(replacement.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { U'A', Snowman });
    REQUIRE(result.size() == 2);

    CHECK(result[0].glyph.font == *primaryKey);
    CHECK(result[1].glyph.font == *primaryKey);
    CHECK(result[1].glyph.index.value == replacement->glyph.index.value);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("open_shaper.fallback.an_unresolvable_glyph_does_not_poison_the_rest_of_the_run",
          "[open_shaper][fallback]")
{
    // The snowman is in no font at all. Before the fix, its unresolved glyph made every *following*
    // cluster look as though it had failed too: each one walked the entire fallback chain and came back
    // shaped by the last font tried, even though the primary rendered it perfectly well.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 }, { Replacement, 8 } } };
    auto fallback = bdf_font { "fallback", true, { { U'A', 3 }, { U'B', 3 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Snowman, U'A', U'B' });
    REQUIRE(result.size() == 3);

    CHECK(result[1].glyph.font == *primaryKey);
    CHECK(result[2].glyph.font == *primaryKey);
    CHECK(result[1].advance.x == 8); // the primary's advance, so genuinely the primary's glyph
    CHECK(result[2].advance.x == 8);
}

TEST_CASE("open_shaper.fallback.a_combining_mark_travels_with_its_base", "[open_shaper][fallback]")
{
    // One cell holding a base plus a combining accent. The primary has the base but not the accent, so
    // the whole cluster -- base included -- must move to the fallback. Splitting it would shape the base
    // in one font and its mark in another, and the mark would no longer sit over the glyph it belongs to.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 } } };
    auto fallback = bdf_font { "fallback", true, { { U'A', 8 }, { Acute, 0 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const fallbackKey = shaper.load_font(descriptionFor("fallback"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(fallbackKey.has_value());

    auto const result = shapeOneCell(shaper, *primaryKey, u32string { U'A', Acute });
    REQUIRE(result.size() == 2);

    for (auto const& gpos: result)
    {
        CHECK(gpos.glyph.font == *fallbackKey);
        CHECK(gpos.glyph.index.value != 0);
    }
}

TEST_CASE("open_shaper.fallback.respects_the_fallback_limit", "[open_shaper][fallback]")
{
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { Replacement, 8 } } };
    auto first = bdf_font { "first", true, { { U'A', 8 } } };
    auto second = bdf_font { "second", true, { { Ornament, 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("first"), .source = first.source() },
        { .description = descriptionFor("second"), .source = second.source() },
    });

    auto locator = mock_font_locator {};

    SECTION("a limit of zero disables fallback entirely")
    {
        auto shaper = open_shaper { bdf_font::Dpi, locator };
        shaper.set_font_fallback_limit(0);

        auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
        REQUIRE(primaryKey.has_value());

        auto const replacement = shaper.shape(*primaryKey, Replacement);
        REQUIRE(replacement.has_value());

        auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
        REQUIRE(result.size() == 1);
        CHECK(result[0].glyph.font == *primaryKey);
        CHECK(result[0].glyph.index.value == replacement->glyph.index.value);
    }

    SECTION("an unlimited chain reaches the last font")
    {
        auto shaper = open_shaper { bdf_font::Dpi, locator };
        shaper.set_font_fallback_limit(-1);

        auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
        auto const secondKey = shaper.load_font(descriptionFor("second"), bdf_font::Size);
        REQUIRE(primaryKey.has_value());
        REQUIRE(secondKey.has_value());

        auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
        REQUIRE(result.size() == 1);
        CHECK(result[0].glyph.font == *secondKey);
    }
}

TEST_CASE("open_shaper.fallback.extends_the_chain_on_demand", "[open_shaper][fallback]")
{
    // Only the first batch of fallbacks is loaded up front; the rest arrive as the walk runs off the end
    // of the list. Park the one font that can help well beyond that first batch.
    auto const guard = registry_guard {};

    auto names = vector<string> { "primary" };
    for (auto const i: views::iota(1, 11))
        names.emplace_back(format("filler{}", i));
    names.emplace_back("rescuer");

    auto fonts = vector<bdf_font> {};
    fonts.reserve(names.size());
    for (auto const& name: names)
        fonts.emplace_back(name,
                           true,
                           name == "rescuer" ? vector<bdf_glyph> { { Ornament, 8 } }
                                             : vector<bdf_glyph> { { U'A', 8 } });

    auto registry = vector<font_description_and_source> {};
    for (auto const i: views::iota(size_t { 0 }, fonts.size()))
        registry.emplace_back(font_description_and_source { .description = descriptionFor(names[i]),
                                                            .source = fonts[i].source() });
    mock_font_locator::configure(registry);

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };

    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const rescuerKey = shaper.load_font(descriptionFor("rescuer"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(rescuerKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == *rescuerKey);
    CHECK(result[0].glyph.index.value != 0);
}

TEST_CASE("open_shaper.fallback.skips_a_font_that_will_not_load", "[open_shaper][fallback]")
{
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 } } };
    auto broken = broken_font {};
    auto rescuer = bdf_font { "rescuer", true, { { Ornament, 8 } } };

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
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 } } };
    auto fallback = bdf_font { "fallback", true, { { Ornament, 8 }, { U'A', 8 }, { U'B', 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("fallback"), .source = fallback.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };
    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const fallbackKey = shaper.load_font(descriptionFor("fallback"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(fallbackKey.has_value());

    auto const codepoints = u32string { Ornament, U'A', U'B' };
    auto clusters = vector<unsigned> { 2, 0, 1 }; // deliberately out of order

    auto result = shape_result {};
    shaper.shape(*primaryKey,
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
        CHECK(gpos.glyph.font == *fallbackKey);
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
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 } } };
    auto proportional = bdf_font { "proportional", false, { { Ornament, 3 } } };
    auto monospaced = bdf_font { "monospaced", true, { { Ornament, 8 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("proportional"), .source = proportional.source() },
        { .description = descriptionFor("monospaced"), .source = monospaced.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };

    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const monospacedKey = shaper.load_font(descriptionFor("monospaced"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(monospacedKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == *monospacedKey);
    CHECK(result[0].advance.x == 8);
}

TEST_CASE("open_shaper.fallback.accepts_a_proportional_fallback_rather_than_render_nothing",
          "[open_shaper][fallback][spacing]")
{
    // Preferring monospace must never harden into refusing everything else: emoji, CJK and symbol
    // fallbacks are frequently proportional, and a glyph that renders today must not become a box.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { Replacement, 8 } } };
    auto proportional = bdf_font { "proportional", false, { { Ornament, 3 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary"), .source = primary.source() },
        { .description = descriptionFor("proportional"), .source = proportional.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };

    auto const primaryKey = shaper.load_font(descriptionFor("primary"), bdf_font::Size);
    auto const proportionalKey = shaper.load_font(descriptionFor("proportional"), bdf_font::Size);
    REQUIRE(primaryKey.has_value());
    REQUIRE(proportionalKey.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == *proportionalKey);
    CHECK(result[0].glyph.index.value != 0);
}

TEST_CASE("open_shaper.fallback.strict_spacing_refuses_a_proportional_fallback",
          "[open_shaper][fallback][spacing]")
{
    // strict_spacing is the opt-in hard filter: the user has said they would rather see nothing at all
    // than a glyph from a font that does not fit the grid.
    auto const guard = registry_guard {};
    auto primary = bdf_font { "primary", true, { { U'A', 8 }, { Replacement, 8 } } };
    auto proportional = bdf_font { "proportional", false, { { Ornament, 3 } } };

    mock_font_locator::configure({
        { .description = descriptionFor("primary", true), .source = primary.source() },
        { .description = descriptionFor("proportional", true), .source = proportional.source() },
    });

    auto locator = mock_font_locator {};
    auto shaper = open_shaper { bdf_font::Dpi, locator };

    auto const primaryKey = shaper.load_font(descriptionFor("primary", true), bdf_font::Size);
    REQUIRE(primaryKey.has_value());

    auto const replacement = shaper.shape(*primaryKey, Replacement);
    REQUIRE(replacement.has_value());

    auto const result = shapeCells(shaper, *primaryKey, u32string { Ornament });
    REQUIRE(result.size() == 1);
    CHECK(result[0].glyph.font == *primaryKey);
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

        font_source_list resolve(gsl::span<const char32_t /*codepoints*/>) override { return {}; }
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
