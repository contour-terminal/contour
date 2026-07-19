#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>

#include <vtrasterizer/FontDescriptions.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/Renderer.h>
#include <vtrasterizer/RendererTestHelpers.h>
#include <vtrasterizer/TextRenderer.h>

#include <text_shaper/bdf_test_font.h>
#include <text_shaper/mock_font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/SuppressWindowsDialogs.hpp>
#include <crispy/utils.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>
#include <string_view>
#include <thread>

static std::filesystem::path testFontPath;

using namespace vtrasterizer;
using namespace vtbackend;
using namespace text;

using TextureAtlas = vtrasterizer::Renderable::TextureAtlas;

template <typename T, typename U, typename S>
    requires std::is_convertible_v<T, S>
constexpr boxed::boxed<T, U> operator*(boxed::boxed<T, U> w, S scalar) noexcept
{
    return boxed::boxed<T, U>(w.value * scalar);
}

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

/// Test accessor granting unit tests access to Renderer's deferred-reconfiguration internals.
class RendererTest
{
  public:
    /// Triggers the render-thread reconfiguration step that renderImpl() would normally run.
    static void applyPendingReconfig(Renderer& renderer) { renderer.applyPendingReconfig(); }

    /// Returns the *live* grid metrics (mutated only on the render thread).
    [[nodiscard]] static GridMetrics const& liveGridMetrics(Renderer const& renderer)
    {
        return renderer._gridMetrics;
    }

    /// Returns whether a reconfiguration request is currently staged but not yet applied.
    [[nodiscard]] static bool hasPendingReconfig(Renderer const& renderer)
    {
        return renderer._pendingReconfig.has_value();
    }

    /// Seeds self-consistent grid metrics into the live and published metrics.
    ///
    /// The minimal BDF test font yields zero line metrics from the shaper, which a real TextureAtlas
    /// (and the cursor/decoration direct mappings) reject. Tests that need a render target / atlas use
    /// this to provide usable metrics without depending on font metrics.
    static void seedMetrics(Renderer& renderer, GridMetrics metrics)
    {
        renderer._gridMetrics = metrics;
        renderer._publishedMetrics = metrics;
        renderer._publishedCellSize.store(metrics.cellSize, std::memory_order_release);
    }

    /// Returns the staged pending font descriptions, if a full font-descriptions change is staged.
    [[nodiscard]] static std::optional<FontDescriptions> pendingFontDescriptions(Renderer const& renderer)
    {
        if (!renderer._pendingReconfig)
            return std::nullopt;
        return renderer._pendingReconfig->fontDescriptions;
    }

    /// Returns the staged pending font size, if a size-only change is staged.
    [[nodiscard]] static std::optional<text::font_size> pendingFontSize(Renderer const& renderer)
    {
        if (!renderer._pendingReconfig)
            return std::nullopt;
        return renderer._pendingReconfig->fontSize;
    }

    /// Returns the *live* font descriptions (mutated only on the render thread).
    [[nodiscard]] static FontDescriptions const& liveFontDescriptions(Renderer const& renderer)
    {
        return renderer._fontDescriptions;
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
CHARS 6
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
STARTCHAR U+E000
ENCODING 57344
SWIDTH 500 0
DWIDTH 16 0
BBX 16 24 0 -4
BITMAP
0000
0000
07E0
0C30
1818
300C
300C
3FFC
3FFC
300C
300C
300C
300C
300C
300C
1818
0C30
07E0
0000
0000
0000
0000
0000
0000
ENDCHAR
STARTCHAR U+E001
ENCODING 57345
SWIDTH 500 0
DWIDTH 30 0
BBX 30 18 0 -2
BITMAP
00000000
3FFFFFFC
3FFFFFFC
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3000000C
3FFFFFFC
3FFFFFFC
00000000
ENDCHAR
ENDFONT
)";

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

    constexpr auto RegularBitmapWidth = Width(8);
    constexpr auto RegularBitmapHeight = Height(12);

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

    auto renderer = TextRenderer { gridMetrics, textShaper, fontDescriptions, fontKeys, events };

    MockRenderTarget renderTarget;
    vtrasterizer::atlas::DirectMappingAllocator<vtrasterizer::RenderTileAttributes> allocator;
    renderer.setRenderTarget(renderTarget, allocator);

    // The tile is one CELL, exactly as Renderer::configureTextureAtlas sets it in production. A
    // roomier tile (this was 256x256, ~12x the cell) makes the atlas' central invariant
    // untestable: tile origins are spaced one tile apart, so an oversized bitmap overwrites its
    // neighbour, and nothing here could ever produce one large enough to notice.
    auto const atlasProperties =
        vtrasterizer::atlas::AtlasProperties { .format = vtrasterizer::atlas::Format::Red,
                                               .tileSize = gridMetrics.cellSize,
                                               .hashCount = { 1024 },
                                               .tileCount = { 4096 },
                                               .directMappingCount = 128 };
    auto textureAtlas = TextureAtlas(renderTarget.getMockBackend(), atlasProperties);
    renderer.setTextureAtlas(textureAtlas);

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

    SECTION("rasterize oversized non-RGBA glyph U+E000")
    {
        // U+E000 is a 16x24 glyph (BBX 16 24 0 -4), larger than the cell size (10x20).
        // This simulates Nerd Font icons that exceed cell dimensions in alpha_mask format.
        // Before the fix, such glyphs would crash in the cropping path.
        auto const buffer = renderGlyph(U'\uE000');
        REQUIRE(buffer.has_value());

        // The HEIGHT is what the atlas bounds: a tile is exactly one cell tall and the next tile's
        // origin is that far away, so extra rows overwrite an unrelated glyph.
        CHECK(buffer->bitmapSize.height <= gridMetrics.cellSize.height);

        // The width is NOT bounded here, for the same reason as the wide-but-not-tall case below:
        // a glyph wider than one cell is sliced across cells by createSlicedRasterizedGlyph. Scaling
        // it to fit one cell would squash a 16-wide glyph to 10 rather than to the 13 the height
        // ratio alone calls for, which is the aspect the font actually asked for.
        CHECK(buffer->bitmapSize.width == vtbackend::Width(13));
    }

    SECTION("wide-but-not-tall non-RGBA glyph is not scaled down")
    {
        // U+E001 is a 30x18 glyph (BBX 30 18 0 -2), wider than cell but fits in cell height.
        // This simulates a programming ligature that spans multiple cells.
        // The glyph must NOT be scaled down — the wide bitmap must be preserved so that
        // createSlicedRasterizedGlyph() can handle it for proper rendering across multiple cells.
        auto const buffer = renderGlyph(U'\uE001');
        REQUIRE(buffer.has_value());

        // The bitmap width must remain wider than cell width (not scaled down).
        CHECK(buffer->bitmapSize.width > gridMetrics.cellSize.width);
        // The bitmap height must fit within cell height.
        CHECK(buffer->bitmapSize.height <= gridMetrics.cellSize.height);
    }

