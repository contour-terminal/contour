
#include <vtbackend/Image.h>
#include <vtbackend/test_helpers.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <iostream>

using namespace vtbackend;

TEST_CASE("RasterizedImage.fragment", "[RasterizedImage]")
{
    auto const imageSize = ImageSize { Width(100), Height(100) };
    auto imageData = std::vector<uint8_t>(100 * 100 * 4, 0xFF); // White opaque
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
    REQUIRE(fragment.size() == 10 * 20 * 4);

    // Benchmark
    auto const start = std::chrono::high_resolution_clock::now();
    int const iterations = 1000000;
    size_t check = 0;
    for (int i = 0; i < iterations; ++i)
    {
        auto f = rasterizedImage->fragment(CellLocation { .line = LineOffset(5), .column = ColumnOffset(5) });
        check += f.size();
        // Verify center pixel is white (part of image)
        if (f.size() > 0)
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
