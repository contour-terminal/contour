#include <vtbackend/Color.h>

#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RendererTestHelpers.h>
#include <vtrasterizer/TextureAtlas.h>

#include <catch2/catch_test_macros.hpp>

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
};
} // namespace vtrasterizer

struct TestTileData
{
    vtbackend::ImageSize bitmapSize;
    vtrasterizer::atlas::Format bitmapFormat;
    std::vector<uint8_t> bitmap;
};

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
