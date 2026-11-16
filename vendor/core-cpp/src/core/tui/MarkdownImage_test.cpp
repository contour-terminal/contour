// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ScopedTempDir.hpp>
#include <core/tui/FilesystemImageProvider.hpp>
#include <core/tui/ImageLoader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace core::tui;

namespace
{

/// @brief A valid 2x2 red PPM (P6) image.
auto ppmBytes() -> std::vector<std::uint8_t>
{
    auto const header = std::string_view("P6\n2 2\n255\n");
    auto data = std::vector<std::uint8_t>(header.begin(), header.end());
    for ([[maybe_unused]] auto const pixel: { 0, 1, 2, 3 })
    {
        data.push_back(255);
        data.push_back(0);
        data.push_back(0);
    }
    return data;
}

/// @brief A valid 2x2 red PNG (8-bit RGB, zlib-compressed IDAT).
auto pngBytes() -> std::vector<std::uint8_t>
{
    return { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
             0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xFD, 0xD4, 0x9A,
             0x73, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
             0x9F, 0x01, 0x8C, 0xFF, 0x33, 0x30, 0x00, 0x00, 0x1F, 0xEE, 0x03, 0xFD, 0x35, 0x1B, 0x00, 0x33,
             0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82 };
}

/// @brief A valid 2x2 24-bit BMP.
auto bmpBytes() -> std::vector<std::uint8_t>
{
    return { 0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
             0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
             0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
             0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00 };
}

/// @brief A valid 2x2 GIF87a image.
auto gifBytes() -> std::vector<std::uint8_t>
{
    return { 0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x02, 0x00, 0x02, 0x00, 0xF0, 0x00,
             0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x2C, 0x00, 0x00, 0x00, 0x00,
             0x02, 0x00, 0x02, 0x00, 0x00, 0x02, 0x03, 0x04, 0x80, 0x02, 0x00, 0x3B };
}

/// @brief Creates a unique temporary directory for one test case.
auto makeTempDir(std::string_view name) -> std::filesystem::path
{
    auto const dir = std::filesystem::temp_directory_path() / "core-tui-markdown-image" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// @brief Writes @p data into @p dir under @p name and returns the file name.
auto writeFile(std::filesystem::path const& dir, std::string_view name, std::vector<std::uint8_t> const& data)
    -> std::string
{
    auto stream = std::ofstream(dir / name, std::ios::binary);
    stream.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    return std::string(name);
}

/// @brief Builds a provider rooted at @p dir with 8x16 cells and Sixel enabled.
auto makeProvider(std::filesystem::path dir, int maxColumns = 80) -> FilesystemImageProvider
{
    return FilesystemImageProvider(
        ImageRenderConfig {
            .baseDir = std::move(dir), .cellWidthPx = 8, .cellHeightPx = 16, .maxColumns = maxColumns },
        [] { return true; });
}

} // namespace

// ============================================================================
// Capability and remote-source handling
// ============================================================================

TEST_CASE("FilesystemImageProvider.supportsSixel_reflects_constructor_flag")
{
    CHECK(FilesystemImageProvider(ImageRenderConfig {}, [] { return true; }).supportsSixel());
    CHECK_FALSE(FilesystemImageProvider(ImageRenderConfig {}, [] { return false; }).supportsSixel());
}

TEST_CASE("FilesystemImageProvider.remote_sources_rejected_without_disk_access")
{
    // baseDir deliberately does not exist: a remote source must never touch it.
    auto provider = makeProvider("/nonexistent-image-dir");

    CHECK_FALSE(provider.prepare("https://x.com/a.png", std::nullopt).has_value());
    CHECK_FALSE(provider.prepare("http://x.com/a.png", std::nullopt).has_value());
    CHECK_FALSE(provider.prepare("//cdn.x.com/a.png", std::nullopt).has_value());
    CHECK_FALSE(provider.prepare("data:image/png;base64,AAAA", std::nullopt).has_value());
}

TEST_CASE("FilesystemImageProvider.isRemoteImageSource_classification")
{
    CHECK(isRemoteImageSource("https://x.com/a.png"));
    CHECK(isRemoteImageSource("http://x.com/a.png"));
    CHECK(isRemoteImageSource("ftp://x.com/a.png"));
    CHECK(isRemoteImageSource("//cdn.x/a.png"));
    CHECK(isRemoteImageSource("data:image/png;base64,AA"));
    CHECK_FALSE(isRemoteImageSource("logo.png"));
    CHECK_FALSE(isRemoteImageSource("img/logo.png"));
    CHECK_FALSE(isRemoteImageSource("/abs/logo.png"));
}

TEST_CASE("FilesystemImageProvider.empty_source_is_an_error")
{
    auto provider = makeProvider("/tmp");
    CHECK_FALSE(provider.prepare("", std::nullopt).has_value());
}

// ============================================================================
// Path resolution
// ============================================================================

TEST_CASE("FilesystemImageProvider.relative_path_resolves_against_baseDir")
{
    auto provider = makeProvider("/base/dir");

    auto const result = provider.prepare("img/logo.png", std::nullopt);

    REQUIRE_FALSE(result.has_value());
    // The provider joins with the platform's preferred separator ('\' on Windows),
    // so the expected path must be built the same way.
    auto const expected = (std::filesystem::path("/base/dir") / "img/logo.png").string();
    CHECK(result.error().contains(expected));
}

TEST_CASE("FilesystemImageProvider.absolute_path_is_not_joined_to_baseDir")
{
    auto provider = makeProvider("/base/dir");

    auto const result = provider.prepare("/elsewhere/logo.png", std::nullopt);

    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().contains("/base/dir"));
    CHECK(result.error().contains("/elsewhere/logo.png"));
}

