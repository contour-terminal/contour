// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/VectorGlyph.hpp>

#include <vtrasterizer/PathGeometry.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace vtrasterizer
{

/// How much of each pixel a shape covers, for the pixels of a window the shape can reach: a rectangle of
/// the window, and nothing outside it. A small shape in a large window is a small mask.
struct CoverageMask
{
    size_t left = 0;           ///< The rectangle's first column in the window.
    size_t top = 0;            ///< Its first row.
    size_t width = 0;          ///< How many columns it spans.
    size_t height = 0;         ///< How many rows.
    std::vector<float> values; ///< Per pixel of the rectangle, row-major, the share of it covered: 0 to 1.

    /// @param x A column of the window.
    /// @param y A row of the window.
    /// @return The share of that pixel that is covered; 0 outside the rectangle.
    [[nodiscard]] float at(size_t x, size_t y) const noexcept
    {
        return x >= left && x - left < width && y >= top && y - top < height
                   ? values[((y - top) * width) + (x - left)]
                   : 0.0F;
    }

    /// @param window The window the mask was made for.
    /// @return The coverage of every pixel of the window, row-major and top row first.
    [[nodiscard]] std::vector<float> expanded(vtbackend::ImageSize window) const;
};

/// How much of each pixel of a window a set of polygons covers, antialiased.
///
/// Each polyline is filled as a closed polygon, whatever its closure says: a fill closes every subpath.
/// Coverage is exact area, not samples: each edge adds the signed area it sweeps to the pixels of its rows,
/// and a row summed left to right is each pixel's winding weighted by how much of the pixel has it. What
/// lies outside the window is clipped, exactly, so a tile cut from a larger picture holds what the picture
/// shows there.
/// @param polygons The polygons, in pixels, the window's top-left corner at the origin.
/// @param rule Which points are inside.
/// @param window The window's size.
/// @return The coverage of the part of the window the polygons' bounds reach.
[[nodiscard]] CoverageMask pathCoverage(std::span<Polyline const> polygons,
                                        vtbackend::VectorFillRule rule,
                                        vtbackend::ImageSize window);

} // namespace vtrasterizer
