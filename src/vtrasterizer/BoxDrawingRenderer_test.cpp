#include <vtbackend/core/Color.hpp>
#include <vtbackend/shell/Folding.hpp>

#include <vtrasterizer/BoxDrawingRenderer.hpp>
#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RendererTestHelpers.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <ranges>

using namespace vtrasterizer;
using namespace vtbackend;

// Forward declare the test class to match the friend declaration
namespace vtrasterizer
{
class BoxDrawingRendererTest
{
  public:
    static std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                         ImageSize size,
                                                         int lineThickness,
                                                         size_t supersampling = 1)
    {
        return BoxDrawingRenderer::buildBoxElements(codepoint, size, lineThickness, supersampling);
    }

    /// Builds elements via the non-static buildElements() path using a minimal GridMetrics.
    static std::optional<atlas::Buffer> buildElements(char32_t codepoint, ImageSize size, int lineThickness)
    {
        auto gridMetrics = GridMetrics { .pageSize = { LineCount(24), ColumnCount(80) },
                                         .cellSize = size,
                                         .baseline = 0,
                                         .underline = { .position = 1, .thickness = lineThickness } };
        auto renderer = BoxDrawingRenderer(gridMetrics);
        return renderer.buildElements(codepoint, size, lineThickness);
    }
};
} // namespace vtrasterizer

namespace
{
struct TestTileData
{
    vtbackend::ImageSize bitmapSize;
    vtrasterizer::atlas::Format bitmapFormat;
    std::vector<uint8_t> bitmap;
};
} // namespace

TEST_CASE("Powerline inverse dividers complement the solid dividers", "[boxdrawing][powerline]")
{
    for (auto const size: { ImageSize { Width(11), Height(21) },
                            ImageSize { Width(16), Height(32) },
                            ImageSize { Width(17), Height(33) } })
        for (auto const solid: { char32_t { 0xE0B0 }, char32_t { 0xE0B2 } })
        {
            auto const inverse = solid == 0xE0B0 ? char32_t { 0xE0D7 } : char32_t { 0xE0D6 };
            CAPTURE(unbox<int>(size.width), unbox<int>(size.height), static_cast<unsigned>(solid));
            REQUIRE(BoxDrawingRenderer::renderable(inverse));
            auto const divider = BoxDrawingRendererTest::buildElements(solid, size, 1);
            auto const cutout = BoxDrawingRendererTest::buildElements(inverse, size, 1);
            REQUIRE(divider);
            REQUIRE(cutout);
            REQUIRE(divider->size() == cutout->size());
            // Supersampling truncates each alpha average independently, losing at most one unit.
            CHECK(std::ranges::all_of(std::views::iota(size_t { 0 }, divider->size()), [&](auto index) {
                auto const coverage = static_cast<int>((*divider)[index]) + (*cutout)[index];
                return coverage >= 254 && coverage <= 255;
            }));
        }
}

/// Counts the number of non-zero pixels in a buffer.
static size_t countLitPixels(atlas::Buffer const& buffer)
{
    return static_cast<size_t>(std::ranges::count_if(buffer, [](uint8_t v) { return v > 0; }));
}

/// Computes a horizontal mirror of a buffer.
static atlas::Buffer horizontalMirror(atlas::Buffer const& buffer, ImageSize size)
{
    auto const w = unbox<size_t>(size.width);
    auto const h = unbox<size_t>(size.height);
    auto mirrored = atlas::Buffer(buffer.size());
    for (auto y: std::views::iota(0zu, h))
        for (auto x: std::views::iota(0zu, w))
            mirrored[(y * w) + x] = buffer[(y * w) + (w - 1 - x)];
    return mirrored;
}

/// Computes a vertical mirror of a buffer.
static atlas::Buffer verticalMirror(atlas::Buffer const& buffer, ImageSize size)
{
    auto const w = unbox<size_t>(size.width);
    auto const h = unbox<size_t>(size.height);
    auto mirrored = atlas::Buffer(buffer.size());
    for (auto y: std::views::iota(0zu, h))
        for (auto x: std::views::iota(0zu, w))
            mirrored[(y * w) + x] = buffer[((h - 1 - y) * w) + x];
    return mirrored;
}

TEST_CASE("Powerline basic separators fit their tiles and preserve diagonal aliases", "[powerline]")
{
    for (auto const size: { ImageSize { Width(11), Height(21) }, ImageSize { Width(16), Height(32) } })
    {
        for (auto const codepoint:
             { U'\uE0B1', U'\uE0B3', U'\uE0B8', U'\uE0B9', U'\uE0BB', U'\uE0BD', U'\uE0BF' })
        {
            REQUIRE(BoxDrawingRenderer::renderable(codepoint));
            auto const tile = BoxDrawingRendererTest::buildElements(codepoint, size, 1);
            REQUIRE(tile);
            CHECK(tile->size() == size.area());
            CHECK(countLitPixels(*tile) > 0);
            CHECK(countLitPixels(*tile) < tile->size());
        }
        auto const lowerLeft = BoxDrawingRendererTest::buildElements(U'\uE0B8', size, 1);
        REQUIRE(lowerLeft);
        CHECK(lowerLeft->front() == 255);
        CHECK(lowerLeft->back() == 0);
        for (auto const& pair: { std::pair { U'\uE0B9', U'\uE0BF' }, std::pair { U'\uE0BB', U'\uE0BD' } })
            CHECK(BoxDrawingRendererTest::buildElements(pair.first, size, 1)
                  == BoxDrawingRendererTest::buildElements(pair.second, size, 1));
        for (auto const codepoint: { U'\uE0B1', U'\uE0B3' })
        {
            auto const chevron = BoxDrawingRendererTest::buildElements(codepoint, size, 1);
            REQUIRE(chevron);
            auto const width = size.width.as<size_t>();
            auto const middle = size.height.as<size_t>() / 2;
            auto const edgeCoverage = [&](size_t column) {
                auto coverage = 0;
                for (auto const row: std::views::iota(middle - 1, middle + 2))
                    coverage += (*chevron)[(row * width) + column];
                return coverage;
            };
            if (codepoint == U'\uE0B1')
            {
                CHECK(edgeCoverage(0) == 0);
                CHECK(edgeCoverage(width - 1) > 0);
            }
            else
            {
                CHECK(edgeCoverage(0) > 0);
                CHECK(edgeCoverage(width - 1) == 0);
            }
        }
    }
}

