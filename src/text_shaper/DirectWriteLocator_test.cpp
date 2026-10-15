// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/DirectWriteLocator.hpp>
#include <text_shaper/DirectWriteShaper.hpp>
#include <text_shaper/MockFontLocator.hpp>
#include <text_shaper/OpenShaper.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <type_traits>

using namespace text;

namespace
{

/// Uses Windows-provided fonts so these tests do not require user-installed Nerd Fonts.
[[nodiscard]] FontDescription description(std::string family)
{
    auto result = FontDescription {};
    result.familyName = std::move(family);
    result.spacing = FontSpacing::Mono;
    return result;
}

[[nodiscard]] FontPath source(DirectWriteLocator& locator, std::string family)
{
    auto const fonts = locator.locate(description(std::move(family)));
    REQUIRE_FALSE(fonts.empty());
    return std::get<FontPath>(fonts.front());
}

} // namespace

TEST_CASE("DirectWriteLocator.configured_fallback_chain", "[directwrite][fallback]")
{
    auto locator = DirectWriteLocator {};
    auto desc = description("Consolas");
    auto const primary = source(locator, "Consolas");
    auto const emoji = source(locator, "Segoe UI Emoji");
    auto const symbol = source(locator, "Segoe UI Symbol");
    desc.fontFallback = FontFallbackList { { "Segoe UI Emoji", "Segoe UI Symbol" } };
    SECTION("configured order")
    {
        auto const chain = locator.locate(desc);
        REQUIRE(chain.size() == 3);
        CHECK(std::get<FontPath>(chain[0]).value == primary.value);
        CHECK(std::get<FontPath>(chain[1]).value == emoji.value);
        CHECK(std::get<FontPath>(chain[2]).value == symbol.value);
    }
    SECTION("missing and duplicate families")
    {
        desc.fontFallback =
            FontFallbackList { { "No such Contour test font", "Segoe UI Emoji", "Segoe UI Emoji" } };
        auto const chain = locator.locate(desc);
        REQUIRE(chain.size() == 2);
        CHECK(std::get<FontPath>(chain[1]).value == emoji.value);
    }
    SECTION("none")
    {
        desc.fontFallback = FontFallbackNone {};
        CHECK(locator.locate(desc).size() == 1);
    }
    SECTION("strict monospace")
    {
        desc.strictSpacing = true;
        auto const chain = locator.locate(desc);
        REQUIRE(chain.size() == 1);
        CHECK(std::get<FontPath>(chain.front()).value == primary.value);
    }
}

TEMPLATE_TEST_CASE("DirectWriteLocator.shapers_preserve_covered_text",
                   "[directwrite][fallback]",
                   OpenShaper,
                   DirectWriteShaper)
{
    auto nativeLocator = DirectWriteLocator {};
    auto locator = MockFontLocator {};
    auto desc = description("Consolas");
    desc.fontFallback = FontFallbackList { { "Segoe UI Emoji" } };
    MockFontLocator::configure(
        { { desc, source(nativeLocator, "Consolas") },
          { description("Segoe UI Emoji"), source(nativeLocator, "Segoe UI Emoji") } });
    // No system coverage answer: ignoring the configured chain must leave a missing glyph.
    auto shaper = TestType({ 96, 96 }, locator);
    auto const primary = shaper.loadFont(desc, FontSize { 13 });
    auto const fallback = shaper.loadFont(description("Segoe UI Emoji"), FontSize { 13 });
    REQUIRE(primary);
    REQUIRE(fallback);
    REQUIRE(*primary != *fallback);
    auto input = std::u32string_view {};
    auto clusters = std::vector<unsigned> {};
    SECTION("supplementary codepoint")
    {
        input = U"A\U0001F600B";
        clusters = { 0, 1, 2 };
    }
    SECTION("variation selector shares its base cell")
    {
        input = U"A\U0001F600\uFE0FB";
        clusters = { 0, 1, 1, 2 };
    }
    auto mixed = ShapeResult {};
    shaper.shape(*primary,
                 input,
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 mixed);
    REQUIRE(mixed.size() >= 3);
    CHECK(mixed.front().glyph.font == *primary);
    CHECK(mixed.back().glyph.font == *primary);
    for (auto const i: std::views::iota(size_t { 1 }, mixed.size() - 1))
        CHECK(mixed[i].glyph.font != *primary);
    for (auto const& glyph: mixed)
        CHECK(glyph.glyph.index.value != 0);
}

