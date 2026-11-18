// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/VectorGlyph.hpp>

#include <vtrasterizer/PathGeometry.hpp>

#include <span>
#include <vector>

namespace vtrasterizer
{

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
/// @return Per pixel, row-major and top row first, the share of it that is inside: 0 to 1.
[[nodiscard]] std::vector<float> pathCoverage(std::span<Polyline const> polygons,
                                              vtbackend::VectorFillRule rule,
                                              vtbackend::ImageSize window);

} // namespace vtrasterizer
