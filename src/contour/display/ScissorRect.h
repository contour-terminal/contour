// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <optional>

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

/// Computes the effective scissor clip for a draw from the transient inner scissor (smooth-scroll / cursor
/// clip pushed by vtrasterizer's Renderer) and the scene graph's per-node clip (Qt's RenderState scissor).
///
/// Pure, GPU-free policy so the clip-nesting decision is unit-testable without a command buffer. Nesting can
/// only ever shrink the clipped region, so when both are present the result is their intersection; when only
/// one is present it is used as-is; when neither is present std::nullopt signals "no clip" (the caller
/// scissors to the full render target — a pipeline that declares UsesScissor must be given an explicit
/// scissor every draw, else it inherits a previous node's stale scissor).
/// @param innerScissor The transient inner scissor for this draw, if any (bottom-left-origin device pixels).
/// @param nodeScissor  The scene graph's clip rectangle for this node, if any (same coordinate space).
/// @return The effective clip rectangle, or std::nullopt when no clip applies.
[[nodiscard]] constexpr std::optional<ScissorRect> computeEffectiveClip(
    std::optional<ScissorRect> const& innerScissor, std::optional<ScissorRect> const& nodeScissor) noexcept
{
    if (innerScissor.has_value() && nodeScissor.has_value())
        return innerScissor->intersect(*nodeScissor);
    if (innerScissor.has_value())
        return innerScissor;
    if (nodeScissor.has_value())
        return nodeScissor;
    return std::nullopt;
}

} // namespace contour::display
