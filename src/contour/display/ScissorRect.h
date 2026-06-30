// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

namespace contour::display
{

/// An axis-aligned clip rectangle in OpenGL scissor coordinates: bottom-left origin, device pixels.
///
/// Kept free of Qt/OpenGL so the clip geometry can be unit-tested without a GL context.
struct ScissorRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    /// @returns true if the rectangle covers no pixels (so a scissor with it clips everything away).
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    /// Intersects this rectangle with @p other.
    /// @param other The rectangle to clip against (e.g. an installed outer scissor).
    /// @return The overlapping region, or an empty rectangle (covering no pixels) if they are
    ///         disjoint. The result is never larger than either input, so nesting an inner scissor
    ///         inside an outer one can only ever shrink the clipped region, never enlarge it.
    [[nodiscard]] constexpr ScissorRect intersect(ScissorRect const& other) const noexcept
    {
        auto const left = std::max(x, other.x);
        auto const bottom = std::max(y, other.y);
        auto const right = std::min(x + width, other.x + other.width);
        auto const top = std::min(y + height, other.y + other.height);
        return ScissorRect {
            .x = left,
            .y = bottom,
            .width = std::max(0, right - left),
            .height = std::max(0, top - bottom),
        };
    }
};

} // namespace contour::display
