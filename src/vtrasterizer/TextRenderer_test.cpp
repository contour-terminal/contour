#include <vtbackend/Color.h>

#include <vtrasterizer/FontDescriptions.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RendererTestHelpers.h>
#include <vtrasterizer/TextRenderer.h>

#include <text_shaper/mock_font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/utils.h>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

static std::filesystem::path testFontPath;

using namespace vtrasterizer;
using namespace vtbackend;
using namespace text;

using TextureAtlas = vtrasterizer::Renderable::TextureAtlas;

namespace vtrasterizer
{
class TextRendererTest
{
  public:
    static std::optional<TextureAtlas::TileCreateData> createRasterizedGlyph(
        TextRenderer& renderer,
        atlas::TileLocation tileLocation,
        text::glyph_key const& glyphKey,
        unicode::PresentationStyle presentation)
    {
        return renderer.createRasterizedGlyph(tileLocation, glyphKey, presentation);
    }
};
} // namespace vtrasterizer

class MockTextRendererEvents: public TextRendererEvents
{
  public:
    void onBeforeRenderingText() override {}
    void onAfterRenderingText() override {}
};

namespace
{
constexpr std::string_view TestFontContent = R"(STARTFONT 2.1
FONT -Success-Console-Medium-R-Normal--12-120-75-75-C-80-ISO10646-1
SIZE 9 96 96
FONTBOUNDINGBOX 8 12 0 -2
STARTPROPERTIES 1
FONT_ASCENT 10
FONT_DESCENT 2
ENDPROPERTIES
CHARS 4
STARTCHAR A
ENCODING 65
SWIDTH 500 0
DWIDTH 8 0
BBX 8 12 0 -2
BITMAP
00
00
18
24
42
42
7E
42
42
42
00
00
ENDCHAR
STARTCHAR B
ENCODING 66
SWIDTH 500 0
DWIDTH 8 0
BBX 8 12 0 -2
BITMAP
00
00
7C
42
42
7C
42
42
42
7C
00
00
ENDCHAR
STARTCHAR U+2500
ENCODING 9472
SWIDTH 500 0
DWIDTH 8 0
BBX 8 12 0 -2
BITMAP
00
00
00
00
00
FF
00
00
00
00
00
00
ENDCHAR
STARTCHAR g
ENCODING 103
SWIDTH 500 0
DWIDTH 8 0
BBX 8 12 0 -2
BITMAP
00
00
00
00
7E
42
42
7E
02
02
42
3C
ENDCHAR
ENDFONT
)";

class MockAtlasBackend: public vtrasterizer::atlas::AtlasBackend
{
  public:
    [[nodiscard]] vtbackend::ImageSize atlasSize() const noexcept override
    {
        return vtbackend::ImageSize { vtbackend::Width(1024), vtbackend::Height(1024) };
    }

    void configureAtlas(vtrasterizer::atlas::ConfigureAtlas) override {}
    void uploadTile(vtrasterizer::atlas::UploadTile) override {}
    void renderTile(vtrasterizer::atlas::RenderTile) override {}
};

class MockRenderTarget: public vtrasterizer::RenderTarget
{
  public:
    void setRenderSize(vtbackend::ImageSize) override {}
    void setMargin(vtrasterizer::PageMargin) override {}
    vtrasterizer::atlas::AtlasBackend& textureScheduler() override { return _textureScheduler; }
    void renderRectangle(int, int, vtbackend::Width, vtbackend::Height, vtbackend::RGBAColor) override {}
    void scheduleScreenshot(ScreenshotCallback) override {}
    void execute(std::chrono::steady_clock::time_point) override {}
    void clearCache() override {}
    std::optional<vtrasterizer::AtlasTextureScreenshot> readAtlas() override { return std::nullopt; }
    void inspect(std::ostream&) const override {}

  private:
    MockAtlasBackend _textureScheduler;
};

std::optional<TextureAtlas::TileCreateData> renderSingleGlyph(TextRenderer& renderer,
                                                              text::shaper& textShaper,
                                                              text::font_key fontKey,
                                                              text::font_size fontSize,
                                                              char32_t codepoint)
{
    text::shape_result shaped;
    std::vector<unsigned> clusters(1, 0); // one char, one cluster

    textShaper.shape(fontKey,
                     std::u32string_view(&codepoint, 1),
                     gsl::span<unsigned>(clusters.data(), clusters.size()),
                     unicode::Script::Common,
                     unicode::PresentationStyle::Text,
                     shaped);

    if (shaped.empty())
        return std::nullopt;

    auto const& run = shaped[0];
    auto const glyphIndex = run.glyph.index;
    auto const glyphKey = glyph_key { .size = fontSize, .font = fontKey, .index = glyphIndex };

    return vtrasterizer::TextRendererTest::createRasterizedGlyph(renderer,
                                                                 atlas::TileLocation {}, // Dummy location
                                                                 glyphKey,
                                                                 unicode::PresentationStyle::Text);
}
} // namespace

