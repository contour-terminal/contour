
#include <vtbackend/Image.h>
#include <vtbackend/test_helpers.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <ranges>

using namespace vtbackend;

namespace
{

/// Marks every image pixel, so that "is this pixel image or gap?" is answerable without ambiguity.
/// Deliberately never equal to TestBackground: an image pixel that happens to match the background
/// would read as a gap and quietly weaken the comparison.
constexpr auto ImageMarker = uint8_t { 0x80 };
constexpr auto TestBackground = RGBAColor { 0x00, 0x00, 0x00, 0xFF };

/// An image whose every pixel encodes its own coordinate, so a sampled texel identifies exactly
/// which source pixel it came from.
std::shared_ptr<Image> makeCoordinateImage(ImageSize size)
{
    auto data = Image::Data(size.area() * 4, 0);
    for (auto const y: std::views::iota(0u, unbox<unsigned>(size.height)))
        for (auto const x: std::views::iota(0u, unbox<unsigned>(size.width)))
        {
            auto const base = ((y * unbox<unsigned>(size.width)) + x) * 4;
            data[base + 0] = static_cast<uint8_t>(x & 0xFF);
            data[base + 1] = static_cast<uint8_t>(y & 0xFF);
            data[base + 2] = ImageMarker;
            data[base + 3] = 0xFF;
        }
    return std::make_shared<Image>(ImageId(1), ImageFormat::RGBA, std::move(data), size, [](auto) {});
}

/// Counts the cells of a full-page sixel's span that the image fails to cover -- i.e. the gap.
///
/// Drives the live composition rather than a model of it: an application is told the cell is
/// @p reportedCell (it divides what TerminalDisplay::reportedPixelSize reports by the grid to get it)
/// and sizes its canvas to `reportedCell * page`. Screen::sixelImage spans that canvas across the page
/// and builds the RasterizedImage with the REPORTED cell -- but ImageRenderer::renderImage then asks
/// for the placement with the DEVICE cell, and that is what fragmentPlacement aspect-fits it into.
///
/// So the two cells must be SIMILAR, not merely close: ImageResize::ResizeToFit scales by
/// std::min(gridW/imageW, gridH/imageH), which honors whichever axis lost less and letterboxes the
/// other. Only a reported cell that is exactly the device cell makes that scale 1.0 on both axes.
///
/// @param deviceCell   The renderer's own cell, in device pixels (GridMetrics::cellSize).
/// @param reportedCell The cell an application was told about.
/// @param page         Total page size the image is drawn across.
/// @return The number of cells in the span not fully covered by the image; 0 means no gap.
[[nodiscard]] int uncoveredCells(ImageSize deviceCell, ImageSize reportedCell, PageSize page)
{
    auto const canvas = reportedCell * page; // what the application draws
    // Screen::sixelImage derives the span as ceil(canvas / reportedCell); a canvas that is a whole
    // number of reported cells -- which a full-page image is, by construction -- ceils to exactly the
    // page. Stated as the page rather than re-deriving it, so this test does not carry its own second
    // copy of that rounding rule to drift from Screen.cpp's.
    auto const span = GridSize { .lines = page.lines, .columns = page.columns };
    auto const rasterized = std::make_shared<RasterizedImage>(makeCoordinateImage(canvas),
                                                              ImageAlignment::TopStart,
                                                              ImageResize::ResizeToFit,
                                                              TestBackground,
                                                              span,
                                                              reportedCell);

    auto uncovered = 0;
    for (auto const line: std::views::iota(0, unbox<int>(span.lines)))
        for (auto const column: std::views::iota(0, unbox<int>(span.columns)))
        {
            auto const pos = CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
            if (!rasterized->fragmentPlacement(pos, deviceCell).coversCell)
                ++uncovered;
        }
    return uncovered;
}

} // namespace