    auto const renderAndCapture = [&](char32_t codepoint, vtbackend::LineFlags flags) {
        renderTarget.getMockBackend().renderCommands.clear();
        renderer.beginFrame();
        auto cell = vtbackend::RenderCell {};
        cell.codepoints = std::u32string(1, codepoint);
        cell.attributes.flags = vtbackend::CellFlags {};
        cell.attributes.lineFlags = flags;
        cell.attributes.foregroundColor = vtbackend::RGBColor(0xFF, 0xFF, 0xFF);
        renderer.renderCell(cell);
        renderer.endFrame();
        return renderTarget.getMockBackend().renderCommands;
    };

    SECTION("Double Width")
    {
        auto const commands = renderAndCapture(U'A', vtbackend::LineFlag::DoubleWidth);
        REQUIRE(commands.size() == 1);
        auto const& cmd = commands[0];

        // 'A' bitmap width is 8. Double width -> 16.
        CHECK(cmd.targetSize.width == RegularBitmapWidth * 2);
        CHECK(cmd.targetSize.height == RegularBitmapHeight);
    }

    SECTION("Double Height Top")
    {
        auto const commands = renderAndCapture(U'A', vtbackend::LineFlag::DoubleHeightTop);
        REQUIRE(commands.size() == 1);
        auto const& cmd = commands[0];

        CHECK(cmd.targetSize.width == RegularBitmapWidth * 2);

        // Compare against normal render for UV checks
        auto const normalCommands = renderAndCapture(U'A', vtbackend::LineFlag::None);
        REQUIRE(normalCommands.size() == 1);
        auto const& normalCmd = normalCommands[0];

        CHECK(cmd.normalizedLocation.height == Catch::Approx(normalCmd.normalizedLocation.height / 2.0f));
        CHECK(cmd.normalizedLocation.y == Catch::Approx(normalCmd.normalizedLocation.y));
    }

    SECTION("Double Height Bottom")
    {
        auto const commands = renderAndCapture(U'A', vtbackend::LineFlag::DoubleHeightBottom);
        REQUIRE(commands.size() == 1);
        auto const& cmd = commands[0];

        CHECK(cmd.targetSize.width == RegularBitmapWidth * 2);

        auto const normalCommands = renderAndCapture(U'A', vtbackend::LineFlag::None);
        REQUIRE(normalCommands.size() == 1);
        auto const& normalCmd = normalCommands[0];

        CHECK(cmd.normalizedLocation.height == Catch::Approx(normalCmd.normalizedLocation.height / 2.0f));
        CHECK(
            cmd.normalizedLocation.y
            == Catch::Approx(normalCmd.normalizedLocation.y + (normalCmd.normalizedLocation.height / 2.0f)));
    }

    SECTION("Double Width Advancement")
    {
        renderTarget.getMockBackend().renderCommands.clear();
        renderer.beginFrame();

        // Render "A"
        auto cellA = vtbackend::RenderCell {};
        cellA.position = vtbackend::CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };
        cellA.codepoints = U"A";
        cellA.attributes.flags = vtbackend::CellFlags {};
        cellA.attributes.lineFlags = vtbackend::LineFlag::DoubleWidth;
        cellA.attributes.foregroundColor = vtbackend::RGBColor(0xFF, 0xFF, 0xFF);
        renderer.renderCell(cellA);

        // Render "B" at Column 2 (since A takes 2 columns in Screen).
        auto cellB = vtbackend::RenderCell {};
        cellB.position = vtbackend::CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) };
        cellB.codepoints = U"B";
        cellB.attributes.flags = vtbackend::CellFlags {};
        cellB.attributes.lineFlags = vtbackend::LineFlag::DoubleWidth;
        cellB.attributes.foregroundColor = vtbackend::RGBColor(0xFF, 0xFF, 0xFF);
        renderer.renderCell(cellB);

        renderer.endFrame();

        auto const& commands = renderTarget.getMockBackend().renderCommands;
        REQUIRE(commands.size() == 2);

        auto const& cmdA = commands[0];
        auto const& cmdB = commands[1];

        // A at X=0.
        CHECK(cmdA.x.value == 0);
        // B at col 2 -> X=20.
        CHECK(cmdB.x.value == 20);
    }

    SECTION("Double Width with Preceding Spaces")
    {
        renderTarget.getMockBackend().renderCommands.clear();
        renderer.beginFrame();

        // "  A". 2 spaces -> Col 4 (2*2).
        auto cellA = vtbackend::RenderCell {};
        cellA.groupStart = true;
        cellA.position = vtbackend::CellLocation { .line = LineOffset(0), .column = ColumnOffset(4) };
        cellA.codepoints = U"A";
        cellA.attributes.flags = vtbackend::CellFlags {};
        cellA.attributes.lineFlags = vtbackend::LineFlag::DoubleWidth;
        cellA.attributes.foregroundColor = vtbackend::RGBColor(0xFF, 0xFF, 0xFF);
        renderer.renderCell(cellA);

        renderer.endFrame();

        auto const& commands = renderTarget.getMockBackend().renderCommands;
        REQUIRE(commands.size() == 1);
        auto const& cmdA = commands[0];

        // Col 4 -> 40px.
        CHECK(cmdA.x.value == 40);
    }

    SECTION("Double Height Vertical Placement")
    {
        // Test vertical placement for 'A' (Ascent 10, Descent 2) and 'g' (Ascent 10, Descent 2 but usually
        // descender-heavy). In the test font, 'A' and 'g' have identical BBX. We will capture the Y
        // coordinate.

        auto const checkY = [&](char32_t cp, vtbackend::LineFlag flag, int expectedY) {
            auto const commands = renderAndCapture(cp, flag);
            REQUIRE(commands.size() == 1);
            CHECK(commands[0].y.value == expectedY);
            return commands[0].y.value;
        };

        // Current Buggy Implementation calculations:
        // Pen.y = 20 (Line 0 Bottom).
        // Baseline = 15.
        // BearingY (A/g) = 10.
        // Height (A/g) = 12.

        // 1x Top: 20 - 15 - 10 = -5.

        // DoubleHeightTop Impl: pen.y += LineHeight(20) - Height(12) - Baseline(15) = -7.
        // Target = -5 + (-7) = -12.

        // DoubleHeightBottom Impl: pen.y -= Baseline(15).
        // Target = -5 + (-15) = -20.

        // Verify Buggy behavior first (to confirm I understand the bug/code).
        // Update: Corrected values derived from fix:
        // Top: +5.
        // Bottom: -3.
        checkY(U'g', vtbackend::LineFlag::DoubleHeightTop, 5);
        checkY(U'g', vtbackend::LineFlag::DoubleHeightBottom, -3);
    }
}