TEST_CASE("TextRenderer", "[renderer]")
{
    auto const gridMetrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                           .cellSize = ImageSize { Width(10), Height(20) },
                                           .baseline = 15,
                                           .underline = { .position = 17, .thickness = 1 } };

    auto fontDescriptions = FontDescriptions {};
    fontDescriptions.dpi = DPI { 96, 96 };
    fontDescriptions.size = font_size { 12.0 };
    fontDescriptions.textShapingEngine = TextShapingEngine::OpenShaper;

    // Setup Mock Font Locator
    REQUIRE(std::filesystem::exists(testFontPath));

    auto registry = std::vector<font_description_and_source> {
        { .description = font_description::parse("regular"),
          .source = font_path { .value = std::filesystem::absolute(testFontPath).string() } }
    };
    mock_font_locator::configure(std::move(registry));

    auto fontLocator = std::make_unique<mock_font_locator>();

    // Verify locator finds it
    auto const sources = fontLocator->locate(font_description::parse("regular"));
    CHECK(sources.size() >= 1);

    auto& fontLocatorRef = *fontLocator; // standard usage
    auto textShaper = open_shaper(DPI { 96, 96 }, fontLocatorRef);

    // Pre-load font to get valid key
    auto const regularFontDesc = font_description::parse("regular");
    auto const regularFontSize = font_size { 9.0 }; // Matches BDF SIZE 9 96 96
    auto const regularFontKey = textShaper.load_font(regularFontDesc, regularFontSize);
    REQUIRE(regularFontKey.has_value());

    // We need to initialize metrics for the font.
    // TextRenderer constructor does updateFontMetrics().

    FontKeys fontKeys { .regular = regularFontKey.value(),
                        .bold = regularFontKey.value(), // Reuse
                        .italic = regularFontKey.value(),
                        .boldItalic = regularFontKey.value(),
                        .emoji = regularFontKey.value() };

    MockTextRendererEvents events;

    TextRenderer renderer(gridMetrics, textShaper, fontDescriptions, fontKeys, events);

    MockRenderTarget renderTarget;
    vtrasterizer::atlas::DirectMappingAllocator<vtrasterizer::RenderTileAttributes> allocator;
    renderer.setRenderTarget(renderTarget, allocator);

    auto const renderGlyph = [&](char32_t codepoint) -> std::optional<TextureAtlas::TileCreateData> {
        return renderSingleGlyph(renderer, textShaper, regularFontKey.value(), regularFontSize, codepoint);
    };

    SECTION("rasterize glyph 'A'")
    {
        auto const buffer = renderGlyph(U'A');
        REQUIRE(buffer.has_value());

        // Verify pixels.
        // BBX 8 12
        vtrasterizer::verifyBitmap(*buffer,
                                   { "........",
                                     "........",
                                     "...##...", // 18
                                     "..#..#..", // 24
                                     ".#....#.", // 42
                                     ".#....#.", // 42
                                     ".######.", // 7E
                                     ".#....#.", // 42
                                     ".#....#.", // 42
                                     ".#....#.", // 42
                                     "........",
                                     "........" });
    }

    SECTION("rasterize glyph 'B'")
    {
        auto const buffer = renderGlyph(U'B');
        REQUIRE(buffer.has_value());

        vtrasterizer::verifyBitmap(*buffer,
                                   { "........",
                                     "........",
                                     ".#####..", // 7C
                                     ".#....#.", // 42
                                     ".#....#.", // 42
                                     ".#####..", // 7C
                                     ".#....#.", // 42
                                     ".#....#.", // 42
                                     ".#....#.", // 42
                                     ".#####..", // 7C
                                     "........",
                                     "........" });
    }

    SECTION("rasterize glyph 'g'")
    {
        auto const buffer = renderGlyph(U'g');
        REQUIRE(buffer.has_value());

        vtrasterizer::verifyBitmap(*buffer,
                                   {
                                       "........",
                                       "........",
                                       "........",
                                       "........",
                                       ".######.", // 7E
                                       ".#....#.", // 42
                                       ".#....#.", // 42
                                       ".######.", // 7E
                                       "......#.", // 02
                                       "......#.", // 02
                                       ".#....#.", // 42
                                       "..####.."  // 3C
                                   });
    }
}

int main(int argc, char* argv[])
{
    auto const tempDir = std::filesystem::temp_directory_path();
    auto const _ = crispy::finally { [&] { std::filesystem::remove(testFontPath); } };

    testFontPath = tempDir / "contour_test_font.bdf";

    {
        std::ofstream fontFile(testFontPath);
        fontFile << TestFontContent;
    }

    Catch::Session session;

    int const returnCode = session.applyCommandLine(argc, argv);
    if (returnCode != 0)
        return returnCode;

    int result = session.run();

    return result;
}