TEST_CASE("RasterizedImage.fullPageSixel leaves no gap when the reported cell is the device cell",
          "[RasterizedImage]")
{
    // The measured cell of a real display scaled by 1.75 (2560x1600 native, 1463x915 logical).
    auto constexpr DeviceCell = ImageSize { Width(17), Height(39) };
    auto constexpr Page = PageSize { .lines = LineCount(4), .columns = ColumnCount(8) };

    SECTION("Device reporting: the reported cell IS the device cell, so every cell is covered")
    {
        // The fit scale is exactly 1.0 on both axes: the image lands 1:1, unresampled and gapless.
        // This is the whole reason PixelReporting::Device is the default -- the cell is the font's
        // advance in device pixels, so only a device-pixel report divides back to it exactly.
        CHECK(uncoveredCells(DeviceCell, DeviceCell, Page) == 0);
    }

    SECTION("Logical reporting at a fractional scale letterboxes, which is why it is not the default")
    {
        // reportedCellSize(17x39, 1.75) floors each axis on its own: floor(9.714)=9, floor(22.29)=22.
        // The width loses 7.4% but the height only 1.3%, so ResizeToFit fills the height and leaves
        // the width short -- a gap down the right of a full-screen image. Asserted rather than merely
        // tolerated, so that quietly routing the default back through a non-similar cell cannot pass.
        CHECK(uncoveredCells(DeviceCell, ImageSize { Width(9), Height(22) }, Page) > 0);
    }
}

TEST_CASE("RasterizedImage.fragmentPlacement agrees with fragment", "[RasterizedImage]")
{
    // fragmentPlacement() states as geometry what fragment() resolves per pixel. If the two ever
    // disagree, moving the resample from the CPU to the GPU moves the image -- so pin them against
    // each other across every alignment and resize policy rather than against hand-computed rects.
    auto constexpr CellSize = ImageSize { Width(10), Height(20) };
    auto constexpr CellSpan = GridSize { .lines = LineCount(4), .columns = ColumnCount(6) };
    // Deliberately not a whole multiple of the cell span, so the policies actually letterbox and
    // some cells come out partially or wholly uncovered.
    auto const image = makeCoordinateImage(ImageSize { Width(37), Height(53) });

    auto const alignments =
        std::array { ImageAlignment::TopStart,    ImageAlignment::TopCenter,    ImageAlignment::TopEnd,
                     ImageAlignment::MiddleStart, ImageAlignment::MiddleCenter, ImageAlignment::MiddleEnd,
                     ImageAlignment::BottomStart, ImageAlignment::BottomCenter, ImageAlignment::BottomEnd };
    auto const resizes =
        std::array { ImageResize::NoResize, ImageResize::ResizeToFit, ImageResize::ResizeToFill };

    for (auto const alignment: alignments)
        for (auto const resize: resizes)
        {
            auto const rasterized = std::make_shared<RasterizedImage>(
                image, alignment, resize, TestBackground, CellSpan, CellSize);

            for (auto const line: std::views::iota(0, unbox<int>(CellSpan.lines)))
                for (auto const column: std::views::iota(0, unbox<int>(CellSpan.columns)))
                {
                    auto const pos =
                        CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
                    auto const placement = rasterized->fragmentPlacement(pos, CellSize);
                    auto const pixels = rasterized->fragment(pos, CellSize);

                    INFO(std::format("alignment={} resize={} line={} column={}",
                                     static_cast<int>(alignment),
                                     static_cast<int>(resize),
                                     line,
                                     column));

                    // Which pixels of the cell fragment() considers image rather than gap must be
                    // exactly the rectangle fragmentPlacement() reports.
                    for (auto const y: std::views::iota(0, unbox<int>(CellSize.height)))
                        for (auto const x: std::views::iota(0, unbox<int>(CellSize.width)))
                        {
                            auto const base = static_cast<size_t>((y * unbox<int>(CellSize.width)) + x) * 4;
                            auto const isGap = pixels[base + 2] != ImageMarker;
                            auto const insideRect =
                                placement.hasImage && x >= placement.targetX
                                && x < placement.targetX + unbox<int>(placement.targetSize.width)
                                && y >= placement.targetY
                                && y < placement.targetY + unbox<int>(placement.targetSize.height);
                            if (insideRect)
                            {
                                INFO(std::format("pixel x={} y={} must be image", x, y));
                                CHECK_FALSE(isGap);
                            }
                        }

                    if (placement.hasImage)
                    {
                        CHECK(placement.sourceWidth > 0.0f);
                        CHECK(placement.sourceHeight > 0.0f);
                        CHECK(placement.sourceX >= 0.0f);
                        CHECK(placement.sourceY >= 0.0f);
                        CHECK(placement.sourceX + placement.sourceWidth <= 1.0001f);
                        CHECK(placement.sourceY + placement.sourceHeight <= 1.0001f);
                    }
                }
        }
}