TEST_CASE("BoxDrawingRenderer", "[renderer]")
{
    // ...

    SECTION("rasterize 0x2500 (HORIZ)")
    {
        auto const codepoint = char32_t { 0x2500 };
        auto const size = ImageSize { Width(5), Height(5) }; // Smaller size for easier matching
        auto const lineThickness = 1;

        auto const buffer =
            vtrasterizer::BoxDrawingRendererTest::buildBoxElements(codepoint, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());

        auto const tileData = TestTileData { .bitmapSize = size,
                                             .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                             .bitmap = *buffer };

        vtrasterizer::verifyBitmap(tileData,
                                   {
                                       ".....",
                                       ".....",
                                       "#####",
                                       ".....",
                                       ".....",
                                   });
    }

    SECTION("rasterize 0x2502 (VERT)")
    {
        auto const codepoint = char32_t { 0x2502 };
        auto const size = ImageSize { Width(5), Height(5) };
        auto const lineThickness = 1;

        auto const buffer =
            vtrasterizer::BoxDrawingRendererTest::buildBoxElements(codepoint, size, lineThickness);
        REQUIRE(buffer.has_value());

        auto const tileData = TestTileData { .bitmapSize = size,
                                             .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                             .bitmap = *buffer };

        vtrasterizer::verifyBitmap(tileData,
                                   {
                                       "..#..",
                                       "..#..",
                                       "..#..",
                                       "..#..",
                                       "..#..",
                                   });
    }

    SECTION("rasterize 0x256C (DOUBLE VERT AND HORIZ)")
    {
        auto const codepoint = char32_t { 0x256C };
        auto const size = ImageSize { Width(10), Height(10) };
        auto const lineThickness = 1;

        auto const buffer =
            vtrasterizer::BoxDrawingRendererTest::buildBoxElements(codepoint, size, lineThickness);
        REQUIRE(buffer.has_value());

        auto const tileData = TestTileData { .bitmapSize = size,
                                             .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                             .bitmap = *buffer };

        vtrasterizer::verifyBitmap(tileData,
                                   {
                                       "....#.#...",
                                       "....#.#...",
                                       "....#.#...",
                                       "....#.#...",
                                       "#####.####",
                                       "..........",
                                       "#####.####",
                                       "....#.#...",
                                       "....#.#...",
                                       "....#.#...",
                                   });
    }
}

