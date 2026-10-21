// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/CoreTextLocator.hpp>
#include <text_shaper/MockFontLocator.hpp>
#include <text_shaper/OpenShaper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <future>
#include <ranges>

using namespace text;

namespace
{
FontDescription description(std::string family)
{
    auto result = FontDescription {};
    result.familyName = std::move(family);
    result.fontFallback = FontFallbackNone {};
    return result;
}

FontPath source(CoreTextLocator& locator, std::string family)
{
    auto fonts = locator.locate(description(std::move(family)));
    REQUIRE(fonts.size() == 1);
    return std::get<FontPath>(fonts.front());
}

bool sameFace(FontPath const& a, FontPath const& b)
{
    return a.value == b.value && a.collectionIndex == b.collectionIndex;
}
} // namespace

TEST_CASE("CoreTextLocator.configured_fallback_chain", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto desc = description("Menlo");
    auto const primary = source(locator, "Menlo");
    auto const emoji = source(locator, "Apple Color Emoji");
    auto const symbol = source(locator, "Apple Symbols");
    desc.fontFallback = FontFallbackList { { "Apple Color Emoji", "Apple Symbols" } };
    SECTION("configured order")
    {
        auto chain = locator.locate(desc);
        REQUIRE(chain.size() == 3);
        CHECK(sameFace(std::get<FontPath>(chain[0]), primary));
        CHECK(sameFace(std::get<FontPath>(chain[1]), emoji));
        CHECK(sameFace(std::get<FontPath>(chain[2]), symbol));
    }
    SECTION("missing and duplicate families")
    {
        desc.fontFallback = FontFallbackList {
            {
                "No such Contour test font",
                "Apple Color Emoji",
                "Apple Color Emoji",
                "Menlo",
            },
        };
        auto chain = locator.locate(desc);
        REQUIRE(chain.size() == 2);
        CHECK(sameFace(std::get<FontPath>(chain[1]), emoji));
    }
    SECTION("none")
    {
        desc.fontFallback = FontFallbackNone {};
        CHECK(locator.locate(desc).size() == 1);
    }
    SECTION("empty explicit list")
    {
        desc.fontFallback = FontFallbackList {};
        CHECK(locator.locate(desc).size() == 1);
    }
    SECTION("automatic cascade")
    {
        desc.fontFallback = std::monostate {};
        CHECK(locator.locate(desc).size() > 1);
    }
}

TEST_CASE("CoreTextLocator.style_and_collection_face", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto desc = description("Menlo");
    auto const regular = source(locator, "Menlo");
    SECTION("bold")
    {
        desc.weight = FontWeight::Bold;
    }
    SECTION("italic")
    {
        desc.slant = FontSlant::Italic;
    }
    SECTION("bold italic")
    {
        desc.weight = FontWeight::Bold;
        desc.slant = FontSlant::Italic;
    }
    auto chain = locator.locate(desc);
    REQUIRE(chain.size() == 1);
    CHECK_FALSE(sameFace(std::get<FontPath>(chain.front()), regular));
}

TEST_CASE("CoreTextLocator.coverage_resolution", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto input = std::u32string_view(U"\U0001F600");
    SECTION("supplementary emoji")
    {
    }
    SECTION("variation selector")
    {
        input = U"\U0001F680\uFE0F";
    }
    auto chain = locator.resolve(gsl::span(input.data(), input.size()));
    REQUIRE_FALSE(chain.empty());
    CHECK(sameFace(std::get<FontPath>(chain.front()), source(locator, "Apple Color Emoji")));
}

TEST_CASE("CoreTextLocator.invalid_coverage_input", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    CHECK(locator.resolve({}).empty());
    auto invalid = std::u32string { char32_t(0x110000) };
    CHECK(locator.resolve(gsl::span(invalid.data(), invalid.size())).empty());
    invalid = { char32_t(0xD800) };
    CHECK(locator.resolve(gsl::span(invalid.data(), invalid.size())).empty());
    invalid = { char32_t(0x10FFFF) };
    CHECK(locator.resolve(gsl::span(invalid.data(), invalid.size())).empty());
}