TEST_CASE("FilesystemImageProvider.reads_a_source_beyond_ASCII_as_UTF-8")
{
    // A markdown image source is UTF-8. On Windows the narrow constructor read it in the ANSI code
    // page and stb_image opened the result with fopen(), which reads that code page too, so a name
    // beyond ASCII named a file that did not exist; and path::string() threw for one the code page
    // cannot spell while the refusal was being spelled.
    auto const temp = core::testing::ScopedTempDir { "core-tui-markdown-image" };
    auto const dir = temp.path() / std::filesystem::path { u8"Jürgen" };
    std::filesystem::create_directories(dir);
    auto const bytes = ppmBytes();
    std::ofstream { dir / std::filesystem::path { u8"Ω.ppm" }, std::ios::binary }.write(
        reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto provider = makeProvider(dir);

    auto const decoded = provider.prepare("\xCE\xA9.ppm", std::nullopt);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->sixel.empty());

    auto const missing = provider.prepare("\xE6\x97\xA5\xE6\x9C\xAC.png", std::nullopt);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().contains("J\xC3\xBCrgen"));
    CHECK(missing.error().contains("\xE6\x97\xA5\xE6\x9C\xAC.png"));
}

TEST_CASE("FilesystemImageProvider.refuses_a_source_that_is_not_UTF-8")
{
    // A markdown document is text from outside the program. "Ω" spelled in CP-1253 (0xD9) followed
    // by ASCII starts no UTF-8 sequence, so on Windows it names no file: it is refused, as a missing
    // file is, rather than thrown. A POSIX path is bytes, so there it names a file that is missing.
    auto provider = makeProvider("/base/dir");

    auto const refused = provider.prepare("\xD9.ppm", std::nullopt);

    REQUIRE_FALSE(refused.has_value());
#ifdef _WIN32
    CHECK(refused.error() == "image source is not UTF-8: \\xd9.ppm");
#endif
}

TEST_CASE("FilesystemImageProvider.missing_local_file_is_an_error")
{
    auto const dir = makeTempDir("missing");
    auto provider = makeProvider(dir);

    CHECK_FALSE(provider.prepare("nope.png", std::nullopt).has_value());
}

TEST_CASE("FilesystemImageProvider.invalid_image_data_is_an_error")
{
    auto const dir = makeTempDir("garbage");
    // The extension passes the gate; stb sniffs the content and rejects it.
    auto const name = writeFile(dir, "broken.png", { 'n', 'o', 't', 'a', 'p', 'n', 'g' });
    auto provider = makeProvider(dir);

    CHECK_FALSE(provider.prepare(name, std::nullopt).has_value());
}

// ============================================================================
// Format parity with the `cat` builtin
// ============================================================================

TEST_CASE("FilesystemImageProvider.gate_matches_isImageExtension")
{
    auto provider = makeProvider("/base");

    // Every extension the shared predicate accepts must clear the provider's gate.
    // The file is absent, so an accepted extension fails at *load*, never at the gate.
    for (auto const* ext: { ".png",
                            ".jpg",
                            ".jpeg",
                            ".gif",
                            ".bmp",
                            ".tga",
                            ".psd",
                            ".hdr",
                            ".pnm",
                            ".pgm",
                            ".ppm",
                            ".PNG",
                            ".JPG" })
    {
        REQUIRE(isImageExtension(ext));
        auto const result = provider.prepare(std::string("x") + ext, std::nullopt);
        REQUIRE_FALSE(result.has_value());
        INFO("extension: " << ext);
        CHECK_FALSE(result.error().contains("unsupported image format"));
    }

    // Everything the predicate rejects must be refused at the gate.
    for (auto const* ext: { ".svg", ".webp", ".txt", ".md", "" })
    {
        REQUIRE_FALSE(isImageExtension(ext));
        auto const result = provider.prepare(std::string("x") + ext, std::nullopt);
        REQUIRE_FALSE(result.has_value());
        INFO("extension: " << ext);
        CHECK(result.error().contains("unsupported image format"));
    }
}

