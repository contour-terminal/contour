// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace std;
using namespace text;

namespace
{
struct mock_font_locator: public font_locator
{
    font_source_list locate(font_description const&) override { return {}; }

    font_source_list all() override { return {}; }

    font_source_list resolve(gsl::span<const char32_t /*codepoints*/>) override { return {}; }
};
} // namespace

TEST_CASE("open_shaper.COLRv1", "[open_shaper]")
{
    auto const fontPath = filesystem::path("/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf");
    if (!filesystem::exists(fontPath))
    {
        WARN("Test skipped. Font file not found: " << fontPath << "\n");
        return;
    }

    mock_font_locator locator;
    auto shaper = open_shaper(DPI { .x = 96, .y = 96 }, locator);

    struct test_font_locator: public mock_font_locator
    {
        filesystem::path path;
        test_font_locator(filesystem::path p): path(std::move(p)) {}

        font_source_list locate(font_description const& /*fd*/) override
        {
            return { font_path { .value = path.string() } };
        }
    };

    test_font_locator testLocator(fontPath);
    shaper.set_locator(testLocator);

    auto const fd = font_description { .familyName = "Noto Color Emoji" };

    auto const fontSize = text::font_size { 12.0 };

    // Explicitly testing load_font which uses locate()
    auto const fontKeyOpt = shaper.load_font(fd, fontSize);

    REQUIRE(fontKeyOpt.has_value());

    auto const fontKey = *fontKeyOpt;

    auto const codepoint = char32_t { 0x1F600 }; // Grinning Face

    // shape() calls load_font internal helper if needed or uses key
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
