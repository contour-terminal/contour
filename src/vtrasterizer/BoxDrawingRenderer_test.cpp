#include <vtbackend/Color.h>

#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RendererTestHelpers.h>
#include <vtrasterizer/TextureAtlas.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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

struct TestTileData
{
    vtbackend::ImageSize bitmapSize;
    vtrasterizer::atlas::Format bitmapFormat;
    std::vector<uint8_t> bitmap;
};

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
            mirrored[y * w + x] = buffer[y * w + (w - 1 - x)];
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
            mirrored[y * w + x] = buffer[(h - 1 - y) * w + x];
    return mirrored;
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
                if ((*buffer)[y * w + x] > 0)
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
                if ((*buffer)[y * w + x] > 0)
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
                CHECK((*buffer)[y * w + x] == 0);
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
                CHECK((*buffer)[y * w + x] == 0);
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