TEST_CASE("CoreTextLocator.shaper_resolves_emoji_after_exhausted_chain", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto shaper = OpenShaper({ 96, 96 }, locator);
    auto desc = description("Menlo");
    desc.fontFallback = FontFallbackList { { "Apple Symbols" } };
    auto primary = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(primary);
    auto clusters = std::vector<unsigned> { 0, 1, 2 };
    auto result = ShapeResult {};
    shaper.shape(*primary,
                 U"A\U0001F600B",
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Latin,
                 unicode::PresentationStyle::Text,
                 result);
    REQUIRE(result.size() == 3);
    CHECK(result.front().glyph.font == *primary);
    CHECK(result.back().glyph.font == *primary);
    auto emoji = shaper.loadFont(description("Apple Color Emoji"), FontSize { 13 });
    REQUIRE(emoji);
    CHECK(result[1].glyph.font == *emoji);
    CHECK(result[1].glyph.index.value != 0);
}

TEST_CASE("CoreTextLocator.generic_families", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    CHECK(sameFace(source(locator, "emoji"), source(locator, "Apple Color Emoji")));
    CHECK(sameFace(source(locator, "monospace"), source(locator, "Menlo")));
}

TEST_CASE("CoreTextLocator.available_faces", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto const fonts = locator.all();
    REQUIRE_FALSE(fonts.empty());
    auto const menlo = source(locator, "Menlo");
    CHECK(std::ranges::any_of(
        fonts, [&](FontSource const& font) { return sameFace(std::get<FontPath>(font), menlo); }));
}

TEST_CASE("CoreTextLocator.shaper_keeps_collection_styles_distinct", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto shaper = OpenShaper({ 96, 96 }, locator);
    auto desc = description("Menlo");
    auto regular = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(regular);
    desc.slant = FontSlant::Italic;
    auto italic = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(italic);
    CHECK(*regular != *italic);
    desc.weight = FontWeight::Bold;
    auto boldItalic = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(boldItalic);
    desc.slant = FontSlant::Normal;
    auto bold = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(bold);
    CHECK(*bold != *boldItalic);
}

TEST_CASE("CoreTextLocator.failed_collection_face_does_not_blacklist_siblings", "[coretext][fallback]")
{
    auto native = CoreTextLocator {};
    auto valid = source(native, "Menlo");
    auto invalid = valid;
    invalid.collectionIndex = 0xFFFF;
    auto badDescription = description("invalid collection face");
    auto goodDescription = description("valid collection face");
    MockFontLocator::configure({ { badDescription, invalid }, { goodDescription, valid } });
    auto locator = MockFontLocator {};
    auto shaper = OpenShaper({ 96, 96 }, locator);
    CHECK_FALSE(shaper.loadFont(badDescription, FontSize { 13 }));
    CHECK(shaper.loadFont(goodDescription, FontSize { 13 }));
}

TEST_CASE("CoreTextLocator.missing_primary_uses_menlo", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    CHECK(sameFace(source(locator, "No such Contour primary font"), source(locator, "Menlo")));
}

TEST_CASE("CoreTextLocator.concurrent_collection_lookups", "[coretext][fallback][threading]")
{
    auto locator = CoreTextLocator {};
    auto tasks = std::vector<std::future<FontSourceList>> {};
    for (auto const weight: { FontWeight::Normal, FontWeight::Bold })
        for (auto const slant: { FontSlant::Normal, FontSlant::Italic })
            tasks.emplace_back(std::async(std::launch::async, [&locator, weight, slant] {
                auto desc = description("Menlo");
                desc.weight = weight;
                desc.slant = slant;
                return locator.locate(desc);
            }));
    auto const regular = source(locator, "Menlo");
    auto indices = std::vector<int> {};
    for (auto& task: tasks)
    {
        auto const fonts = task.get();
        REQUIRE(fonts.size() == 1);
        auto const& face = std::get<FontPath>(fonts.front());
        CHECK(face.value == regular.value);
        CHECK(std::ranges::find(indices, face.collectionIndex) == indices.end());
        indices.push_back(face.collectionIndex);
    }
}