TEST_CASE("Renderer.findCellPartitionPoint", "[renderer]")
{
    SECTION("empty vector returns 0")
    {
        std::vector<RenderCell> cells;
        CHECK(Renderer::findCellPartitionPoint(cells, LineCount(5)) == 0);
    }

    SECTION("all cells below boundary")
    {
        std::vector<RenderCell> cells;
        for (auto i = 0; i < 3; ++i)
        {
            auto cell = RenderCell {};
            cell.position = CellLocation { .line = LineOffset(i), .column = ColumnOffset(0) };
            cells.push_back(cell);
        }
        CHECK(Renderer::findCellPartitionPoint(cells, LineCount(5)) == 3);
    }

    SECTION("all cells above boundary")
    {
        std::vector<RenderCell> cells;
        for (auto i = 5; i < 8; ++i)
        {
            auto cell = RenderCell {};
            cell.position = CellLocation { .line = LineOffset(i), .column = ColumnOffset(0) };
            cells.push_back(cell);
        }
        CHECK(Renderer::findCellPartitionPoint(cells, LineCount(5)) == 0);
    }

    SECTION("mixed: partition at boundary")
    {
        std::vector<RenderCell> cells;
        for (auto i = 0; i < 6; ++i)
        {
            auto cell = RenderCell {};
            cell.position = CellLocation { .line = LineOffset(i), .column = ColumnOffset(0) };
            cells.push_back(cell);
        }
        // Boundary at line 3: cells at lines 0,1,2 are below, cells at 3,4,5 are at or above.
        CHECK(Renderer::findCellPartitionPoint(cells, LineCount(3)) == 3);
    }
}

TEST_CASE("Renderer.findLinePartitionPoint", "[renderer]")
{
    SECTION("empty vector returns 0")
    {
        std::vector<RenderLine> lines;
        CHECK(Renderer::findLinePartitionPoint(lines, LineCount(5)) == 0);
    }

    SECTION("all lines below boundary")
    {
        std::vector<RenderLine> lines;
        for (auto i = 0; i < 3; ++i)
        {
            auto line = RenderLine {};
            line.lineOffset = LineOffset(i);
            lines.push_back(line);
        }
        CHECK(Renderer::findLinePartitionPoint(lines, LineCount(5)) == 3);
    }

    SECTION("all lines above boundary")
    {
        std::vector<RenderLine> lines;
        for (auto i = 5; i < 8; ++i)
        {
            auto line = RenderLine {};
            line.lineOffset = LineOffset(i);
            lines.push_back(line);
        }
        CHECK(Renderer::findLinePartitionPoint(lines, LineCount(5)) == 0);
    }

    SECTION("mixed: partition at boundary")
    {
        std::vector<RenderLine> lines;
        for (auto i = 0; i < 6; ++i)
        {
            auto line = RenderLine {};
            line.lineOffset = LineOffset(i);
            lines.push_back(line);
        }
        // Boundary at line 4: lines 0,1,2,3 are below, lines 4,5 are at or above.
        CHECK(Renderer::findLinePartitionPoint(lines, LineCount(4)) == 4);
    }
}

namespace
{

/// Bundles a fully-constructed headless Renderer (mock font locator, mock render target) so the
/// deferred-reconfiguration code paths can be exercised without a GPU/Qt context.
struct ReconfigFixture
{
    FontDescriptions fontDescriptions = [] {
        auto fd = FontDescriptions {};
        fd.dpi = DPI { 96, 96 };
        fd.size = text::font_size { 9.0 }; // Matches the BDF test font's SIZE 9 96 96.
        fd.textShapingEngine = TextShapingEngine::OpenShaper;
        fd.fontLocator = FontLocatorEngine::Mock;
        fd.regular = text::font_description::parse("regular");
        fd.bold = text::font_description::parse("regular");
        fd.italic = text::font_description::parse("regular");
        fd.boldItalic = text::font_description::parse("regular");
        fd.emoji = text::font_description::parse("regular");
        return fd;
    }();

    // The minimal BDF test font produces zero line metrics from the shaper, which a real TextureAtlas
    // (and direct-mapped cursor/decoration tiles) reject. Tests seed self-consistent metrics via
    // RendererTest::seedMetrics() instead. Mirrors the known-good metrics used by the TextRenderer test.
    static GridMetrics seededMetrics()
    {
        return GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                             .cellSize = vtbackend::ImageSize { Width(10), Height(20) },
                             .baseline = 15,
                             .underline = { .position = 17, .thickness = 1 } };
    }

    vtbackend::ColorPalette colorPalette {};
    MockRenderTarget renderTarget {};
    Renderer renderer;

    ReconfigFixture():
        renderer(vtbackend::PageSize { LineCount(24), ColumnCount(80) },
                 fontDescriptions,
                 colorPalette,
                 crispy::strong_hashtable_size { 1024 },
                 crispy::lru_capacity { 4096 },
                 /* atlasDirectMapping */ false,
                 Decorator::Underline,
                 Decorator::Underline)
    {
        RendererTest::seedMetrics(renderer, seededMetrics());
    }

    /// Wires the mock render target (and texture atlas) into the renderer.
    ///
    /// Done lazily so geometry/state-machine tests that don't need a render target avoid the atlas
    /// setup entirely.
    void attachRenderTarget()
    {
        renderer.setRenderTarget(renderTarget);
        // setRenderTarget() does not stage a reconfiguration; the seeded metrics remain in effect.
    }
};

/// Registers the BDF mock font used by the reconfiguration tests with the global mock locator.
void configureMockFont()
{
    auto registry = std::vector<text::font_description_and_source> {
        { .description = text::font_description::parse("regular"),
          .source = text::font_path { .value = std::filesystem::absolute(testFontPath).string() } }
    };
    text::mock_font_locator::configure(std::move(registry));
}

} // namespace