TEST_CASE("BoxDrawingRenderer.renderable.math_symbols", "[renderer]")
{
    // Parenthesis pieces must be renderable
    for (auto cp = char32_t { 0x239B }; cp <= char32_t { 0x23A0 }; ++cp)
    {
        INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
        CHECK(BoxDrawingRenderer::renderable(cp));
    }

    // All 13 new mathematical symbols must be renderable
    for (auto cp = char32_t { 0x23A7 }; cp <= char32_t { 0x23B3 }; ++cp)
    {
        INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
        CHECK(BoxDrawingRenderer::renderable(cp));
    }

    // Existing square bracket range must remain renderable
    for (auto cp = char32_t { 0x23A1 }; cp <= char32_t { 0x23A6 }; ++cp)
    {
        INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
        CHECK(BoxDrawingRenderer::renderable(cp));
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.extensions", "[renderer]")
{
    auto const size = ImageSize { Width(8), Height(16) };
    auto const lineThickness = 1;

    SECTION("0x23AA CURLY BRACKET EXTENSION produces non-empty vertical line")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x23AA }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        CHECK(litPixels > 0);

        // The vertical line should have lit pixels in every row
        auto const w = unbox<size_t>(size.width);
        auto const h = unbox<size_t>(size.height);
        for (auto y: std::views::iota(0zu, h))
        {
            auto rowLit = size_t { 0 };
            for (auto x: std::views::iota(0zu, w))
                if ((*buffer)[(y * w) + x] > 0)
                    ++rowLit;
            INFO("Row " << y << " should have lit pixels");
            CHECK(rowLit > 0);
        }
    }

    SECTION("0x23AE INTEGRAL EXTENSION produces same output as 0x23AA")
    {
        auto const extBuf = BoxDrawingRendererTest::buildElements(char32_t { 0x23AA }, size, lineThickness);
        auto const intBuf = BoxDrawingRendererTest::buildElements(char32_t { 0x23AE }, size, lineThickness);
        REQUIRE(extBuf.has_value());
        REQUIRE(intBuf.has_value());
        CHECK(*extBuf == *intBuf);
    }

    SECTION("0x23AF HORIZONTAL LINE EXTENSION produces non-empty horizontal line")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x23AF }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        CHECK(litPixels > 0);

        // The horizontal line should have lit pixels in every column
        auto const w = unbox<size_t>(size.width);
        auto const h = unbox<size_t>(size.height);
        for (auto x: std::views::iota(0zu, w))
        {
            auto colLit = size_t { 0 };
            for (auto y: std::views::iota(0zu, h))
                if ((*buffer)[(y * w) + x] > 0)
                    ++colLit;
            INFO("Column " << x << " should have lit pixels");
            CHECK(colLit > 0);
        }
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.hooks", "[renderer]")
{
    auto const size = ImageSize { Width(10), Height(20) };
    auto const lineThickness = 1;

    SECTION("all hook codepoints produce non-empty buffers of correct size")
    {
        for (auto cp: { char32_t { 0x23A7 }, char32_t { 0x23A9 }, char32_t { 0x23AB }, char32_t { 0x23AD } })
        {
            INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
            auto const buffer = BoxDrawingRendererTest::buildElements(cp, size, lineThickness);
            REQUIRE(buffer.has_value());
            REQUIRE(buffer->size() == size.area());
            CHECK(countLitPixels(*buffer) > 0);
        }
    }

    SECTION("left upper hook is vertical mirror of left lower hook")
    {
        auto const upper = BoxDrawingRendererTest::buildElements(char32_t { 0x23A7 }, size, lineThickness);
        auto const lower = BoxDrawingRendererTest::buildElements(char32_t { 0x23A9 }, size, lineThickness);
        REQUIRE(upper.has_value());
        REQUIRE(lower.has_value());
        auto const mirrored = verticalMirror(*upper, size);
        // Due to integer rounding in arc rendering, allow small differences
        auto differences = size_t { 0 };
        for (auto i: std::views::iota(0zu, mirrored.size()))
            if ((mirrored[i] > 0) != ((*lower)[i] > 0))
                ++differences;
        // Allow up to 10% pixel differences due to rounding
        CHECK(differences < size.area() / 10);
    }

    SECTION("left and right upper hooks are approximate horizontal mirrors")
    {
        auto const leftHook = BoxDrawingRendererTest::buildElements(char32_t { 0x23A7 }, size, lineThickness);
        auto const rightHook =
            BoxDrawingRendererTest::buildElements(char32_t { 0x23AB }, size, lineThickness);
        REQUIRE(leftHook.has_value());
        REQUIRE(rightHook.has_value());
        auto const mirrored = horizontalMirror(*leftHook, size);
        auto differences = size_t { 0 };
        for (auto i: std::views::iota(0zu, mirrored.size()))
            if ((mirrored[i] > 0) != ((*rightHook)[i] > 0))
                ++differences;
        CHECK(differences < size.area() / 10);
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.middle_pieces", "[renderer]")
{
    auto const size = ImageSize { Width(10), Height(20) };
    auto const lineThickness = 1;

    SECTION("middle pieces produce non-empty buffers")
    {
        for (auto cp: { char32_t { 0x23A8 }, char32_t { 0x23AC } })
        {
            INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
            auto const buffer = BoxDrawingRendererTest::buildElements(cp, size, lineThickness);
            REQUIRE(buffer.has_value());
            REQUIRE(buffer->size() == size.area());
            CHECK(countLitPixels(*buffer) > 0);
        }
    }

    SECTION("left and right middle pieces are approximate horizontal mirrors")
    {
        auto const leftMid = BoxDrawingRendererTest::buildElements(char32_t { 0x23A8 }, size, lineThickness);
        auto const rightMid = BoxDrawingRendererTest::buildElements(char32_t { 0x23AC }, size, lineThickness);
        REQUIRE(leftMid.has_value());
        REQUIRE(rightMid.has_value());
        auto const mirrored = horizontalMirror(*leftMid, size);
        auto differences = size_t { 0 };
        for (auto i: std::views::iota(0zu, mirrored.size()))
            if ((mirrored[i] > 0) != ((*rightMid)[i] > 0))
                ++differences;
        CHECK(differences < size.area() / 10);
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.sections", "[renderer]")
{
    auto const size = ImageSize { Width(10), Height(20) };
    auto const lineThickness = 1;

    SECTION("bracket sections produce non-empty buffers")
    {
        for (auto cp: { char32_t { 0x23B0 }, char32_t { 0x23B1 } })
        {
            INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
            auto const buffer = BoxDrawingRendererTest::buildElements(cp, size, lineThickness);
            REQUIRE(buffer.has_value());
            REQUIRE(buffer->size() == size.area());
            CHECK(countLitPixels(*buffer) > 0);
        }
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.summation", "[renderer]")
{
    auto const size = ImageSize { Width(10), Height(20) };
    auto const lineThickness = 1;

    SECTION("summation pieces produce non-empty buffers")
    {
        for (auto cp: { char32_t { 0x23B2 }, char32_t { 0x23B3 } })
        {
            INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
            auto const buffer = BoxDrawingRendererTest::buildElements(cp, size, lineThickness);
            REQUIRE(buffer.has_value());
            REQUIRE(buffer->size() == size.area());
            CHECK(countLitPixels(*buffer) > 0);
        }
    }

    SECTION("summation top and bottom are approximate vertical mirrors")
    {
        auto const top = BoxDrawingRendererTest::buildElements(char32_t { 0x23B2 }, size, lineThickness);
        auto const bottom = BoxDrawingRendererTest::buildElements(char32_t { 0x23B3 }, size, lineThickness);
        REQUIRE(top.has_value());
        REQUIRE(bottom.has_value());
        auto const mirrored = verticalMirror(*top, size);
        auto differences = size_t { 0 };
        for (auto i: std::views::iota(0zu, mirrored.size()))
            if ((mirrored[i] > 0) != ((*bottom)[i] > 0))
                ++differences;
        CHECK(differences < size.area() / 10);
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.parenthesis_hooks", "[renderer]")
{
    auto const size = ImageSize { Width(10), Height(20) };
    auto const lineThickness = 1;

    SECTION("all parenthesis hook codepoints produce non-empty buffers of correct size")
    {
        for (auto cp: { char32_t { 0x239B }, char32_t { 0x239D }, char32_t { 0x239E }, char32_t { 0x23A0 } })
        {
            INFO("Codepoint: U+" << std::hex << static_cast<uint32_t>(cp));
            auto const buffer = BoxDrawingRendererTest::buildElements(cp, size, lineThickness);
            REQUIRE(buffer.has_value());
            REQUIRE(buffer->size() == size.area());
            CHECK(countLitPixels(*buffer) > 0);
        }
    }

    SECTION("left upper hook is vertical mirror of left lower hook")
    {
        auto const upper = BoxDrawingRendererTest::buildElements(char32_t { 0x239B }, size, lineThickness);
        auto const lower = BoxDrawingRendererTest::buildElements(char32_t { 0x239D }, size, lineThickness);
        REQUIRE(upper.has_value());
        REQUIRE(lower.has_value());
        auto const mirrored = verticalMirror(*upper, size);
        auto differences = size_t { 0 };
        for (auto i: std::views::iota(0zu, mirrored.size()))
            if ((mirrored[i] > 0) != ((*lower)[i] > 0))
                ++differences;
        // Allow up to 10% pixel differences due to rounding
        CHECK(differences < size.area() / 10);
    }

    SECTION("left and right upper hooks are approximate horizontal mirrors")
    {
        auto const leftHook = BoxDrawingRendererTest::buildElements(char32_t { 0x239B }, size, lineThickness);
        auto const rightHook =
            BoxDrawingRendererTest::buildElements(char32_t { 0x239E }, size, lineThickness);
        REQUIRE(leftHook.has_value());
        REQUIRE(rightHook.has_value());
        auto const mirrored = horizontalMirror(*leftHook, size);
        auto differences = size_t { 0 };
        for (auto i: std::views::iota(0zu, mirrored.size()))
            if ((mirrored[i] > 0) != ((*rightHook)[i] > 0))
                ++differences;
        CHECK(differences < size.area() / 10);
    }
}

TEST_CASE("BoxDrawingRenderer.math_symbols.parenthesis_extensions", "[renderer]")
{
    auto const size = ImageSize { Width(8), Height(16) };
    auto const lineThickness = 1;

    SECTION("left extension: non-empty, lit pixels only in left half")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x239C }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        CHECK(litPixels > 0);

        // Verify lit pixels are edge-aligned to the left
        auto const w = unbox<size_t>(size.width);
        auto const h = unbox<size_t>(size.height);
        for (auto y: std::views::iota(0zu, h))
            for (auto x: std::views::iota(w / 2, w))
            {
                INFO("Row " << y << ", Col " << x << " should be dark (right half)");
                CHECK((*buffer)[(y * w) + x] == 0);
            }
    }

    SECTION("right extension: non-empty, lit pixels only in right half")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x239F }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        CHECK(litPixels > 0);

        // Verify lit pixels are edge-aligned to the right
        auto const w = unbox<size_t>(size.width);
        auto const h = unbox<size_t>(size.height);
        for (auto y: std::views::iota(0zu, h))
            for (auto x: std::views::iota(0zu, w / 2))
            {
                INFO("Row " << y << ", Col " << x << " should be dark (left half)");
                CHECK((*buffer)[(y * w) + x] == 0);
            }
    }

    SECTION("left extension is exact horizontal mirror of right extension")
    {
        auto const leftExt = BoxDrawingRendererTest::buildElements(char32_t { 0x239C }, size, lineThickness);
        auto const rightExt = BoxDrawingRendererTest::buildElements(char32_t { 0x239F }, size, lineThickness);
        REQUIRE(leftExt.has_value());
        REQUIRE(rightExt.has_value());
        auto const mirrored = horizontalMirror(*leftExt, size);
        CHECK(mirrored == *rightExt);
    }
}

TEST_CASE("BoxDrawingRenderer.shade_characters", "[renderer]")
{
    auto const size = ImageSize { Width(8), Height(8) };
    auto const lineThickness = 1;

    SECTION("shade codepoints are renderable")
    {
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x2591 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x2592 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x2593 }));
    }

    SECTION("0x2591 LIGHT SHADE produces ~25% lit pixels")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x2591 }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        // 25% of 64 pixels = 16
        CHECK(litPixels == size.area() / 4);
    }

    SECTION("0x2592 MEDIUM SHADE produces ~50% lit pixels")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x2592 }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        // 50% of 64 pixels = 32
        CHECK(litPixels == size.area() / 2);
    }

    SECTION("0x2593 DARK SHADE produces ~75% lit pixels")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x2593 }, size, lineThickness);
        REQUIRE(buffer.has_value());
        REQUIRE(buffer->size() == size.area());
        auto const litPixels = countLitPixels(*buffer);
        // 75% of 64 pixels = 48
        CHECK(litPixels == 3 * size.area() / 4);
    }

    SECTION("LIGHT SHADE and DARK SHADE are complementary")
    {
        auto const light = BoxDrawingRendererTest::buildElements(char32_t { 0x2591 }, size, lineThickness);
        auto const dark = BoxDrawingRendererTest::buildElements(char32_t { 0x2593 }, size, lineThickness);
        REQUIRE(light.has_value());
        REQUIRE(dark.has_value());
        // Every pixel that is lit in light shade is dark in dark shade and vice versa
        for (auto i: std::views::iota(0zu, light->size()))
            CHECK(((*light)[i] > 0) != ((*dark)[i] > 0));
    }

    SECTION("0x2591 LIGHT SHADE bitmap pattern")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x2591 }, size, lineThickness);
        REQUIRE(buffer.has_value());

        auto const tileData = TestTileData { .bitmapSize = size,
                                             .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                             .bitmap = *buffer };

        vtrasterizer::verifyBitmap(tileData,
                                   {
                                       "........",
                                       "#.#.#.#.",
                                       "........",
                                       "#.#.#.#.",
                                       "........",
                                       "#.#.#.#.",
                                       "........",
                                       "#.#.#.#.",
                                   });
    }

    SECTION("0x2592 MEDIUM SHADE bitmap pattern")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x2592 }, size, lineThickness);
        REQUIRE(buffer.has_value());

        auto const tileData = TestTileData { .bitmapSize = size,
                                             .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                             .bitmap = *buffer };

        vtrasterizer::verifyBitmap(tileData,
                                   {
                                       ".#.#.#.#",
                                       "#.#.#.#.",
                                       ".#.#.#.#",
                                       "#.#.#.#.",
                                       ".#.#.#.#",
                                       "#.#.#.#.",
                                       ".#.#.#.#",
                                       "#.#.#.#.",
                                   });
    }

    SECTION("0x2593 DARK SHADE bitmap pattern")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x2593 }, size, lineThickness);
        REQUIRE(buffer.has_value());

        auto const tileData = TestTileData { .bitmapSize = size,
                                             .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                             .bitmap = *buffer };

        vtrasterizer::verifyBitmap(tileData,
                                   {
                                       "########",
                                       ".#.#.#.#",
                                       "########",
                                       ".#.#.#.#",
                                       "########",
                                       ".#.#.#.#",
                                       "########",
                                       ".#.#.#.#",
                                   });
    }
}