TEST_CASE("CoreTextLocator.shaper_refreshes_DPI_without_replacing_keys", "[coretext][dpi]")
{
    auto locator = CoreTextLocator {};
    auto shaper = OpenShaper({ 96, 96 }, locator);
    auto desc = description("Menlo");
    desc.fontFallback = FontFallbackList { { "Apple Color Emoji" } };
    auto const primary = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(primary);
    auto const before = shaper.metrics(*primary);
    auto clusters = std::vector<unsigned> { 0 };
    auto first = ShapeResult {};
    shaper.shape(*primary,
                 U"\U0001F600",
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Common,
                 unicode::PresentationStyle::Emoji,
                 first);
    REQUIRE(first.size() == 1);
    auto const emoji = first.front().glyph.font;
    REQUIRE(emoji != *primary);
    auto const bitmap = shaper.rasterize(first.front().glyph, RenderMode::Gray);
    REQUIRE(bitmap);
    shaper.setDPI({ 192, 192 });
    auto const after = shaper.metrics(*primary);
    CHECK(after.advance > before.advance);
    CHECK(after.lineHeight > before.lineHeight);
    CHECK(shaper.loadFont(desc, FontSize { 13 }) == primary);
    auto second = ShapeResult {};
    shaper.shape(*primary,
                 U"\U0001F600",
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Common,
                 unicode::PresentationStyle::Emoji,
                 second);
    REQUIRE(second.size() == 1);
    CHECK(second.front().glyph.font == emoji);
    auto const scaled = shaper.rasterize(second.front().glyph, RenderMode::Gray);
    REQUIRE(scaled);
    CHECK(scaled->bitmapSize.width > bitmap->bitmapSize.width);
    shaper.setDPI({ 192, 192 });
    CHECK(shaper.metrics(*primary).advance == after.advance);
    shaper.setDPI({ 96, 96 });
    CHECK(shaper.metrics(*primary).advance == before.advance);
}

TEST_CASE("CoreTextLocator.strict_chain_recovers_system_symbols", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto shaper = OpenShaper({ 96, 96 }, locator);
    auto desc = description("Menlo");
    SECTION("regular")
    {
    }
    SECTION("bold")
    {
        desc.weight = FontWeight::Bold;
    }
    SECTION("italic")
    {
        desc.slant = FontSlant::Italic;
    }
    SECTION("bold italic")
    {
        desc.weight = FontWeight::Bold;
        desc.slant = FontSlant::Italic;
    }
    desc.strictSpacing = true;
    desc.spacing = FontSpacing::Mono;
    desc.fontFallback = FontFallbackList { { "Apple Symbols" } };
    auto const primary = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(primary);
    // Neither Menlo nor Apple Symbols supplies the stopwatch. CoreText selects a proportional
    // math face for its text presentation, which must remain usable as the final native recovery.
    auto const input = std::u32string(U"A\u23F1B");
    auto clusters = std::vector<unsigned> { 0, 1, 2 };
    auto result = ShapeResult {};
    shaper.shape(*primary,
                 input,
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Common,
                 unicode::PresentationStyle::Text,
                 result);
    REQUIRE(result.size() == 3);
    CHECK(result.front().glyph.font == *primary);
    CHECK(result.back().glyph.font == *primary);
    CHECK(result[1].glyph.font != *primary);
    CHECK(result[1].glyph.index.value != 0);
    auto const bitmap = shaper.rasterize(result[1].glyph, RenderMode::Gray);
    REQUIRE(bitmap);
    CHECK(bitmap->bitmapSize.width > vtpty::Width { 0 });
    CHECK(locator.coverageFallbackSpacing() == CoverageFallbackSpacing::Unrestricted);
    CHECK(MockFontLocator {}.coverageFallbackSpacing() == CoverageFallbackSpacing::MatchDescription);
}

TEST_CASE("CoreTextLocator.native_recovery_still_honors_disabled_fallback", "[coretext][fallback]")
{
    auto locator = CoreTextLocator {};
    auto shaper = OpenShaper({ 96, 96 }, locator);
    auto desc = description("Menlo");
    SECTION("none")
    {
    }
    SECTION("zero limit")
    {
        desc.fontFallback = FontFallbackList { { "Apple Symbols" } };
        shaper.setFontFallbackLimit(0);
    }
    auto const primary = shaper.loadFont(desc, FontSize { 13 });
    REQUIRE(primary);
    auto const replacement = shaper.shape(*primary, U'\uFFFD');
    REQUIRE(replacement);
    auto clusters = std::vector<unsigned> { 0 };
    auto result = ShapeResult {};
    shaper.shape(*primary,
                 U"\u23F1",
                 gsl::span<unsigned>(clusters),
                 unicode::Script::Common,
                 unicode::PresentationStyle::Text,
                 result);
    REQUIRE(result.size() == 1);
    CHECK(result.front().glyph.font == *primary);
    CHECK(result.front().glyph.index.value == replacement->glyph.index.value);
}
