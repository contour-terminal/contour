// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace vthost
{

/// The largest cell extent, in pixels, taken from outside (wire, snapshot, handover image, config).
inline constexpr uint32_t MaxCellPixelExtent = 1024;

/// The VT340's text cell, in pixels: the cell size every hosted terminal lays images out on before any
/// client reported one.
inline constexpr vtpty::ImageSize DefaultCellPixelSize { .width = vtpty::Width(10),
                                                         .height = vtpty::Height(20) };

/// Bounds a cell size that entered from outside before it reaches a terminal.
/// @param width The cell's width in pixels; any value a source can hold, a negative one included.
/// @param height The cell's height in pixels; likewise.
/// @return @p width × @p height as a cell size, or nothing unless each extent is in 1..MaxCellPixelExtent.
[[nodiscard]] constexpr std::optional<vtpty::ImageSize> sanePixelSize(int64_t width, int64_t height) noexcept
{
    auto const inRange = [](int64_t extent) {
        return extent >= 1 && std::cmp_less_equal(extent, MaxCellPixelExtent);
    };
    if (!inRange(width) || !inRange(height))
        return std::nullopt;
    return vtpty::ImageSize { .width = vtpty::Width::cast_from(width),
                              .height = vtpty::Height::cast_from(height) };
}

} // namespace vthost