TEST_CASE("FilesystemImageProvider.svg_falls_back")
{
    auto const dir = makeTempDir("svg");
    auto const name = writeFile(dir, "logo.svg", { '<', 's', 'v', 'g', '/', '>' });
    auto provider = makeProvider(dir);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains("unsupported image format"));
}

TEST_CASE("FilesystemImageProvider.webp_falls_back_despite_recognised_magic")
{
    auto const dir = makeTempDir("webp");
    // detectImageMediaType() knows this magic, but stb cannot decode WebP.
    auto const data = std::vector<std::uint8_t> { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P' };
    CHECK(detectImageMediaType(data) == "image/webp");
    auto const name = writeFile(dir, "logo.webp", data);
    auto provider = makeProvider(dir);

    CHECK_FALSE(provider.prepare(name, std::nullopt).has_value());
}

// ============================================================================
// Real decoding, per format
// ============================================================================

TEST_CASE("FilesystemImageProvider.decodes_ppm")
{
    auto const dir = makeTempDir("ppm");
    auto const name = writeFile(dir, "img.ppm", ppmBytes());
    auto provider = makeProvider(dir);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->sixel.empty());
    CHECK(result->cellWidth == 1);  // 2px / 8px per cell, rounded up
    CHECK(result->cellHeight == 1); // 2px / 16px per cell, rounded up
}

TEST_CASE("FilesystemImageProvider.decodes_png")
{
    auto const dir = makeTempDir("png");
    auto const name = writeFile(dir, "img.png", pngBytes());
    auto provider = makeProvider(dir);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->sixel.empty());
    CHECK(result->cellWidth == 1);
    CHECK(result->cellHeight == 1);
}

TEST_CASE("FilesystemImageProvider.decodes_bmp")
{
    auto const dir = makeTempDir("bmp");
    auto const name = writeFile(dir, "img.bmp", bmpBytes());
    auto provider = makeProvider(dir);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->sixel.empty());
}

TEST_CASE("FilesystemImageProvider.decodes_gif")
{
    auto const dir = makeTempDir("gif");
    auto const name = writeFile(dir, "img.gif", gifBytes());
    auto provider = makeProvider(dir);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->sixel.empty());
}

TEST_CASE("FilesystemImageProvider.sixel_payload_is_unframed")
{
    auto const dir = makeTempDir("unframed");
    auto const name = writeFile(dir, "img.ppm", ppmBytes());
    auto provider = makeProvider(dir);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value());
    // TerminalOutput::writeSixel() supplies the DCS framing.
    CHECK_FALSE(result->sixel.contains("\033P"));
    CHECK_FALSE(result->sixel.contains("\033\\"));
}

// ============================================================================
// Sizing
// ============================================================================

TEST_CASE("FilesystemImageProvider.zero_cell_metrics_use_fallback")
{
    auto const dir = makeTempDir("zerocell");
    auto const name = writeFile(dir, "img.ppm", ppmBytes());
    auto provider = FilesystemImageProvider(
        ImageRenderConfig { .baseDir = dir, .cellWidthPx = 0, .cellHeightPx = 0, .maxColumns = 80 },
        [] { return true; });

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value()); // no divide-by-zero
    CHECK(result->cellWidth == 1);
    CHECK(result->cellHeight == 1);
}

TEST_CASE("FilesystemImageProvider.never_upscales_small_image")
{
    auto const dir = makeTempDir("noupscale");
    auto const name = writeFile(dir, "img.ppm", ppmBytes()); // 2x2 px

    auto provider = makeProvider(dir);
    auto const result = provider.prepare(name, 9999);

    REQUIRE(result.has_value());
    CHECK(result->cellWidth == 1); // still 2px wide
}

TEST_CASE("FilesystemImageProvider.width_attribute_clamped_to_max_columns")
{
    auto const dir = makeTempDir("clamp");
    auto const name = writeFile(dir, "img.ppm", ppmBytes());
    auto provider = makeProvider(dir, /*maxColumns*/ 4);

    auto const result = provider.prepare(name, 9999);

    REQUIRE(result.has_value());
    CHECK(result->cellWidth <= 4);
}

TEST_CASE("FilesystemImageProvider.auto_fit_downscales_wide_image")
{
    auto const dir = makeTempDir("autofit");
    // 64x1 px image; with 8px cells and maxColumns=4 the fit width is 32px => 4 cells.
    auto header = std::string_view("P6\n64 1\n255\n");
    auto data = std::vector<std::uint8_t>(header.begin(), header.end());
    for ([[maybe_unused]] auto const column: std::views::iota(0, 64))
    {
        data.push_back(0);
        data.push_back(128);
        data.push_back(255);
    }
    auto const name = writeFile(dir, "wide.ppm", data);
    auto provider = makeProvider(dir, /*maxColumns*/ 4);

    auto const result = provider.prepare(name, std::nullopt);

    REQUIRE(result.has_value());
    CHECK(result->cellWidth == 4);
}
