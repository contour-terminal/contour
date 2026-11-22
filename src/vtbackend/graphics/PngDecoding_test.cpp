// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the libpng decoder: every kind of PNG, the files it refuses, and a kitty `f=100`
// transmission decoded by it end to end.

#include <vtbackend/graphics/PngDecoding.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/PngSamples.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;
using namespace vtbackend;
namespace samples = vtbackend::test::png_samples;

namespace
{
/// @return The bytes of @p text, as a decoder is handed them.
[[nodiscard]] std::span<uint8_t const> bytesOf(std::string_view text) noexcept
{
    return { reinterpret_cast<uint8_t const*>(text.data()), text.size() };
}

/// @return Why @p result is no image, or std::nullopt if it is one.
[[nodiscard]] std::optional<PngDecodeError> refusal(std::expected<DecodedPng, PngDecodeError> const& result)
{
    if (result.has_value())
        return std::nullopt;
    return result.error();
}

/// A decoder for the files that are not PNG, which counts its calls and decodes each to one white pixel.
struct OtherFormats
{
    std::shared_ptr<int> calls = std::make_shared<int>(0);

    [[nodiscard]] ImageDecoder decoder() const
    {
        return [calls = calls](
                   ImageFormat, std::span<uint8_t const>, ImageSize& size) -> std::optional<Image::Data> {
            ++*calls;
            size = ImageSize { Width(1), Height(1) };
            return Image::Data { 0xFF, 0xFF, 0xFF, 0xFF };
        };
    }
};
} // namespace

TEST_CASE("PngDecoding.decodes_every_kind_of_png_to_rgba", "[png]")
{
    struct Case
    {
        std::string_view name;
        std::span<uint8_t const> file;
        ImageSize size;
        std::vector<uint8_t> rgba;
    };
    // clang-format off
    auto const cases = std::vector<Case> {
        { .name = "8-bit RGBA",
          .file = samples::Rgba8Png,
          .size = ImageSize { Width(2), Height(2) },
          .rgba = { 255, 0, 0, 255,   0, 255, 0, 128,
                    0, 0, 255, 0,     255, 255, 255, 255 } },
        { .name = "8-bit RGB",
          .file = samples::Rgb8Png,
          .size = ImageSize { Width(2), Height(2) },
          .rgba = { 10, 20, 30, 255,  40, 50, 60, 255,
                    70, 80, 90, 255,  100, 110, 120, 255 } },
        { .name = "palette with transparency",
          .file = samples::PalettePng,
          .size = ImageSize { Width(2), Height(2) },
          .rgba = { 255, 0, 0, 255,   0, 255, 0, 0x80,
                    0, 0, 255, 255,   255, 0, 0, 255 } },
        { .name = "8-bit grayscale",
          .file = samples::Gray8Png,
          .size = ImageSize { Width(2), Height(2) },
          .rgba = { 0, 0, 0, 255,       85, 85, 85, 255,
                    170, 170, 170, 255, 255, 255, 255, 255 } },
        // Taken as sRGB, as an 8-bit file is, not as linear light: 0x8080 is 0x80, not a gamma-encoded 0xBC.
        { .name = "16-bit grayscale",
          .file = samples::Gray16Png,
          .size = ImageSize { Width(2), Height(2) },
          .rgba = { 0, 0, 0, 255,       0x80, 0x80, 0x80, 255,
                    255, 255, 255, 255, 0x12, 0x12, 0x12, 255 } },
        { .name = "Adam7-interlaced RGB",
          .file = samples::InterlacedPng,
          .size = ImageSize { Width(3), Height(3) },
          .rgba = { 0, 0, 100, 255,   10, 0, 101, 255,   20, 0, 102, 255,
                    0, 10, 103, 255,  10, 10, 104, 255,  20, 10, 105, 255,
                    0, 20, 106, 255,  10, 20, 107, 255,  20, 20, 108, 255 } },
    };
    // clang-format on
    for (auto const& testCase: cases)
    {
        INFO(testCase.name);
        CHECK(isPng(testCase.file));
        auto const decoded = decodePngImage(testCase.file, PngDecodingLimits {});
        REQUIRE(decoded.has_value());
        CHECK(decoded->size == testCase.size);
        CHECK(decoded->rgba == testCase.rgba);
    }
}

TEST_CASE("PngDecoding.data_without_the_signature_is_NotPng", "[png]")
{
    auto const files = std::vector<std::string_view> {
        ""sv,
        "\x89PNG"sv,                         // a prefix of the signature
        "GIF89a\x01\x00\x01\x00"sv,          // another format
        "\x89PNG\r\n\x1b\n\0\0\0\x0dIHDR"sv, // one byte off
    };
    for (auto const file: files)
    {
        INFO(std::format("{} bytes", file.size()));
        CHECK_FALSE(isPng(bytesOf(file)));
        CHECK(refusal(decodePngImage(bytesOf(file), PngDecodingLimits {})) == PngDecodeError::NotPng);
    }
}

