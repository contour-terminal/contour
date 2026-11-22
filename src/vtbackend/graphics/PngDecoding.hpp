// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace vtbackend
{

/// The default for PngDecodingLimits::maxDecodedBytes: the default kitty graphics storage quota
/// (Settings::kittyImageStorageQuota), which no larger image would fit into anyway.
inline constexpr auto DefaultMaxDecodedPngBytes = static_cast<size_t>(128 * 1024 * 1024);

/// What a PNG decoder holds a file to. A PNG is untrusted input: its header states any size up to
/// 2^31 - 1 pixels a side in a few dozen bytes.
struct PngDecodingLimits
{
    /// The most bytes the decoded RGBA pixels (4 a pixel) may take. Checked against the size the file
    /// states, before anything is allocated for it.
    size_t maxDecodedBytes = DefaultMaxDecodedPngBytes;
};

/// Why a PNG could not be decoded.
enum class PngDecodeError : uint8_t
{
    NotPng,   ///< The data does not start with the PNG signature.
    Corrupt,  ///< A PNG signature, but no complete, valid PNG behind it.
    TooLarge, ///< A valid header stating a size of zero, or more pixels than the limits allow.
};

/// A decoded PNG.
struct DecodedPng
{
    ImageSize size;   ///< The image's size, as the file states it.
    Image::Data rgba; ///< The pixels, row by row, 4 bytes (R, G, B, A, not premultiplied) each.
};

/// @param data The bytes to test.
/// @return Whether @p data starts with the eight-byte PNG signature.
[[nodiscard]] bool isPng(std::span<uint8_t const> data) noexcept;

/// Decodes a PNG file to RGBA pixels with libpng. Every PNG colour type and bit depth is accepted,
/// interlaced or not; a 16-bit file is reduced to 8 bits, and one without colour-space chunks is taken as
/// sRGB, as an 8-bit one is.
///
/// Safe to call concurrently: it shares no state.
/// @param data The PNG file.
/// @param limits What the file is held to.
/// @return The image, or why there is none.
[[nodiscard]] std::expected<DecodedPng, PngDecodeError> decodePngImage(std::span<uint8_t const> data,
                                                                       PngDecodingLimits limits);

/// The ImageDecoder of a build without Qt, and the PNG half of one with it: decodes PNG files with libpng
/// (decodePngImage()) and hands any other file to a second decoder, if it was given one.
///
/// A file with the PNG signature is libpng's alone: one it refuses is refused, not handed on, so a GUI
/// build and a GUI-less daemon decode a PNG the same way and hold it to the same limits. The second decoder
/// is for the files that iTerm2 inline images and GIP hand over as ImageFormat::PNG without being one
/// (JPEG, GIF, ...).
class PngDecoder
{
  public:
    /// @param limits What every PNG file is held to.
    /// @param otherFormats What decodes a file that is not a PNG; absent, such a file is refused.
    explicit PngDecoder(PngDecodingLimits limits = {}, ImageDecoder otherFormats = {});

    /// Matches ImageDecoder.
    /// @param format The format the file was sent as; only ImageFormat::PNG is decoded.
    /// @param data The file.
    /// @param size Set to the decoded image's size; left alone on a refusal.
    /// @return The RGBA pixels, or std::nullopt for a file that could not be decoded.
    [[nodiscard]] std::optional<Image::Data> operator()(ImageFormat format,
                                                        std::span<uint8_t const> data,
                                                        ImageSize& size) const;

  private:
    PngDecodingLimits _limits;
    ImageDecoder _otherFormats;
};

} // namespace vtbackend
