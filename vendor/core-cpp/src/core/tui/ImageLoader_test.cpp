// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/testing/ScopedTempDir.hpp>
#include <core/tui/ImageLoader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ranges>
#include <vector>

using namespace core::tui;

// ============================================================================
// isImageExtension tests
// ============================================================================

TEST_CASE("ImageLoader.isImageExtension_recognizes_image_formats")
{
    CHECK(isImageExtension(".png"));
    CHECK(isImageExtension(".jpg"));
    CHECK(isImageExtension(".jpeg"));
    CHECK(isImageExtension(".gif"));
    CHECK(isImageExtension(".bmp"));
    CHECK(isImageExtension(".tga"));
    CHECK(isImageExtension(".psd"));
    CHECK(isImageExtension(".hdr"));
    CHECK(isImageExtension(".pnm"));
    CHECK(isImageExtension(".pgm"));
    CHECK(isImageExtension(".ppm"));
}

TEST_CASE("ImageLoader.isImageExtension_case_insensitive")
{
    CHECK(isImageExtension(".PNG"));
    CHECK(isImageExtension(".Jpg"));
    CHECK(isImageExtension(".JPEG"));
    CHECK(isImageExtension(".GIF"));
}

TEST_CASE("ImageLoader.isImageExtension_rejects_non_image_formats")
{
    CHECK_FALSE(isImageExtension(".cpp"));
    CHECK_FALSE(isImageExtension(".txt"));
    CHECK_FALSE(isImageExtension(".md"));
    CHECK_FALSE(isImageExtension(".h"));
    CHECK_FALSE(isImageExtension(".py"));
    CHECK_FALSE(isImageExtension(""));
}

// ============================================================================
// detectImageMediaType tests
// ============================================================================

TEST_CASE("ImageLoader.detectImageMediaType_png")
{
    // PNG signature: 89 50 4E 47 0D 0A 1A 0A
    auto const data = std::vector<std::uint8_t> { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00 };
    CHECK(detectImageMediaType(data) == "image/png");
}

TEST_CASE("ImageLoader.detectImageMediaType_jpeg")
{
    auto const data = std::vector<std::uint8_t> { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10 };
    CHECK(detectImageMediaType(data) == "image/jpeg");
}

TEST_CASE("ImageLoader.detectImageMediaType_gif87a")
{
    auto const data = std::vector<std::uint8_t> { 'G', 'I', 'F', '8', '7', 'a', 0x00 };
    CHECK(detectImageMediaType(data) == "image/gif");
}

TEST_CASE("ImageLoader.detectImageMediaType_gif89a")
{
    auto const data = std::vector<std::uint8_t> { 'G', 'I', 'F', '8', '9', 'a', 0x00 };
    CHECK(detectImageMediaType(data) == "image/gif");
}

TEST_CASE("ImageLoader.detectImageMediaType_bmp")
{
    auto const data = std::vector<std::uint8_t> { 'B', 'M', 0x00, 0x00, 0x00 };
    CHECK(detectImageMediaType(data) == "image/bmp");
}

TEST_CASE("ImageLoader.detectImageMediaType_webp")
{
    auto const data =
        std::vector<std::uint8_t> { 'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P' };
    CHECK(detectImageMediaType(data) == "image/webp");
}

TEST_CASE("ImageLoader.detectImageMediaType_unknown_text")
{
    auto const data = std::vector<std::uint8_t> { 'H', 'e', 'l', 'l', 'o' };
    CHECK(detectImageMediaType(data).empty());
}

TEST_CASE("ImageLoader.detectImageMediaType_too_short")
{
    auto const data = std::vector<std::uint8_t> { 0xFF, 0xD8 };
    CHECK(detectImageMediaType(data).empty());
}

TEST_CASE("ImageLoader.detectImageMediaType_empty")
{
    auto const data = std::vector<std::uint8_t> {};
    CHECK(detectImageMediaType(data).empty());
}

// ============================================================================
// loadImageFromMemory tests
// ============================================================================

TEST_CASE("ImageLoader.loadImageFromMemory_invalid_data")
{
    auto const garbage = std::vector<std::uint8_t> { 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE };
    auto const result = loadImageFromMemory(garbage);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains("Failed to load image from memory"));
}

TEST_CASE("ImageLoader.loadImageFromMemory_valid_ppm")
{
    // PPM P6 format: simple, no compression
    std::string header = "P6\n2 2\n255\n";
    std::vector<std::uint8_t> data(header.begin(), header.end());
    // 4 red pixels (RGB)
    for ([[maybe_unused]] auto const i: std::views::iota(0, 4))
    {
        data.push_back(255); // R
        data.push_back(0);   // G
        data.push_back(0);   // B
    }

    auto const result = loadImageFromMemory(data);
    REQUIRE(result.has_value());
    CHECK(result->width == 2);
    CHECK(result->height == 2);
    CHECK(result->pixels.size() == std::size_t { 2 } * 2 * 4); // RGBA

    // First pixel should be red (R=255, G=0, B=0, A=255)
    CHECK(result->pixels[0] == 255); // R
    CHECK(result->pixels[1] == 0);   // G
    CHECK(result->pixels[2] == 0);   // B
    CHECK(result->pixels[3] == 255); // A (opaque)
}