TEST_CASE("BoxDrawingRenderer.solidTriangles", "[boxdrawing]")
{
    // Contour draws these itself rather than leaving them to the font: a font's ▼/▶ are small, thin
    // and inconsistent between families, which is what made the fold-column marker hard to see and
    // hard to recognise as a control. Drawn here they are cell-proportionate in every font.
    auto constexpr Size = ImageSize { Width(8), Height(8) };
    auto constexpr LineThickness = 1;

    SECTION("every fold-column glyph is drawn here, not by the font")
    {
        // The fold column's docstring promises its glyphs come from this renderer. Nothing else
        // enforces that: a sixth FoldMarker whose glyph fell outside renderable() would silently
        // drop back to the font and look wrong at runtime, with no build or test failure.
        using vtbackend::FoldMarker;
        for (auto const marker:
             { FoldMarker::Expanded, FoldMarker::Collapsed, FoldMarker::Body, FoldMarker::BodyEnd })
        {
            INFO("marker " << std::format("{}", marker));
            CHECK(BoxDrawingRenderer::renderable(vtbackend::foldMarkerGlyph(marker)));
        }
    }

    SECTION("renderable() claims them, so the font is never consulted")
    {
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x25B6 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x25BC }));

        // Their neighbours in the U+25A0..25FF block are still on the TODO list and must keep
        // falling through to the font rather than rendering as nothing.
        CHECK(!BoxDrawingRenderer::renderable(char32_t { 0x25B2 }));
        CHECK(!BoxDrawingRenderer::renderable(char32_t { 0x25C0 }));
    }

    SECTION("0x25BC BLACK DOWN-POINTING TRIANGLE has its base on top")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x25BC }, Size, LineThickness);
        REQUIRE(buffer.has_value());

        // The atlas buffer is bottom-up -- row 0 is the BOTTOM of the glyph, as U+2580 UPPER HALF
        // BLOCK lighting rows 4..7 shows -- so the art below is upside down: its LAST line is the
        // triangle's base along the top of the cell, and its apex is four rows below that.
        vtrasterizer::verifyBitmap(TestTileData { .bitmapSize = Size,
                                                  .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                                  .bitmap = *buffer },
                                   {
                                       "........",
                                       "........",
                                       "........",
                                       "....#...",
                                       "...##...",
                                       "..####..",
                                       ".######.",
                                       "########",
                                   });
    }

    SECTION("0x25B6 BLACK RIGHT-POINTING TRIANGLE has its base on the left")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x25B6 }, Size, LineThickness);
        REQUIRE(buffer.has_value());

        // Full height down the left edge, tapering to an apex at mid-width: pointing right.
        // Near-symmetric top to bottom, so the bottom-up buffer order barely shows here.
        vtrasterizer::verifyBitmap(TestTileData { .bitmapSize = Size,
                                                  .bitmapFormat = vtrasterizer::atlas::Format::Red,
                                                  .bitmap = *buffer },
                                   {
                                       "#.......",
                                       "##......",
                                       "###.....",
                                       "#####...",
                                       "####....",
                                       "###.....",
                                       "##......",
                                       "#.......",
                                   });
    }
}