TEST_CASE("Renderer.reconfig.geometry_published_synchronously", "[renderer]")
{
    // A geometry change (page size / margin / pixel size) does not need the text shaper, so the
    // requested metrics are published synchronously and observable via gridMetrics() immediately —
    // before any render-thread apply. This preserves the contract that applyResize()/resizeScreen()
    // see the new geometry right away.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto const oldCellSize = renderer.gridMetrics().cellSize;
    auto const newPageSize = PageSize { LineCount(30), ColumnCount(100) };
    auto const newMargin = vtrasterizer::PageMargin { .left = 7, .top = 11, .bottom = 13 };
    auto const newPixelSize = vtbackend::ImageSize { Width(1280), Height(720) };

    renderer.applyResize(newPixelSize, newPageSize, newMargin);

    // Published (UI-visible) metrics reflect the request synchronously...
    CHECK(renderer.gridMetrics().pageSize == newPageSize);
    CHECK(renderer.gridMetrics().pageMargin.left == newMargin.left);
    CHECK(renderer.gridMetrics().pageMargin.top == newMargin.top);
    // ...but the cell size is unchanged by a pure geometry resize.
    CHECK(renderer.gridMetrics().cellSize == oldCellSize);

    // The request is staged for the render thread until applied.
    CHECK(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
}

TEST_CASE("Renderer.reconfig.geometry_not_applied_to_live_until_render", "[renderer]")
{
    // The live grid metrics (read by the renderables) are only mutated when the render thread applies
    // the staged reconfiguration — not when the UI thread requests it.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto const newPageSize = PageSize { LineCount(30), ColumnCount(100) };
    auto const newMargin = vtrasterizer::PageMargin { .left = 7, .top = 11, .bottom = 13 };
    auto const newPixelSize = vtbackend::ImageSize { Width(1280), Height(720) };

    renderer.applyResize(newPixelSize, newPageSize, newMargin);

    // Before apply: the *live* metrics still hold the original page size, even though the published
    // (UI-visible) metrics already reflect the request.
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).pageSize != newPageSize);
    CHECK(renderer.gridMetrics().pageSize == newPageSize);

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // After apply: live metrics updated, nothing left pending.
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).pageSize == newPageSize);
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).pageMargin.left == newMargin.left);
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
}

TEST_CASE("Renderer.reconfig.geometry_resizes_render_target_on_apply", "[renderer]")
{
    // With a render target attached, applying a geometry reconfiguration must resize the render
    // target — and only on the render thread (during applyPendingReconfig), not when requested.
    configureMockFont();
    ReconfigFixture fixture;
    fixture.attachRenderTarget();
    auto& renderer = fixture.renderer;

    auto const newPageSize = PageSize { LineCount(30), ColumnCount(100) };
    auto const newMargin = vtrasterizer::PageMargin { .left = 7, .top = 11, .bottom = 13 };
    auto const newPixelSize = vtbackend::ImageSize { Width(1280), Height(720) };

    renderer.applyResize(newPixelSize, newPageSize, newMargin);
    CHECK(fixture.renderTarget.renderSize() != newPixelSize); // not yet applied

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    CHECK(fixture.renderTarget.renderSize() == newPixelSize); // applied on the render thread
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
}

TEST_CASE("Renderer.reconfig.font_size_change_is_deferred", "[renderer]")
{
    // A font-size change needs the (non-thread-safe) text shaper, so it is fully deferred: it only
    // stages a request and touches neither the live nor the published metrics until the render thread
    // applies it. This guarantees the shaper is never touched concurrently with rendering.
    //
    // NOTE: the minimal BDF test font is a fixed-size bitmap (SIZE 9), so the staged size must be 9pt
    // (any other size fails to load). This asserts the deferral/state machine, not cell-size magnitude
    // (which a real scalable font would change).
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto const publishedBefore = renderer.gridMetrics();
    auto const liveBefore = vtrasterizer::RendererTest::liveGridMetrics(renderer);

    REQUIRE(renderer.setFontSize(text::font_size { 9.0 }));

    // Deferred: nothing observable changed yet, but a reconfiguration is staged.
    CHECK(renderer.gridMetrics().cellSize == publishedBefore.cellSize);
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).cellSize == liveBefore.cellSize);
    CHECK(vtrasterizer::RendererTest::hasPendingReconfig(renderer));

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // After apply: the request is consumed; the font descriptions now carry the requested size.
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
}

TEST_CASE("Renderer.reconfig.font_apply_signals_display_to_resize", "[renderer]")
{
    // Because a font change is applied a frame late, the display must recompute the page size against
    // the new cell size afterwards. applyPendingReconfig() sets a one-shot flag the display consumes.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    // No font change yet → nothing to signal.
    CHECK_FALSE(renderer.consumeFontReconfigApplied());

    REQUIRE(renderer.setFontSize(text::font_size { 9.0 }));
    // Staging alone does not raise the flag; only the render-thread apply does.
    CHECK_FALSE(renderer.consumeFontReconfigApplied());

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // The flag is now set, and consuming it clears it (one-shot).
    CHECK(renderer.consumeFontReconfigApplied());
    CHECK_FALSE(renderer.consumeFontReconfigApplied());
}

TEST_CASE("Renderer.reconfig.geometry_only_does_not_signal_font_resize", "[renderer]")
{
    // A geometry-only reconfiguration changes no cell size, so it must NOT raise the font-applied
    // signal (which would cause a redundant page-size recompute on the display).
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    renderer.applyResize(vtbackend::ImageSize { Width(1280), Height(720) },
                         PageSize { LineCount(30), ColumnCount(100) },
                         vtrasterizer::PageMargin { .left = 1, .top = 2, .bottom = 3 });
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    CHECK_FALSE(renderer.consumeFontReconfigApplied());
}

TEST_CASE("Renderer.reconfig.noop_font_apply_does_not_signal_font_resize", "[renderer]")
{
    // A font apply that does not actually change the cell size (e.g. re-applying identical font
    // descriptions, or a config reload that re-stages the same fonts) must NOT raise the font-applied
    // signal — otherwise the display performs a redundant terminal-locked page-size recompute for a
    // cell size that did not change. This is the symmetric counterpart to
    // geometry_only_does_not_signal_font_resize.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    // First apply establishes a stable cell size from the mock font.
    renderer.setFonts(fixture.fontDescriptions);
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    (void) renderer.consumeFontReconfigApplied(); // drain whatever the first apply signalled

    // Re-stage the identical descriptions: applyFontDescriptions() early-returns, the cell size is
    // unchanged, so no font reconfig must be signalled.
    renderer.setFonts(fixture.fontDescriptions);
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    CHECK_FALSE(renderer.consumeFontReconfigApplied());
}

TEST_CASE("Renderer.reconfig.published_font_descriptions_track_applied_state", "[renderer]")
{
    // fontDescriptions() returns a mutex-guarded snapshot published by the render thread, never the live
    // (render-thread-owned) field. A staged change must therefore become visible via fontDescriptions()
    // only after the render-thread apply, not when it is staged.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto changed = fixture.fontDescriptions;
    changed.maxFallbackCount += 1;

    renderer.setFonts(changed);
    // Still the original until the render thread applies.
    CHECK(renderer.fontDescriptions().maxFallbackCount == fixture.fontDescriptions.maxFallbackCount);

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // Published snapshot now reflects the applied descriptions.
    CHECK(renderer.fontDescriptions().maxFallbackCount == changed.maxFallbackCount);
}