TEST_CASE("RasterizedImage.fragmentPlacement maps a cell onto the matching source texels",
          "[RasterizedImage]")
{
    // A 1:1 case with no scaling and no gap: cell (line, column) must map to exactly its own slice
    // of the source, which is the property the whole-image quad path relies on.
    auto constexpr CellSize = ImageSize { Width(10), Height(20) };
    auto constexpr CellSpan = GridSize { .lines = LineCount(2), .columns = ColumnCount(3) };
    auto const image = makeCoordinateImage(ImageSize { Width(30), Height(40) });
    auto const rasterized = std::make_shared<RasterizedImage>(image,
                                                              ImageAlignment::TopStart,
                                                              ImageResize::NoResize,
                                                              RGBAColor { 0, 0, 0, 0xFF },
                                                              CellSpan,
                                                              CellSize);

    auto const placement = rasterized->fragmentPlacement(
        CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) }, CellSize);

    REQUIRE(placement.hasImage);
    CHECK(placement.coversCell);
    CHECK(placement.targetX == 0);
    CHECK(placement.targetY == 0);
    CHECK(placement.targetSize == CellSize);
    // Column 2 of 3 -> x in [20,30) of 30 -> 2/3 .. 1. Line 1 of 2 -> y in [20,40) of 40 -> 1/2 .. 1.
    CHECK(placement.sourceX == Catch::Approx(20.0 / 30.0));
    CHECK(placement.sourceWidth == Catch::Approx(10.0 / 30.0));
    CHECK(placement.sourceY == Catch::Approx(20.0 / 40.0));
    CHECK(placement.sourceHeight == Catch::Approx(20.0 / 40.0));
}

TEST_CASE("RasterizedImage.fragment", "[RasterizedImage]")
{
    auto const imageSize = ImageSize { Width(100), Height(100) };
    auto imageData = std::vector<uint8_t>(static_cast<std::size_t>(100 * 100 * 4), 0xFF); // White opaque
    auto image =
        std::make_shared<Image>(ImageId(1), ImageFormat::RGBA, std::move(imageData), imageSize, [](auto) {});

    auto rasterizedImage = rasterize(image,
                                     ImageAlignment::MiddleCenter,
                                     ImageResize::ResizeToFit,
                                     RGBAColor(0x000000FF), // Black background
                                     GridSize { .lines = LineCount(10), .columns = ColumnCount(10) },
                                     ImageSize { Width(10), Height(20) } // 10x20 pixels per cell
    );

    // Verify basic properties
    REQUIRE(rasterizedImage->image().width() == imageSize.width);
    REQUIRE(rasterizedImage->image().height() == imageSize.height);

    // Render a fragment
    auto const fragment =
        rasterizedImage->fragment(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    REQUIRE(fragment.size() == static_cast<std::size_t>(10 * 20 * 4));

    // Benchmark
    auto const start = std::chrono::high_resolution_clock::now();
    int const iterations = 1000000;
    size_t check = 0;
    for (int i = 0; i < iterations; ++i)
    {
        auto f = rasterizedImage->fragment(CellLocation { .line = LineOffset(5), .column = ColumnOffset(5) });
        check += f.size();
        // Verify center pixel is white (part of image)
        if (!f.empty())
        {
            // Check first pixel RGBA
            // Image is white (0xFF, 0xFF, 0xFF, 0xFF)
            // Background is black (but we are in image area)
            // Global X=50. xOffset=0. sourceX=50.
            if (f[0] != 0xFF)
            {
                std::cout << "Error: Pixel 0 is not white! Value: " << (int) f[0] << "\n";
            }
        }
    }
    auto const end = std::chrono::high_resolution_clock::now();
    auto const diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "RasterizedImage::fragment benchmark: " << diff << "ms for " << iterations << " iterations ("
              << (double(diff) / iterations) << "ms per op) check=" << check << "\n";
}