TEST_CASE("DirectWriteShaper.disabled_fallback", "[directwrite][fallback]")
{
    auto nativeLocator = DirectWriteLocator {};
    auto locator = MockFontLocator {};
    auto shaper = DirectWriteShaper({ 96, 96 }, locator);
    auto desc = description("Consolas");
    desc.fontFallback = FontFallbackList { { "Segoe UI Emoji" } };
    SECTION("none policy")
    {
        desc.fontFallback = FontFallbackNone {};
    }
    SECTION("zero limit")
    {
        shaper.setFontFallbackLimit(0);
    }
    MockFontLocator::configure(
        { { desc, source(nativeLocator, "Consolas") },
          { description("Segoe UI Emoji"), source(nativeLocator, "Segoe UI Emoji") } });
    MockFontLocator::configureCoverage({ source(nativeLocator, "Segoe UI Emoji") });
    auto const font = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(font);
    auto clusters = std::vector<unsigned> { 0, 1, 2 };
    auto result = ShapeResult {};
    shaper.shape(*font,
                 U"A\U0001F600B",
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 result);
    REQUIRE(result.size() == 3);
    CHECK(result[0].glyph.index.value != 0);
    CHECK(result[1].glyph.index.value == 0);
    CHECK(result[2].glyph.index.value != 0);
    for (auto const& glyph: result)
        CHECK(glyph.glyph.font == *font);
}

TEMPLATE_TEST_CASE("DirectWriteLocator.shapers_preserve_presentation",
                   "[directwrite][fallback]",
                   OpenShaper,
                   DirectWriteShaper)
{
    auto locator = DirectWriteLocator {};
    auto shaper = TestType({ 96, 96 }, locator);
    auto desc = description("Consolas");
    desc.fontFallback = FontFallbackNone {};
    auto const font = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(font);
    auto input = std::u32string_view {};
    auto presentation = unicode::PresentationStyle::Text;
    SECTION("VS15 text")
    {
        input = U"\u263A\uFE0E";
    }
    SECTION("VS16 emoji")
    {
        input = U"\u263A\uFE0F";
        presentation = unicode::PresentationStyle::Emoji;
    }
    auto clusters = std::vector<unsigned>(input.size(), 0);
    auto result = ShapeResult {};
    shaper.shape(*font, input, gsl::span<unsigned>(clusters), unicode::Script::Common, presentation, result);
    REQUIRE_FALSE(result.empty());
    if constexpr (std::is_same_v<TestType, DirectWriteShaper>)
        REQUIRE(result.size() == 1); // DirectWrite consumes the selector without an extra glyph.
    for (auto const& glyph: result)
    {
        CHECK(glyph.presentation == presentation);
        CHECK(glyph.glyph.font == *font);
        CHECK(glyph.glyph.index.value != 0);
    }
}

TEMPLATE_TEST_CASE("DirectWrite.shapers_rasterize_empty_glyphs",
                   "[directwrite][rasterize]",
                   OpenShaper,
                   DirectWriteShaper)
{
    auto locator = DirectWriteLocator {};
    auto shaper = TestType({ 96, 96 }, locator);
    auto const font = shaper.loadFont(description("Consolas"), FontSize { 13 });
    REQUIRE(font);
    auto input = std::u32string_view {};
    SECTION("space has no ink")
    {
        input = U" ";
    }
    SECTION("printable glyph still has ink")
    {
        input = U"A";
    }
    auto clusters = std::vector<unsigned> { 0 };
    auto result = ShapeResult {};
    shaper.shape(*font,
                 input,
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 result);
    REQUIRE(result.size() == 1);
    auto const bitmap = shaper.rasterize(result.front().glyph, RenderMode::Gray);
    REQUIRE(bitmap);
    CHECK(bitmap->valid());
    CHECK(bitmap->bitmap.empty() == (input == U" "));
}