TEST_CASE("Renderer.reconfig.set_fonts_is_deferred", "[renderer]")
{
    // A full font-descriptions change (setFonts) is deferred to the render thread just like a size
    // change, because it reconfigures the non-thread-safe text shaper and rebuilds the atlas.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    // Request a changed descriptions (keep size 9 so the BDF still loads; tweak the fallback limit).
    auto changed = fixture.fontDescriptions;
    changed.maxFallbackCount += 1;

    renderer.setFonts(changed);

    // Deferred: a reconfiguration is staged, but the live font descriptions are unchanged.
    CHECK(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(renderer.fontDescriptions().maxFallbackCount == fixture.fontDescriptions.maxFallbackCount);

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // After the render-thread apply: the change took effect and nothing is left pending.
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(renderer.fontDescriptions().maxFallbackCount == changed.maxFallbackCount);
}

TEST_CASE("Renderer.reconfig.font_load_failure_keeps_previous_font", "[renderer]")
{
    // A font-descriptions change whose regular font cannot be loaded (e.g. the family was uninstalled or
    // a locator failure) must NOT abort the process: applyPendingReconfig() catches the failure and keeps
    // the previously loaded font. Regression test for the crash where loadFontKeys() returned an invalid
    // FontKeys without throwing, so the catch never fired and metrics(invalid_key) aborted.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto const descriptionsBefore = vtrasterizer::RendererTest::liveFontDescriptions(renderer);

    // Make every subsequent font load fail: with an empty registry the mock locator returns no source,
    // so load_font() yields nullopt and loadFontKeys() throws (the regular font cannot be loaded).
    text::mock_font_locator::configure({});

    auto failing = fixture.fontDescriptions;
    failing.regular = text::font_description::parse("this-font-does-not-exist");
    failing.maxFallbackCount += 1; // ensure the descriptions differ so the change is not a no-op
    renderer.setFonts(failing);

    // Must not abort. The apply swallows the load failure and keeps the previous font.
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // The request is consumed (no infinite retry loop), but the live descriptions still hold the
    // previously loaded font — the failed change was not committed.
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(vtrasterizer::RendererTest::liveFontDescriptions(renderer).regular == descriptionsBefore.regular);

    // A failed apply must not signal the display to resize against half-applied metrics.
    CHECK_FALSE(renderer.consumeFontReconfigApplied());
}

TEST_CASE("Renderer.reconfig.setup_apply_returns_consumed_font_reconfig_flag", "[renderer]")
{
    // applyStagedReconfigDuringSetup() applies any staged reconfiguration AND consumes the "font reconfig
    // applied" one-shot signal under _applyMutex, returning it. Consuming it there (rather than via a
    // separate consumeFontReconfigApplied() after the lock is released) is what prevents the GUI thread
    // and the render thread from both consuming the same one-shot signal.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    // A size-only change that alters the cell size: the setup apply reports the font reconfig and the
    // flag is already consumed, so nothing is left for a later consumer.
    REQUIRE(renderer.setFontSize(text::font_size { 9.0 }));
    auto const fontReconfigApplied = renderer.applyStagedReconfigDuringSetup();
    CHECK_FALSE(renderer.consumeFontReconfigApplied()); // consumed inside the setup apply, not here
    // The BDF mock font is fixed-size, so the cell size may or may not change vs the seeded metrics; the
    // contract under test is only that the setup apply, not a later consumer, owns the one-shot signal.
    (void) fontReconfigApplied;

    // A geometry-only change changes no cell size, so the setup apply reports no font reconfig.
    renderer.applyResize(vtbackend::ImageSize { Width(1280), Height(720) },
                         PageSize { LineCount(30), ColumnCount(100) },
                         vtrasterizer::PageMargin { .left = 1, .top = 2, .bottom = 3 });
    CHECK_FALSE(renderer.applyStagedReconfigDuringSetup());
}

TEST_CASE("Renderer.reconfig.set_font_size_folds_into_pending_set_fonts", "[renderer]")
{
    // If a full font-descriptions change is already staged and a font-size change arrives before the
    // render thread applies, the new size must fold into the staged descriptions (most recent wins)
    // rather than being dropped.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto changed = fixture.fontDescriptions;
    changed.maxFallbackCount += 1;
    changed.size = text::font_size { 9.0 };

    renderer.setFonts(changed);
    REQUIRE(renderer.setFontSize(text::font_size { 9.0 })); // same valid BDF size, folds into pending

    // Both the descriptions change and the (folded) size apply together in one pass.
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(renderer.fontDescriptions().maxFallbackCount == changed.maxFallbackCount);
    CHECK(renderer.fontDescriptions().size.pt == Catch::Approx(9.0));
}

TEST_CASE("Renderer.reconfig.geometry_preserved_across_font_change", "[renderer]")
{
    // When a geometry change and a font change are both staged before a single apply, the page size
    // and margin set by the geometry request must survive the font-driven grid-metrics rebuild
    // (loadGridMetrics() otherwise resets page geometry).
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto const newPageSize = PageSize { LineCount(40), ColumnCount(120) };
    auto const newMargin = vtrasterizer::PageMargin { .left = 5, .top = 9, .bottom = 3 };
    auto const newPixelSize = vtbackend::ImageSize { Width(1600), Height(900) };

    renderer.applyResize(newPixelSize, newPageSize, newMargin);
    REQUIRE(renderer.setFontSize(text::font_size { 9.0 })); // BDF test font's only available size.

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    auto const& live = vtrasterizer::RendererTest::liveGridMetrics(renderer);
    CHECK(live.pageSize == newPageSize);
    CHECK(live.pageMargin.left == newMargin.left);
    CHECK(live.pageMargin.top == newMargin.top);
    CHECK(live.pageMargin.bottom == newMargin.bottom);
}

TEST_CASE("Renderer.reconfig.requests_coalesce_until_applied", "[renderer]")
{
    // Rapidly issuing many reconfiguration requests (the user-reported crash repro: aggressively
    // resizing the window) must coalesce into a single pending request that, once applied, leaves no
    // residual pending work and reflects the *last* requested geometry.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    // Oscillate the geometry many times before the render thread gets a chance to apply.
    for (auto const lines: { 20, 45, 12, 60, 33 })
    {
        auto const pageSize = PageSize { LineCount(lines), ColumnCount(lines * 2) };
        renderer.setPageSize(pageSize);
        CHECK(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    }

    auto const finalPageSize = PageSize { LineCount(50), ColumnCount(140) };
    auto const finalMargin = vtrasterizer::PageMargin { .left = 1, .top = 2, .bottom = 3 };
    renderer.applyResize(vtbackend::ImageSize { Width(1920), Height(1080) }, finalPageSize, finalMargin);

    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // One apply drains everything; the last requested geometry wins.
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).pageSize == finalPageSize);
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).pageMargin.left == finalMargin.left);

    // A subsequent apply with nothing staged is a no-op (idempotent).
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).pageSize == finalPageSize);
}