TEST_CASE("BoxDrawingRenderer.foldHeadGlyphsInTinyCells", "[boxdrawing]")
{
    // A small font at a low DPI can hand the builder a cell only a few device pixels wide. The box has
    // to come out of that as SOMETHING drawable rather than as undefined behaviour: sizing it once
    // clamped the desired side between a floor of 5 and a ceiling of w-2, which are in the wrong order
    // the moment the cell is narrower than seven pixels.
    for (auto const width: std::views::iota(1, 12))
    {
        for (auto const height: std::views::iota(1, 12))
        {
            CAPTURE(width, height);
            auto const cell =
                ImageSize { Width(static_cast<unsigned>(width)), Height(static_cast<unsigned>(height)) };
            for (auto const codepoint: { char32_t { 0x10F000 }, char32_t { 0x10F001 } })
            {
                CAPTURE(static_cast<uint32_t>(codepoint));
                auto const buffer = BoxDrawingRendererTest::buildElements(codepoint, cell, 1);
                REQUIRE(buffer.has_value());
                CHECK(buffer->size() == static_cast<size_t>(width * height));
            }
        }
    }
}

TEST_CASE("BoxDrawingRenderer.foldHeadGlyphs", "[boxdrawing]")
{
    // Contour's own private-use glyphs: the fold column's head, a box carrying a minus while the
    // block is open and a plus once it is closed. No Unicode codepoint carries the stem that ties an
    // open head to the body column below it, which is why these are drawn here rather than typed.
    //
    // A realistic cell (9x20). The art is in BUFFER order -- row 0 is the BOTTOM of the glyph -- so
    // it reads upside down; the stem is therefore at the TOP of the open head's art.
    auto constexpr Cell = ImageSize { Width(9), Height(20) };

    SECTION("renderable() claims the block, and nothing beyond it")
    {
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x10F000 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x10F001 }));

        // The reserved block is documented as F0000..F00FF, but only what is drawn may be claimed:
        // claiming the rest would swallow codepoints that must still fall through to the font.
        CHECK(!BoxDrawingRenderer::renderable(char32_t { 0xF0002 }));
        CHECK(!BoxDrawingRenderer::renderable(char32_t { 0xEFFFF }));
    }

    SECTION("U+10F000 fold head, open: a boxed minus with a stem into the body below")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x10F000 }, Cell, 1);
        REQUIRE(buffer.has_value());

        vtrasterizer::verifyBitmap(
            TestTileData {
                .bitmapSize = Cell, .bitmapFormat = vtrasterizer::atlas::Format::Red, .bitmap = *buffer },
            {
                "....#....", "....#....", "....#....", "....#....", "....#....", "....#....", "....#....",
                ".#######.", ".#.....#.", ".#.....#.", ".#.###.#.", ".#.....#.", ".#.....#.", ".#######.",
                ".........", ".........", ".........", ".........", ".........", ".........",
            });
    }

    SECTION("U+10F001 fold head, closed: a boxed plus, and no stem")
    {
        auto const buffer = BoxDrawingRendererTest::buildElements(char32_t { 0x10F001 }, Cell, 1);
        REQUIRE(buffer.has_value());

        // No stem: a collapsed block has nothing below it for the column to continue into.
        vtrasterizer::verifyBitmap(
            TestTileData {
                .bitmapSize = Cell, .bitmapFormat = vtrasterizer::atlas::Format::Red, .bitmap = *buffer },
            {
                ".........", ".........", ".........", ".........", ".........", ".........", ".........",
                ".#######.", ".#.....#.", ".#..#..#.", ".#.###.#.", ".#..#..#.", ".#.....#.", ".#######.",
                ".........", ".........", ".........", ".........", ".........", ".........",
            });
    }

    SECTION("the open head's stem meets the body column exactly")
    {
        // The stem, the body and its end share one column at one width, or the fold column steps at every
        // head -- light and heavy alike, one cell wide or two, at any stroke.
        auto const litColumns = [](atlas::Buffer const& b, ImageSize size, size_t row) {
            auto const w = unbox<size_t>(size.width);
            auto columns = std::vector<size_t> {};
            for (auto const x: std::views::iota(size_t { 0 }, w))
                if (b[(row * w) + x] > 0)
                    columns.push_back(x);
            return columns;
        };
        auto constexpr Wide = ImageSize { Width(18), Height(20) };
        for (auto const size: { Cell, Wide })
        {
            for (auto const thickness: { 1, 2 })
            {
                for (auto const& [open, body, end]: { std::tuple { 0x10F000, 0x10F006, 0x10F007 },
                                                      std::tuple { 0x10F008, 0x10F00A, 0x10F00B },
                                                      std::tuple { 0x10F00C, 0x10F00E, 0x10F00F } })
                {
                    CAPTURE(unbox<int>(size.width), thickness, open);
                    auto const headBuffer =
                        BoxDrawingRendererTest::buildElements(char32_t(open), size, thickness);
                    auto const bodyBuffer =
                        BoxDrawingRendererTest::buildElements(char32_t(body), size, thickness);
                    auto const endBuffer =
                        BoxDrawingRendererTest::buildElements(char32_t(end), size, thickness);
                    REQUIRE(headBuffer.has_value());
                    REQUIRE(bodyBuffer.has_value());
                    REQUIRE(endBuffer.has_value());

                    // Buffers run bottom-up: row 0 is the cell's bottom, the last row its top.
                    auto const top = unbox<size_t>(size.height) - 1;
                    auto const stem = litColumns(*headBuffer, size, 0);
                    CHECK_FALSE(stem.empty());
                    CHECK(litColumns(*bodyBuffer, size, top) == stem);
                    CHECK(litColumns(*bodyBuffer, size, 0) == stem);
                    CHECK(litColumns(*endBuffer, size, top) == stem);
                }
            }
        }
    }

    SECTION("the heavy fold column is twice the weight, the heavier three times")
    {
        auto const light = BoxDrawingRendererTest::buildElements(char32_t { 0x10F006 }, Cell, 1);
        auto const heavy = BoxDrawingRendererTest::buildElements(char32_t { 0x10F00A }, Cell, 1);
        auto const heavier = BoxDrawingRendererTest::buildElements(char32_t { 0x10F00E }, Cell, 1);
        REQUIRE(light.has_value());
        REQUIRE(heavy.has_value());
        REQUIRE(heavier.has_value());
        CHECK(countLitPixels(*heavy) == 2 * countLitPixels(*light));
        CHECK(countLitPixels(*heavier) == 3 * countLitPixels(*light));
    }

    SECTION("the stem leaves the box from its centre, light or heavy, one cell wide or two")
    {
        // As many columns of box left of the stem as right of it: a heavy stem a pixel off the centre of an
        // odd box reads as a crooked control.
        auto constexpr Wide = ImageSize { Width(18), Height(20) };
        for (auto const size: { Cell, Wide })
        {
            for (auto const& [closed, open]: { std::pair { 0x10F001, 0x10F000 },
                                               std::pair { 0x10F009, 0x10F008 },
                                               std::pair { 0x10F00D, 0x10F00C } })
            {
                CAPTURE(unbox<int>(size.width), closed);
                auto const w = unbox<int>(size.width);
                auto const box = BoxDrawingRendererTest::buildElements(char32_t(closed), size, 1);
                auto const head = BoxDrawingRendererTest::buildElements(char32_t(open), size, 1);
                REQUIRE(box.has_value());
                REQUIRE(head.has_value());
                auto boxColumns = std::vector<int> {};
                for (auto const i: std::views::iota(0, w * 20))
                    if ((*box)[static_cast<size_t>(i)] > 0)
                        boxColumns.push_back(i % w);
                auto stem = std::vector<int> {}; // the head's bottom row: its stem alone
                for (auto const x: std::views::iota(0, w))
                    if ((*head)[static_cast<size_t>(x)] > 0)
                        stem.push_back(x);
                REQUIRE_FALSE(boxColumns.empty());
                REQUIRE_FALSE(stem.empty());
                CHECK(stem.front() - std::ranges::min(boxColumns)
                      == std::ranges::max(boxColumns) - stem.back());
            }
        }
    }

    SECTION("the body's end turns right at the row's middle, out to the cell's edge")
    {
        auto const end = BoxDrawingRendererTest::buildElements(char32_t { 0x10F007 }, Cell, 1);
        REQUIRE(end.has_value());
        vtrasterizer::verifyBitmap(
            TestTileData {
                .bitmapSize = Cell, .bitmapFormat = vtrasterizer::atlas::Format::Red, .bitmap = *end },
            {
                ".........", ".........", ".........", ".........", ".........", ".........", ".........",
                ".........", ".........", ".........", "....#####", "....#....", "....#....", "....#....",
                "....#....", "....#....", "....#....", "....#....", "....#....", "....#....",
            });
    }
}

