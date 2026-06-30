// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QMatrix4x4>

namespace contour::display
{

/// Composes the item-local→clip transform fed to the terminal's RHI vertex shader, correcting for the
/// device-pixel-ratio mismatch between Qt's scene graph and the terminal rasterizer.
///
/// Qt's QSGRenderNode contract places a vertex at `projection * nodeMatrix * vertex`, where both matrices
/// operate in **logical** (device-independent) pixels — the scene graph applies the device-pixel ratio
/// itself when rasterizing into the device-pixel render target. The terminal rasterizer, however, emits
/// vertices in **device** pixels (its cell metrics and glyph atlas are built at the content scale / DPR so
/// text is crisp 1:1 with hardware pixels, as on the master branch). Feeding device-pixel vertices straight
/// into the logical-space transform would scale the grid up by the DPR — an oversized font, the grid
/// overflowing past the status line, and (because the grid no longer matches the device-pixel mouse mapping)
/// off-by-DPR text selection. Pre-multiplying by a 1/DPR scale converts the device-pixel vertices back to
/// logical space, so the grid is positioned correctly while each device-resolution glyph texel still maps to
/// exactly one hardware pixel (no GPU up/downscaling → crisp).
///
/// @param projection The scene graph's projection matrix (scene/logical space → clip space).
/// @param nodeMatrix The node's model-view matrix (item-local/logical space → scene space).
/// @param dpr        The device-pixel ratio (content scale). Values <= 0 are treated as identity (no scale).
/// @return The combined matrix mapping the rasterizer's device-pixel vertices to clip space.
[[nodiscard]] inline QMatrix4x4 composeItemToClip(QMatrix4x4 const& projection,
                                                  QMatrix4x4 const& nodeMatrix,
                                                  float dpr) noexcept
{
    auto deviceToLogical = QMatrix4x4 {};
    if (dpr > 0.0f)
        deviceToLogical.scale(1.0f / dpr, 1.0f / dpr);
    return projection * nodeMatrix * deviceToLogical;
}

} // namespace contour::display