TEST_CASE("Renderer.reconfig.concurrent_requests_and_apply", "[renderer]")
{
    // Reproduces the actual threading model: a "render thread" repeatedly applies staged
    // reconfigurations (as renderImpl() does) while a "UI thread" concurrently requests geometry and
    // font-size changes (as wheelEvent()/resize handlers do). Run under ThreadSanitizer this asserts
    // that _reconfigMutex fully covers the shared state — no data race on _gridMetrics/_publishedMetrics
    // /_pendingReconfig. Without a sanitizer it still exercises the lock for crashes/UB and converges.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    constexpr int Iterations = 2000;
    std::atomic<bool> applierStop { false };

    // Render-thread role: drain pending reconfigurations as fast as possible.
    std::thread applier([&] {
        while (!applierStop.load(std::memory_order_relaxed))
            vtrasterizer::RendererTest::applyPendingReconfig(renderer);
        // Final drain so the last staged request is guaranteed applied.
        vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    });

    // UI-thread role: issue interleaved geometry and font requests.
    for (auto const i: std::views::iota(0, Iterations))
    {
        auto const lines = 10 + (i % 50);
        renderer.applyResize(vtbackend::ImageSize { Width(640 + i % 100), Height(480 + i % 100) },
                             PageSize { LineCount(lines), ColumnCount(lines * 2) },
                             vtrasterizer::PageMargin { .left = i % 5, .top = i % 7, .bottom = i % 3 });
        if (i % 3 == 0)
            renderer.setPageSize(PageSize { LineCount(lines), ColumnCount(lines * 2) });
        if (i % 11 == 0)
            (void) renderer.setFontSize(text::font_size { 9.0 }); // BDF font's only valid size.
        if (i % 17 == 0)
        {
            // Exercise the full font-descriptions reconfiguration path concurrently as well.
            auto fonts = fixture.fontDescriptions;
            fonts.maxFallbackCount = 1 + (i % 4);
            renderer.setFonts(fonts);
        }
        // Reading the published metrics and consuming the font-applied signal from the UI thread
        // (as the display does after each frame) must also be race-free.
        (void) renderer.gridMetrics();
        (void) renderer.consumeFontReconfigApplied();
    }

    applierStop.store(true, std::memory_order_relaxed);
    applier.join();

    // Drain anything still staged and assert a consistent final state.
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
    CHECK(vtrasterizer::RendererTest::liveGridMetrics(renderer).cellSize == renderer.gridMetrics().cellSize);
}

TEST_CASE("Renderer.reconfig.set_font_dpi_does_not_clobber_staged_fonts", "[renderer]")
{
    // A DPI-only change (setFontDPI) must merge into an already-staged full font-descriptions change
    // rather than rebuilding the request from the live descriptions — otherwise a concurrent font
    // change (e.g. a config reload that staged a new family/fallback) would be silently dropped.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    // Stage a full font-descriptions change first (simulating a config reload).
    auto changed = fixture.fontDescriptions;
    changed.maxFallbackCount += 7; // A distinguishing, non-DPI field.
    renderer.setFonts(changed);

    // Now a DPI change arrives in the same inter-frame gap.
    auto const newDpi = DPI { 144, 144 };
    renderer.setFontDPI(newDpi);

    // The staged descriptions must retain the new family/fallback AND pick up the new DPI.
    auto const staged = vtrasterizer::RendererTest::pendingFontDescriptions(renderer);
    REQUIRE(staged.has_value());
    CHECK(staged->maxFallbackCount == changed.maxFallbackCount); // not clobbered
    CHECK(staged->dpi == newDpi);                                // DPI applied
}

TEST_CASE("Renderer.reconfig.set_font_dpi_folds_staged_size", "[renderer]")
{
    // If only a size-only change is staged when a DPI change arrives, promoting it to a full
    // descriptions change must fold the staged size in (applyPendingReconfig applies descriptions XOR
    // size), not drop it.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    REQUIRE(renderer.setFontSize(text::font_size { 9.0 })); // BDF font's only valid size.
    REQUIRE(vtrasterizer::RendererTest::pendingFontSize(renderer).has_value());

    renderer.setFontDPI(DPI { 144, 144 });

    // The size-only staging is now folded into the descriptions and cleared.
    auto const staged = vtrasterizer::RendererTest::pendingFontDescriptions(renderer);
    REQUIRE(staged.has_value());
    CHECK(staged->size.pt == Catch::Approx(9.0));
    CHECK_FALSE(vtrasterizer::RendererTest::pendingFontSize(renderer).has_value());
}

TEST_CASE("Renderer.reconfig.set_font_dpi_noop_when_unchanged", "[renderer]")
{
    // Setting the same DPI that is already live stages nothing (avoids spurious reconfigurations).
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    renderer.setFontDPI(renderer.fontDescriptions().dpi);
    CHECK_FALSE(vtrasterizer::RendererTest::hasPendingReconfig(renderer));
}

TEST_CASE("Renderer.reconfig.published_cell_size_tracks_grid_metrics", "[renderer]")
{
    // publishedCellSize() is the lock-free mirror of gridMetrics().cellSize; the two must agree after
    // construction, after a seed, and after a render-thread apply.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    CHECK(renderer.publishedCellSize() == renderer.gridMetrics().cellSize);

    // A geometry-only change does not alter the cell size; the mirror stays consistent.
    renderer.applyResize(vtbackend::ImageSize { Width(800), Height(600) },
                         PageSize { LineCount(25), ColumnCount(80) },
                         vtrasterizer::PageMargin { .left = 0, .top = 0, .bottom = 0 });
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    CHECK(renderer.publishedCellSize() == renderer.gridMetrics().cellSize);

    // After a font apply, the mirror still equals the published cell size.
    REQUIRE(renderer.setFontSize(text::font_size { 9.0 }));
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);
    CHECK(renderer.publishedCellSize() == renderer.gridMetrics().cellSize);
}

TEST_CASE("Renderer.reconfig.page_geometry_preserved_across_dpi_change", "[renderer]")
{
    // Regression test for decoupling the font rebuild from page geometry: updateFontMetrics() now
    // updates only the font-derived fields in place, so a font/DPI change must not reset the page
    // size/margin previously established by a geometry request.
    configureMockFont();
    ReconfigFixture fixture;
    auto& renderer = fixture.renderer;

    auto const newPageSize = PageSize { LineCount(33), ColumnCount(111) };
    auto const newMargin = vtrasterizer::PageMargin { .left = 4, .top = 8, .bottom = 6 };
    renderer.applyResize(vtbackend::ImageSize { Width(1111), Height(666) }, newPageSize, newMargin);
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    // Re-stage the same (valid) font size, which exercises updateFontMetrics() in place.
    REQUIRE(renderer.setFontSize(text::font_size { 9.0 }));
    vtrasterizer::RendererTest::applyPendingReconfig(renderer);

    auto const& live = vtrasterizer::RendererTest::liveGridMetrics(renderer);
    CHECK(live.pageSize == newPageSize);
    CHECK(live.pageMargin.left == newMargin.left);
    CHECK(live.pageMargin.top == newMargin.top);
    CHECK(live.pageMargin.bottom == newMargin.bottom);
}

