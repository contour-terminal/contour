// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Error.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace core::tui
{

/// @brief RGBA image data for sixel encoding.
struct ImageData
{
    std::span<std::uint8_t const> pixels; ///< RGBA pixel data (4 bytes per pixel).
    int width = 0;                        ///< Image width in pixels.
    int height = 0;                       ///< Image height in pixels.
};

/// @brief Encodes RGBA image data to a sixel string.
///
/// Performs color quantization (median-cut) to reduce to the specified number
/// of colors, then encodes the image in sixel format. The result can be
/// passed to TerminalOutput::writeSixel().
///
/// @param image The source image data (RGBA, 4 bytes per pixel).
/// @param maxColors Maximum number of colors in the palette (1-256, default 256).
/// @return The sixel-encoded string, or an error.
[[nodiscard]] auto encodeSixel(ImageData const& image, int maxColors = 256) -> Result<std::string>;

} // namespace core::tui
