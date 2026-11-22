// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace contour::display
{

/// Decodes an image file to RGBA pixels with QImage, whatever format QImage recognises in it: iTerm2 inline
/// images and GIP hand any image file (JPEG, GIF, ...) over as ImageFormat::PNG. Needs no QGuiApplication,
/// and is safe to call from any thread.
/// @param format The source format; only ImageFormat::PNG is decoded.
/// @param data The encoded image.
/// @param size Set to the decoded image's size.
/// @return The RGBA pixels, or std::nullopt for another format or data QImage cannot decode.
[[nodiscard]] std::optional<vtbackend::Image::Data> decodeImage(vtbackend::ImageFormat format,
                                                                std::span<uint8_t const> data,
                                                                vtbackend::ImageSize& size);

/// @return The vtbackend::ImageDecoder every terminal of a GUI build is given, the in-process ones and a
///         daemon's hosted ones alike: libpng decodes a PNG, as in a build without Qt, so both decode it
///         the same way and hold it to the same limits; QImage (decodeImage()) decodes any other file.
[[nodiscard]] vtbackend::ImageDecoder makeImageDecoder();

} // namespace contour::display