// {{{ issue #1939: a run that falls back must stay on the cell grid

namespace
{

constexpr auto Ornament = char32_t { 0x276F };     ///< The prompt ornament from issue #1939.
constexpr auto NoBreakSpace = char32_t { 0x00A0 }; ///< Follows it, and does not break the run.
constexpr auto CellWidth = 10;

/// Renders each of @p cells as its own grid cell of one text group, and returns the X coordinate every
/// glyph was drawn at.
[[nodiscard]] std::vector<int> renderedGlyphPositions(TextRenderer& renderer,
                                                      MockRenderTarget& renderTarget,
                                                      std::vector<std::u32string> const& cells)
{
    renderTarget.getMockBackend().renderCommands.clear();
    renderer.beginFrame();

    for (auto const i: std::views::iota(size_t { 0 }, cells.size()))
    {
        auto cell = vtbackend::RenderCell {};
        cell.groupStart = i == 0;
        cell.position =
            vtbackend::CellLocation { .line = LineOffset(0), .column = ColumnOffset(static_cast<int>(i)) };
        cell.codepoints = cells[i];
        cell.attributes.flags = vtbackend::CellFlags {};
        cell.attributes.foregroundColor = vtbackend::RGBColor(0xFF, 0xFF, 0xFF);
        renderer.renderCell(cell);
    }

    renderer.endFrame();

    auto positions = std::vector<int> {};
    for (auto const& command: renderTarget.getMockBackend().renderCommands)
        positions.emplace_back(command.x.value);
    return positions;
}

} // namespace

TEST_CASE("TextRenderer.fallback_run_stays_on_the_cell_grid", "[renderer][fallback]")
{
    // The primary is a monospaced terminal font with the letters but no prompt ornament: SF Mono.
    auto primary = text::test::bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 } } };

    // The fallback is proportional and covers everything the primary is missing -- DejaVu Sans. Its
    // non-breaking space advances a mere 3px against a 10px cell, which is what used to round away to
    // nothing and drag the rest of the run one cell to the left.
    auto fallback = text::test::bdf_font {
        "fallback", false, { { Ornament, 12 }, { NoBreakSpace, 3 }, { U'A', 7 }, { U'B', 7 } }
    };

    auto description = text::font_description {};
    description.familyName = "primary";
    description.spacing = text::font_spacing::mono;

    auto fallbackDescription = description;
    fallbackDescription.familyName = "fallback";

    mock_font_locator::configure({
        { .description = description, .source = primary.source() },
        { .description = fallbackDescription, .source = fallback.source() },
    });
    auto const _ = crispy::finally { [] { mock_font_locator::configure({}); } };

    auto locator = mock_font_locator {};
    auto textShaper = open_shaper { text::test::bdf_font::Dpi, locator };

    auto const fontKey = textShaper.load_font(description, text::test::bdf_font::Size);
    REQUIRE(fontKey.has_value());

    auto const gridMetrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                           .cellSize = ImageSize { Width(CellWidth), Height(20) },
                                           .baseline = 15,
                                           .underline = { .position = 17, .thickness = 1 } };

    auto fontDescriptions = FontDescriptions {};
    fontDescriptions.dpi = text::test::bdf_font::Dpi;
    fontDescriptions.size = text::test::bdf_font::Size;
    fontDescriptions.textShapingEngine = TextShapingEngine::OpenShaper;

    auto const fontKeys = FontKeys {
        .regular = *fontKey, .bold = *fontKey, .italic = *fontKey, .boldItalic = *fontKey, .emoji = *fontKey
    };

    MockTextRendererEvents events;
    auto renderer = TextRenderer { gridMetrics, textShaper, fontDescriptions, fontKeys, events };

    MockRenderTarget renderTarget;
    vtrasterizer::atlas::DirectMappingAllocator<vtrasterizer::RenderTileAttributes> allocator;
    renderer.setRenderTarget(renderTarget, allocator);

    // The tile is one CELL, exactly as Renderer::configureTextureAtlas sets it in production. A
    // roomier tile (this was 256x256, ~12x the cell) makes the atlas' central invariant
    // untestable: tile origins are spaced one tile apart, so an oversized bitmap overwrites its
    // neighbour, and nothing here could ever produce one large enough to notice.
    auto const atlasProperties =
        vtrasterizer::atlas::AtlasProperties { .format = vtrasterizer::atlas::Format::Red,
                                               .tileSize = gridMetrics.cellSize,
                                               .hashCount = { 1024 },
                                               .tileCount = { 4096 },
                                               .directMappingCount = 128 };
    auto textureAtlas = TextureAtlas(renderTarget.getMockBackend(), atlasProperties);
    renderer.setTextureAtlas(textureAtlas);

    SECTION("a narrow fallback advance does not shift the rest of the run left")
    {
        // The prompt as Claude Code writes it: the ornament, a non-breaking space, then the first word.
        // A plain space would end the text group; a non-breaking space does not, which is why all four
        // cells are shaped as one run and why only the first word was ever affected.
        auto const positions = renderedGlyphPositions(
            renderer,
            renderTarget,
            { std::u32string { Ornament }, std::u32string { NoBreakSpace }, U"A", U"B" });

        REQUIRE(positions.size() == 4);

        // Every glyph sits on its own cell boundary. Before the fix the non-breaking space advanced the
        // pen by nothing at all, and this came back as { 0, 10, 10, 20 } -- the letters a cell too far
        // left, drawn over the space.
        CHECK(positions[0] == 0);
        CHECK(positions[1] == 1 * CellWidth);
        CHECK(positions[2] == 2 * CellWidth);
        CHECK(positions[3] == 3 * CellWidth);
    }

    SECTION("a run the primary font covers is unaffected")
    {
        auto const positions = renderedGlyphPositions(renderer, renderTarget, { U"A", U"B" });

        REQUIRE(positions.size() == 2);
        CHECK(positions[0] == 0);
        CHECK(positions[1] == 1 * CellWidth);
    }
}