TEST_CASE("PngDecoding.a_truncated_or_damaged_png_is_Corrupt", "[png]")
{
    auto const file = std::span<uint8_t const> { samples::Rgb8Png };
    // The signature alone; inside IHDR; IHDR whole but no IDAT; inside IDAT.
    for (auto const length: { size_t { 8 }, size_t { 20 }, size_t { 33 }, size_t { 50 } })
    {
        INFO(std::format("cut to {} bytes", length));
        CHECK(refusal(decodePngImage(file.first(length), PngDecodingLimits {})) == PngDecodeError::Corrupt);
    }

    // A flipped bit in IHDR's width, and in IDAT's compressed data: both fail their chunk's CRC.
    for (auto const offset: { size_t { 19 }, size_t { 45 } })
    {
        INFO(std::format("byte {} flipped", offset));
        auto damaged = std::vector<uint8_t>(file.begin(), file.end());
        damaged[offset] ^= 0x01;
        CHECK(refusal(decodePngImage(damaged, PngDecodingLimits {})) == PngDecodeError::Corrupt);
    }
}

TEST_CASE("PngDecoding.a_size_beyond_the_limit_is_refused_before_it_is_allocated", "[png]")
{
    // 60000x60000 decodes to 14.4 GB: refused for its header alone, which is all it carries the data for.
    CHECK(refusal(decodePngImage(samples::HugePng, PngDecodingLimits {})) == PngDecodeError::TooLarge);

    // The bound is exact: a 2x2 RGBA image takes 16 bytes.
    CHECK(refusal(decodePngImage(samples::Rgba8Png, PngDecodingLimits { .maxDecodedBytes = 15 }))
          == PngDecodeError::TooLarge);
    CHECK(decodePngImage(samples::Rgba8Png, PngDecodingLimits { .maxDecodedBytes = 16 }).has_value());
}

TEST_CASE("PngDecoder.decodes_png_itself_and_hands_on_everything_else", "[png]")
{
    auto const other = OtherFormats {};
    auto const decoder = ImageDecoder { PngDecoder { PngDecodingLimits {}, other.decoder() } };

    SECTION("a PNG")
    {
        auto size = ImageSize {};
        auto const pixels = decoder(ImageFormat::PNG, samples::Rgb8Png, size);
        REQUIRE(pixels.has_value());
        CHECK(size == ImageSize { Width(2), Height(2) });
        CHECK(pixels->size() == 2uz * 2uz * 4uz);
        CHECK(*other.calls == 0);
    }

    SECTION("a PNG libpng refuses is refused, not handed on")
    {
        auto size = ImageSize { Width(7), Height(7) };
        CHECK_FALSE(decoder(ImageFormat::PNG, samples::HugePng, size).has_value());
        CHECK_FALSE(decoder(ImageFormat::PNG, std::span { samples::Rgb8Png }.first(50), size).has_value());
        CHECK(size == ImageSize { Width(7), Height(7) });
        CHECK(*other.calls == 0);
    }

    SECTION("another file sent as PNG")
    {
        auto size = ImageSize {};
        auto const pixels = decoder(ImageFormat::PNG, bytesOf("GIF89a"sv), size);
        CHECK(pixels == Image::Data { 0xFF, 0xFF, 0xFF, 0xFF });
        CHECK(size == ImageSize { Width(1), Height(1) });
        CHECK(*other.calls == 1);
    }

    SECTION("only ImageFormat::PNG is decoded")
    {
        auto size = ImageSize {};
        CHECK_FALSE(decoder(ImageFormat::RGBA, samples::Rgb8Png, size).has_value());
        CHECK(*other.calls == 0);
    }
}

TEST_CASE("PngDecoder.without_a_second_decoder_refuses_what_is_no_png", "[png]")
{
    auto const decoder = ImageDecoder { PngDecoder {} };
    auto size = ImageSize {};
    CHECK_FALSE(decoder(ImageFormat::PNG, bytesOf("GIF89a"sv), size).has_value());
    CHECK(decoder(ImageFormat::PNG, samples::Gray8Png, size).has_value());
}

TEST_CASE("PngDecoder.a_kitty_png_transmission_is_shown", "[png][kitty]")
{
    // The terminal of a build without Qt, as `contour daemon` hosts it: it decodes `f=100` with libpng.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) },
                                           ImageDecoder { PngDecoder {} } };
    mock.terminal.setCellPixelSize(ImageSize { Width(1), Height(1) });
    auto const file =
        std::string_view { reinterpret_cast<char const*>(samples::Rgb8Png.data()), samples::Rgb8Png.size() };

    mock.writeToScreen(std::format("\033_Ga=T,f=100,i=1;{}\033\\", core::base64::encode(file)));

    CHECK(mock.terminal.peekInput() == "\033_Gi=1;OK\033\\");
    auto const fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment);
    auto const& image = fragment->rasterizedImage().image();
    CHECK(image.format() == ImageFormat::RGBA);
    CHECK(image.size() == ImageSize { Width(2), Height(2) });
    CHECK(image.data()
          == Image::Data { 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255 });
}
