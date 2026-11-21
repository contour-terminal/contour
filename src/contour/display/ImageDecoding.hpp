// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace contour::display
{

/// Decodes a PNG image to RGBA pixels with QImage: the vtbackend::ImageDecoder every terminal of a GUI build
/// is given, the in-process ones by their display and a daemon's hosted ones by `contour daemon`. Needs no
/// QGuiApplication, and is safe to call from any thread.
/// @param format The source format; only ImageFormat::PNG is decoded.
/// @param data The encoded image.
/// @param size Set to the decoded image's size.
/// @return The RGBA pixels, or std::nullopt for another format or data that is no decodable PNG.
[[nodiscard]] std::optional<vtbackend::Image::Data> decodeImage(vtbackend::ImageFormat format,
                                                                std::span<uint8_t const> data,
                                                                vtbackend::ImageSize& size);

} // namespace contour::display
