// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/MockFontLocator.hpp>
#include <text_shaper/NerdFontSymbolRanges.hpp>
#include <text_shaper/NerdFontSymbolsLocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace text;

namespace
{

/// A file standing in for the symbols font, removed again when it goes out of scope. The locator only asks
/// whether it is a file, so its contents do not matter here.
struct StandInFont
{
    std::filesystem::path path;

    StandInFont():
        path { std::filesystem::temp_directory_path()
               / std::format("contour-nerd-font-symbols-test-{}.ttf",
                             reinterpret_cast<std::uintptr_t>(this)) }
    {
        std::ofstream { path, std::ios::binary } << "not a font";
    }

    ~StandInFont()
    {
        auto error = std::error_code {};
        std::filesystem::remove(path, error);
    }

    StandInFont(StandInFont const&) = delete;
    StandInFont& operator=(StandInFont const&) = delete;
};

constexpr auto SystemFont = std::string_view { "system-fallback.ttf" };

/// @return The one font the mock locator answers coverage queries with: the system's.
[[nodiscard]] FontSourceList systemFallback()
{
    return FontSourceList { FontPath { .value = std::string { SystemFont } } };
}

[[nodiscard]] std::string pathOf(FontSource const& source)
{
    return std::get<FontPath>(source).value;
}

} // namespace

TEST_CASE("NerdFontSymbolRanges is ascending and without overlap", "[nerd-font]")
{
    for (auto const& range: NerdFontSymbolRanges)
        CHECK(range.first <= range.last);
    for (auto const i: std::views::iota(size_t { 1 }, NerdFontSymbolRanges.size()))
        CHECK(NerdFontSymbolRanges[i - 1].last < NerdFontSymbolRanges[i].first);
}

TEST_CASE("isNerdFontSymbol is true inside the private use ranges Nerd Fonts fills, and false around them",
          "[nerd-font]")
{
    CHECK(isNerdFontSymbol(U''));               // Powerline: a right-pointing solid triangle
    CHECK(isNerdFontSymbol(U''));               // Devicons, which Segoe Fluent Icons also claims
    CHECK(isNerdFontSymbol(U''));               // Font Logos
    CHECK(isNerdFontSymbol(char32_t { 0xF0001 })); // Material Design, the supplementary private use area
    CHECK(isNerdFontSymbol(U'⏻'));                 // IEC power symbol

    CHECK_FALSE(isNerdFontSymbol(U'A'));
    CHECK_FALSE(isNerdFontSymbol(U'─'));                 // box drawing, drawn by the terminal itself
    CHECK_FALSE(isNerdFontSymbol(char32_t { 0x1F600 })); // an emoji
    CHECK_FALSE(isNerdFontSymbol(char32_t { NerdFontSymbolRanges.front().first - 1 }));
    CHECK_FALSE(isNerdFontSymbol(char32_t { NerdFontSymbolRanges.back().last + 1 }));
}

TEST_CASE("NerdFontSymbolsLocator names the bundled font for Nerd Fonts' code points", "[nerd-font]")
{
    MockFontLocator::configure({});
    MockFontLocator::configureCoverage(systemFallback());
    auto inner = MockFontLocator {};
    auto const font = StandInFont {};
    auto locator = NerdFontSymbolsLocator { inner, font.path };
    REQUIRE(locator.hasSymbolsFont());

    SECTION("a run of Nerd Fonts code points is the bundled font's")
    {
        auto const codepoints = std::array<char32_t, 3> { U'', U'', char32_t { 0xF0001 } };
        auto const sources = locator.resolve(codepoints);
        REQUIRE(sources.size() == 1);
        CHECK(std::filesystem::path { pathOf(sources.front()) } == font.path);
    }

    SECTION("any other code point is the wrapped locator's")
    {
        auto const ascii = std::array<char32_t, 1> { U'A' };
        auto const sources = locator.resolve(ascii);
        REQUIRE(sources.size() == 1);
        CHECK(pathOf(sources.front()) == SystemFont);
    }

    SECTION("a run mixing both is the wrapped locator's, not the symbols font's")
    {
        auto const mixed = std::array<char32_t, 2> { U'', U'A' };
        auto const sources = locator.resolve(mixed);
        REQUIRE(sources.size() == 1);
        CHECK(pathOf(sources.front()) == SystemFont);
    }

    SECTION("nothing to resolve is the wrapped locator's")
    {
        auto const sources = locator.resolve(gsl::span<char32_t const> {});
        REQUIRE(sources.size() == 1);
        CHECK(pathOf(sources.front()) == SystemFont);
    }

    SECTION("everything but resolve is forwarded")
    {
        CHECK(locator.coverageFallbackSpacing() == inner.coverageFallbackSpacing());
        CHECK(locator.all().size() == inner.all().size());
    }
}

TEST_CASE("NerdFontSymbolsLocator without the symbols font is the wrapped locator", "[nerd-font]")
{
    MockFontLocator::configure({});
    MockFontLocator::configureCoverage(systemFallback());
    auto inner = MockFontLocator {};
    auto const missing =
        std::filesystem::temp_directory_path() / "contour-nerd-font-symbols-does-not-exist.ttf";
    auto locator = NerdFontSymbolsLocator { inner, missing };

    CHECK_FALSE(locator.hasSymbolsFont());
    auto const nerd = std::array<char32_t, 1> { U'' };
    auto const sources = locator.resolve(nerd);
    REQUIRE(sources.size() == 1);
    CHECK(pathOf(sources.front()) == SystemFont);
}

#ifdef CONTOUR_NERD_FONT_SYMBOLS_FILE
TEST_CASE("the bundled Nerd Fonts symbols font covers every code point of the table", "[nerd-font]")
{
    auto const file = std::filesystem::path { CONTOUR_NERD_FONT_SYMBOLS_FILE };
    if (!std::filesystem::is_regular_file(file))
        SKIP("The Nerd Fonts symbols font is not staged in this build tree.");

    auto library = FT_Library {};
    REQUIRE(FT_Init_FreeType(&library) == 0);
    auto face = FT_Face {};
    REQUIRE(FT_New_Face(library, file.string().c_str(), 0, &face) == 0);

    // A glyph the table claims but the font lacks would be drawn as the font's "missing" box instead of
    // falling through to a system font that has it: so every row must be backed by the font itself.
    auto missing = std::vector<char32_t> {};
    for (auto const& range: NerdFontSymbolRanges)
        for (auto const codepoint: std::views::iota(range.first, char32_t { range.last + 1 }))
            if (FT_Get_Char_Index(face, static_cast<FT_ULong>(codepoint)) == 0)
                missing.push_back(codepoint);
    CAPTURE(missing.size(),
            missing.empty() ? 0U : static_cast<unsigned>(missing.front()),
            missing.empty() ? 0U : static_cast<unsigned>(missing.back()));
    CHECK(missing.empty());

    // The Mono variant, which is what keeps it admissible for a fixed-width primary font.
    CHECK(FT_IS_FIXED_WIDTH(face));

    FT_Done_Face(face);
    FT_Done_FreeType(library);
}
#endif