TEST_CASE("BoxDrawingRenderer.contourGlyphsKeepTheCellsStrokeWhenDrawnTwoCellsWide", "[boxdrawing]")
{
    // A DECDWL row's text is stretched to twice its width, and its box drawing takes twice the stroke to
    // match. The gutter's glyphs are drawn two cells wide for their size: they keep a cell's stroke, so the
    // fold column reads the same in either width.
    auto const wide = vtbackend::LineFlags { vtbackend::LineFlag::DoubleWidth };
    auto const narrow = vtbackend::LineFlags {};
    CHECK(BoxDrawingRenderer::tileLineThickness(char32_t { 0x2502 }, wide, 1) == 2);
    CHECK(BoxDrawingRenderer::tileLineThickness(char32_t { 0x2502 }, narrow, 1) == 1);
    for (auto const codepoint: std::views::iota(char32_t { 0x10F000 }, char32_t { 0x10F010 }))
    {
        CAPTURE(static_cast<uint32_t>(codepoint));
        CHECK(BoxDrawingRenderer::renderable(codepoint));
        CHECK(BoxDrawingRenderer::tileLineThickness(codepoint, wide, 1) == 1);
    }
    CHECK_FALSE(BoxDrawingRenderer::isContourGlyph(char32_t { 0x10F010 }));
}