// ============================================================================
// loadImage tests
// ============================================================================

namespace
{

/// @brief Generates a valid PPM (P6) image file with 2x2 red pixels.
/// PPM is simpler to construct correctly than PNG.
auto generateValidPpm() -> std::vector<std::uint8_t>
{
    // PPM P6 format: simple, no compression
    std::string header = "P6\n2 2\n255\n";
    std::vector<std::uint8_t> data(header.begin(), header.end());
    // 4 red pixels (RGB)
    for ([[maybe_unused]] auto const i: std::views::iota(0, 4))
    {
        data.push_back(255); // R
        data.push_back(0);   // G
        data.push_back(0);   // B
    }
    return data;
}

auto writeTempFile(std::string_view name, std::vector<std::uint8_t> const& data) -> std::string
{
    auto const dir = std::filesystem::path("tmp");
    std::filesystem::create_directories(dir);
    auto const path = dir / name;
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path.string();
}

} // namespace

TEST_CASE("ImageLoader.loadImage_valid_ppm")
{
    auto const data = generateValidPpm();
    auto const path = writeTempFile("test_image.ppm", data);

    auto const result = loadImage(path);
    REQUIRE(result.has_value());
    CHECK(result->width == 2);
    CHECK(result->height == 2);
    CHECK(result->pixels.size() == std::size_t { 2 } * 2 * 4); // RGBA

    // First pixel should be red (R=255, G=0, B=0, A=255)
    CHECK(result->pixels[0] == 255); // R
    CHECK(result->pixels[1] == 0);   // G
    CHECK(result->pixels[2] == 0);   // B
    CHECK(result->pixels[3] == 255); // A (opaque)

    std::filesystem::remove(path);
}

TEST_CASE("ImageLoader.loadImage_reads_its_path_as_UTF-8")
{
    // The path is UTF-8, as every core-cpp string is. On Windows stb_image opened it with fopen(),
    // which reads the ANSI code page, so a file beyond ASCII was a file that did not exist.
    auto const temp = core::testing::ScopedTempDir { "core-tui-image-loader" };
    auto const file = temp.path() / std::filesystem::path { u8"Jürgen-Ω.ppm" };
    auto const data = generateValidPpm();
    std::ofstream { file, std::ios::binary }.write(reinterpret_cast<char const*>(data.data()),
                                                   static_cast<std::streamsize>(data.size()));

    auto const result = loadImage(core::pathToUtf8(file));
    REQUIRE(result.has_value());
    CHECK(result->width == 2);
    CHECK(result->height == 2);
}

TEST_CASE("ImageLoader.loadImage_nonexistent_file")
{
    auto const result = loadImage("/nonexistent/path/to/image.png");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains("Failed to load"));
}

TEST_CASE("ImageLoader.loadImage_invalid_data")
{
    std::vector<std::uint8_t> const garbage = { 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE };
    auto const path = writeTempFile("test_invalid.png", garbage);

    auto const result = loadImage(path);
    REQUIRE_FALSE(result.has_value());

    std::filesystem::remove(path);
}

// ============================================================================
// resizeImage tests
// ============================================================================

TEST_CASE("ImageLoader.resizeImage_downscale")
{
    // Create a 100x100 test image
    auto constexpr SrcWidth = 100;
    auto constexpr SrcHeight = 100;
    auto image = OwnedImage {
        .pixels = std::vector<std::uint8_t>(std::size_t { SrcWidth } * SrcHeight * 4, 128),
        .width = SrcWidth,
        .height = SrcHeight,
    };

    auto const result = resizeImage(image, 50, 50);
    REQUIRE(result.has_value());
    CHECK(result->width == 50);
    CHECK(result->height == 50);
    CHECK(result->pixels.size() == std::size_t { 50 } * 50 * 4);
}

TEST_CASE("ImageLoader.resizeImage_aspect_ratio_auto_height")
{
    auto image = OwnedImage {
        .pixels = std::vector<std::uint8_t>(std::size_t { 200 } * 100 * 4, 128),
        .width = 200,
        .height = 100,
    };

    // Target width=100, height=0 → should auto-compute height=50
    auto const result = resizeImage(image, 100, 0);
    REQUIRE(result.has_value());
    CHECK(result->width == 100);
    CHECK(result->height == 50);
}

TEST_CASE("ImageLoader.resizeImage_aspect_ratio_auto_width")
{
    auto image = OwnedImage {
        .pixels = std::vector<std::uint8_t>(std::size_t { 200 } * 100 * 4, 128),
        .width = 200,
        .height = 100,
    };

    // Target width=0, height=50 → should auto-compute width=100
    auto const result = resizeImage(image, 0, 50);
    REQUIRE(result.has_value());
    CHECK(result->width == 100);
    CHECK(result->height == 50);
}

TEST_CASE("ImageLoader.resizeImage_invalid_both_zero")
{
    auto image = OwnedImage {
        .pixels = std::vector<std::uint8_t>(std::size_t { 10 } * 10 * 4, 128),
        .width = 10,
        .height = 10,
    };

    auto const result = resizeImage(image, 0, 0);
    REQUIRE_FALSE(result.has_value());
}