TEST_CASE("TextRenderer.a_scaled_block_draws_one_cell_sized_tile_per_band", "[renderer][textsizing]")
{
    // The atlas is a fixed GRID: TextureAtlas spaces tile origins exactly tileSize apart, so a
    // bitmap taller than one cell is not "too big" -- it is a write into the tile below, silently
    // replacing an unrelated glyph's pixels. That is why the damage from this bug shows up as
    // WRONG CHARACTERS somewhere else on screen rather than as a malformed scaled glyph.
    //
    // An `OSC 66` block is `scale` cells tall, but it must still reach the atlas as cell-sized
    // tiles -- kitty's extract_cell_region does exactly this, and never stores a sprite larger
    // than one unscaled cell.
    //
    // NOTE on what this test can and cannot see: BDF is a fixed-size bitmap format (@see
    // bdf_test_font), so FreeType hands back the same raster whatever size is asked for. The
    // oversize half of this test is therefore VACUOUS here -- it cannot produce a too-large tile
    // to reject. It is the banding half below that gates, and the oversize bound is gated for
    // real by MockAtlasBackend across every renderer test and by the [display] OSC 66 test, both
    // of which run against a scalable font.
    auto font = text::test::bdf_font { "primary", true, { { U'A', 8 }, { U'B', 8 } } };

    auto description = text::font_description {};
    description.familyName = "primary";
    description.spacing = text::font_spacing::mono;

    mock_font_locator::configure({ { .description = description, .source = font.source() } });
    auto const _ = crispy::finally { [] { mock_font_locator::configure({}); } };

    auto locator = mock_font_locator {};
    auto textShaper = open_shaper { text::test::bdf_font::Dpi, locator };

    auto const fontKey = textShaper.load_font(description, text::test::bdf_font::Size);
    REQUIRE(fontKey.has_value());

    // `baseline` is the DESCENT -- Renderer computes it as lineHeight - ascender -- so it is a few
    // pixels, not most of the cell. The neighbouring tests carry 15 against a 20px cell, which
    // leaves an ascender no glyph fits under and would put a scaled block's baseline above its own
    // canvas.
    auto const gridMetrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                           .cellSize = ImageSize { Width(CellWidth), Height(20) },
                                           .baseline = 4,
                                           .underline = { .position = 2, .thickness = 1 } };

    auto fontDescriptions = FontDescriptions {};
    fontDescriptions.dpi = text::test::bdf_font::Dpi;
    fontDescriptions.size = text::test::bdf_font::Size;
    fontDescriptions.textShapingEngine = TextShapingEngine::OpenShaper;

    auto const fontKeys = FontKeys {
        .regular = *fontKey, .bold = *fontKey, .italic = *fontKey, .boldItalic = *fontKey, .emoji = *fontKey
    };

    MockTextRendererEvents events;

    // Only Stretch is exercised here. Rerasterize asks the shaper for the face at a larger size, and
    // BDF is a fixed-size bitmap format (@see bdf_test_font) -- it hands back the identical raster,
    // so a block could not put ink below its first row however correct the code was. Asserting it
    // anyway would be asserting a property of the test font. Rerasterize is gated end to end by the
    // [display] OSC 66 test, which runs against a real scalable font.
    auto const method = GlyphScalingMethod::Stretch;
    INFO("scaling method: " << nameOf(method));
    auto const& scaler = glyphScalerFor(method);

    auto renderer = TextRenderer { gridMetrics, textShaper, fontDescriptions, fontKeys, events, scaler };

    MockRenderTarget renderTarget;
    vtrasterizer::atlas::DirectMappingAllocator<vtrasterizer::RenderTileAttributes> allocator;
    renderer.setRenderTarget(renderTarget, allocator);

    auto const atlasProperties =
        vtrasterizer::atlas::AtlasProperties { .format = vtrasterizer::atlas::Format::Red,
                                               .tileSize = gridMetrics.cellSize,
                                               .hashCount = { 1024 },
                                               .tileCount = { 4096 },
                                               .directMappingCount = 128 };
    auto textureAtlas = TextureAtlas(renderTarget.getMockBackend(), atlasProperties);
    renderer.setTextureAtlas(textureAtlas);

    auto const scale = GENERATE(uint8_t { 1 }, uint8_t { 2 }, uint8_t { 3 });
    INFO("scale: " << static_cast<int>(scale));

    // Each band is its own row, and each row is one group -- exactly how RenderBufferBuilder feeds
    // a block: the head row carries the text, and every row below it repeats the head's cluster
    // with its own band index. groupStart/groupEnd bracket each row, or the grouper carries the
    // pen across a line boundary.
    auto& backend = renderTarget.getMockBackend();

    // Each band is its own row, and each row is one group -- exactly how RenderBufferBuilder feeds
    // a block: the head row carries the text, and every row below it repeats the head's cluster
    // with its own band index. groupStart/groupEnd bracket each row, or the grouper carries the
    // pen across a line boundary.
    //
    // Rendered one band at a time, and the uploads each band produced are inspected before the next
    // runs. Counting tiles DRAWN would prove nothing -- a blank tile is still a draw, which is
    // exactly how a first version of this test passed against the very bug it was written for.
    auto bandsWithInk = std::set<int> {};

    renderer.beginFrame();
    for (auto const band: std::views::iota(0, static_cast<int>(scale)))
    {
        auto const uploadsBefore = backend.uploadCommands.size();

        auto cell = vtbackend::RenderCell {};
        cell.groupStart = true;
        cell.groupEnd = true;
        cell.position = vtbackend::CellLocation { .line = LineOffset(band), .column = ColumnOffset(0) };
        cell.codepoints = U"A";
        cell.attributes.foregroundColor = vtbackend::RGBColor(0xFF, 0xFF, 0xFF);
        cell.sizing.scale.scale = scale;
        cell.sizing.band = static_cast<uint8_t>(band);
        // What the backend claimed: a one-cell-wide glyph at scale s spans s columns.
        cell.sizing.columns = scale;
        renderer.renderCell(cell);
        renderer.endFrame();
        renderer.beginFrame();

        auto const inked = std::ranges::any_of(
            backend.uploadCommands | std::views::drop(uploadsBefore), [](auto const& upload) {
                return std::ranges::any_of(upload.bitmap, [](uint8_t v) { return v != 0; });
            });
        if (inked)
            bandsWithInk.insert(band);
    }
    renderer.endFrame();

    // The assertion the MockAtlasBackend also makes as each upload arrives; named here so the
    // failure reads as what it is rather than as an anonymous CHECK deep in the harness.
    INFO("oversized uploads: " << backend.oversizedUploads.size() << " of " << backend.uploadCommands.size());
    CHECK(backend.oversizedUploads.empty());

    // The banding contract: a block's ink must reach its LAST row and must not be confined to its
    // head. Drawing only the head row is how a block collapsed to a sliver of the glyph's top edge.
    //
    // Not "every band has ink": a glyph's own bitmap carries blank padding rows (this font's 'A'
    // starts two rows down), so an interior or leading band can be legitimately empty. Asserting
    // otherwise would be asserting the shape of the test font's letter.
    INFO("bands carrying ink: " << bandsWithInk.size() << " of " << static_cast<int>(scale));
    CHECK(bandsWithInk.contains(static_cast<int>(scale) - 1));
    CHECK(bandsWithInk.size() >= std::min<size_t>(2, scale));
}

// }}}

int main(int argc, char* argv[])
{
    crispy::suppressWindowsDialogs();

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