TEST_CASE("BoxDrawingRenderer.gutterStatusMarks", "[boxdrawing]")
{
    // The block column's status marks, drawn here for the reason the fold heads are: one size and weight
    // in every font, centred on the row the fold column's stem runs down. A realistic 9x20 cell.
    auto constexpr Cell = ImageSize { Width(9), Height(20) };
    auto const build = [&](char32_t codepoint) {
        auto buffer = BoxDrawingRendererTest::buildElements(codepoint, Cell, 1);
        REQUIRE(buffer.has_value());
        return *std::move(buffer);
    };

    SECTION("renderable() claims them, so the font is never consulted")
    {
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x10F002 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x10F003 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x10F004 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x10F005 }));
    }

    SECTION("the Unicode shapes they look like stay the font's")
    {
        // Program output writes these -- systemctl's ●, a prompt's ▸ or ◆, DEC Special Graphics' ` -- and
        // draws them in its font's own size and weight, as it did before the gutter had marks.
        CHECK_FALSE(BoxDrawingRenderer::renderable(char32_t { 0x25B8 }));
        CHECK_FALSE(BoxDrawingRenderer::renderable(char32_t { 0x25C6 }));
        CHECK_FALSE(BoxDrawingRenderer::renderable(char32_t { 0x25CF }));
        CHECK_FALSE(BoxDrawingRenderer::renderable(char32_t { 0x2717 }));
    }

    SECTION("each fills its cell's buffer, draws something, and sits on the row's middle")
    {
        for (auto const codepoint:
             { char32_t { 0x10F002 }, char32_t { 0x10F003 }, char32_t { 0x10F004 }, char32_t { 0x10F005 } })
        {
            CAPTURE(static_cast<uint32_t>(codepoint));
            auto const buffer = build(codepoint);
            CHECK(buffer.size() == 9zu * 20zu);
            CHECK(countLitPixels(buffer) > 0);
            CHECK(buffer == verticalMirror(buffer, Cell));
        }
    }

    SECTION("the diamond and the disc are symmetric left to right; the triangle points right")
    {
        auto const diamond = build(0x10F003);
        auto const disc = build(0x10F004);
        auto const triangle = build(0x10F002);
        CHECK(diamond == horizontalMirror(diamond, Cell));
        CHECK(disc == horizontalMirror(disc, Cell));

        auto const litInColumns = [](atlas::Buffer const& buffer, size_t from, size_t to) {
            auto lit = size_t { 0 };
            for (auto const y: std::views::iota(size_t { 0 }, size_t { 20 }))
                for (auto const x: std::views::iota(from, to))
                    lit += buffer[(y * 9) + x] > 0 ? 1 : 0;
            return lit;
        };
        // Base on the left, apex on the right: more of it lies left of the middle column.
        CHECK(litInColumns(triangle, 0, 4) > litInColumns(triangle, 5, 9));
    }

    SECTION("the disc covers more of the cell than the diamond inscribed in it")
    {
        CHECK(countLitPixels(build(0x10F004)) > countLitPixels(build(0x10F003)));
    }

    SECTION("the cross is symmetric both ways, and is not the disc")
    {
        auto const cross = build(0x10F005);
        CHECK(cross == horizontalMirror(cross, Cell));
        CHECK(cross != build(0x10F004));
        // Its centre is lit, and a corner of the disc's square is not: a cross, not a box.
        CHECK(cross[(10 * 9) + 4] > 0);
        CHECK(cross[(2 * 9) + 0] == 0);
    }
}

