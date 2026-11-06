// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.hpp>

#include <cstdint>
#include <optional>

namespace vthost
{

/// The largest cell extent, in pixels, taken from outside (wire, snapshot, handover image, config).
inline constexpr uint32_t MaxCellPixelExtent = 1024;

/// Bounds a cell size that entered from outside before it reaches a terminal.
/// @param width The cell's width in pixels.
/// @param height The cell's height in pixels.
/// @return @p width × @p height as a cell size, or nothing unless each extent is in 1..MaxCellPixelExtent.
[[nodiscard]] constexpr std::optional<vtpty::ImageSize> sanePixelSize(uint32_t width,
                                                                      uint32_t height) noexcept
{
    auto const inRange = [](uint32_t extent) {
        return extent >= 1 && extent <= MaxCellPixelExtent;
    };
    if (!inRange(width) || !inRange(height))
        return std::nullopt;
    return vtpty::ImageSize { .width = vtpty::Width::cast_from(width),
                              .height = vtpty::Height::cast_from(height) };
}

} // namespace vthost
