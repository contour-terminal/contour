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

/// Maps an item-relative inner scissor into render-target coordinates (both bottom-left origin,
/// device pixels).
///
/// vtrasterizer stages its transient inner scissor (smooth-scroll / cursor clip) relative to the ITEM
/// — its reference frame is renderSize(), the item's device-pixel extent — while the GPU scissor
/// addresses the render target (the window's framebuffer). For a pane that is not anchored at the
/// window's bottom-left the two frames differ, so the rectangle must be shifted by the item's offset
/// inside the target. Pure so the frame conversion is unit-testable without a command buffer.
/// @param rect               The item-relative scissor rectangle (bottom-left origin, device pixels).
/// @param itemLeftDevice     The item's left edge inside the render target, device pixels from the left.
/// @param itemTopDevice      The item's top edge inside the render target, device pixels from the TOP.
/// @param itemHeightDevice   The item's extent height in device pixels.
/// @param targetHeightDevice The render target's height in device pixels.
/// @return The same rectangle expressed in render-target coordinates.
[[nodiscard]] constexpr ScissorRect itemScissorToTarget(ScissorRect const& rect,
                                                        int itemLeftDevice,
                                                        int itemTopDevice,
                                                        int itemHeightDevice,
                                                        int targetHeightDevice) noexcept
{
    // Bottom-left-origin y offset of the item's bottom edge inside the target.
    auto const itemBottomOffset = targetHeightDevice - (itemTopDevice + itemHeightDevice);
    return ScissorRect {
        .x = rect.x + itemLeftDevice,
        .y = rect.y + itemBottomOffset,
        .width = rect.width,
        .height = rect.height,
    };
}

} // namespace contour::display