TEST_CASE("BoxDrawingRenderer.gutterGlyphsDrawnTwoCellsWideAreTwiceTheSize", "[boxdrawing]")
{
    // A two-cell block column draws its glyph into a buffer two cells wide (LineFlag::DoubleWidth): the fold
    // control and the marks then size themselves to the row's height rather than to one cell's width.
    auto constexpr Cell = ImageSize { Width(10), Height(19) };
    auto constexpr Wide = ImageSize { Width(20), Height(19) };
    auto const lit = [](char32_t codepoint, ImageSize size) {
        auto buffer = BoxDrawingRendererTest::buildElements(codepoint, size, 1);
        REQUIRE(buffer.has_value());
        CHECK(buffer->size() == unbox<size_t>(size.width) * unbox<size_t>(size.height));
        return countLitPixels(*buffer);
    };

    for (auto const codepoint: { char32_t { 0x10F000 },
                                 char32_t { 0x10F001 },
                                 char32_t { 0x10F002 },
                                 char32_t { 0x10F003 },
                                 char32_t { 0x10F004 },
                                 char32_t { 0x10F005 } })
    {
        CAPTURE(static_cast<uint32_t>(codepoint));
        CHECK(lit(codepoint, Wide) > lit(codepoint, Cell) * 3 / 2);
    }
}

TEST_CASE("BoxDrawingRenderer.gutterStatusMarksInTinyCells", "[boxdrawing]")
{
    // A small font at a low DPI hands the builder a cell a few device pixels wide; the marks must still
    // come out as a buffer of exactly that size rather than as undefined behaviour.
    for (auto const width: std::views::iota(1, 12))
    {
        for (auto const height: std::views::iota(1, 12))
        {
            CAPTURE(width, height);
            auto const cell =
                ImageSize { Width(static_cast<unsigned>(width)), Height(static_cast<unsigned>(height)) };
            for (auto const codepoint: { char32_t { 0x10F002 },
                                         char32_t { 0x10F003 },
                                         char32_t { 0x10F004 },
                                         char32_t { 0x10F005 } })
            {
                auto const buffer = BoxDrawingRendererTest::buildElements(codepoint, cell, 1);
                REQUIRE(buffer.has_value());
                CHECK(buffer->size() == static_cast<size_t>(width * height));
            }
        }
    }
}
