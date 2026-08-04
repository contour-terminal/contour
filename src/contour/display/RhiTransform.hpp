// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QSize>
#include <QtGui/QMatrix4x4>

namespace contour::display
{

/// Resolves the device→logical scale for a frame from the frame's OWN geometry.
///
/// This is the divisor composeItemToClip() needs, and it must be *exactly* the ratio the scene graph used
/// to build the projection it is composed with — otherwise the rasterizer's device-pixel vertices are
/// scaled by the discrepancy. That is not a cosmetic error: the glyph atlas is sampled with
/// QRhiSampler::Nearest so each texel maps 1:1 to a hardware pixel, and any scale other than 1 makes the
/// sample points drift across texel boundaries, duplicating some source columns and dropping others. The
/// result is the smeared stems, stray 1px bars and clipped cursor edges reported in #2040.
///
/// Deriving the ratio here, rather than reading a separately-maintained content scale, is what makes that
/// impossible by construction: both quantities come from the same frame. A caller-supplied DPR is a guess
/// about what Qt did — and the two DPRs Qt exposes (QWindow::devicePixelRatio() and
/// QQuickWindow::effectiveDevicePixelRatio()) are not the same number, so the guess can be wrong.
///
/// @param targetPixelSize   The render target's size in DEVICE pixels (QRhiRenderTarget::pixelSize()).
/// @param windowLogicalSize The window's size in LOGICAL pixels, the space Qt's matrices operate in.
/// @return The scale, or 1.0 when either extent is degenerate (nothing sensible to divide by).
[[nodiscard]] inline float deviceToLogicalScale(QSize targetPixelSize, QSizeF windowLogicalSize) noexcept
{
    // Width is the reference: a window is never zero-width while it is being rendered, and both extents
    // carry the same ratio. Height is the fallback for the degenerate-width case rather than a second
    // independent answer — one frame has one scale.
    if (targetPixelSize.width() > 0 && windowLogicalSize.width() > 0.0)
        return static_cast<float>(targetPixelSize.width() / windowLogicalSize.width());
    if (targetPixelSize.height() > 0 && windowLogicalSize.height() > 0.0)
        return static_cast<float>(targetPixelSize.height() / windowLogicalSize.height());
    return 1.0f;
}

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
/// @param dpr        The device→logical scale, which MUST be the ratio the scene graph built @p projection
///                   with — see deviceToLogicalScale(), which derives it from the frame instead of guessing.
///                   Values <= 0 are treated as identity (no scale).
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
