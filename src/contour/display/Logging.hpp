// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LogStore.hpp>

namespace contour::display
{

auto inline const displayLog =
    logstore::Category("gui.display", "Logs display driver details (e.g. OpenGL).");

/// Reports frames whose composed item→clip transform does not map one rasterizer device pixel onto
/// one hardware pixel.
///
/// Its own category rather than part of gui.display, which logs steadily and would bury this:
/// enabled alone the log is SILENT unless the invariant breaks, so any output at all is a finding.
///
/// The glyph atlas is sampled with QRhiSampler::Nearest, which is exact only at scale 1 — at any
/// other scale the sample points drift across texel boundaries and whole glyph columns duplicate or
/// drop rather than merely blurring. @see #2040, RhiTransform.h.
auto inline const geometryProbeLog = logstore::Category(
    "gui.display.geometry", "Logs frames whose device-pixel scale is not 1:1 (see #2040).");

/// Reports glyph quads that do not span exactly as many device pixels as the atlas region they
/// sample has texels.
///
/// Its own category, silent unless the invariant breaks, so any output is a finding.
///
/// Distinct from gui.display.geometry, which checks the item→clip TRANSFORM: a frame can have a
/// perfectly correct transform and still sample wrongly, because a quad's size is read at RENDER
/// time from targetSize/bitmapSize while its texture region was baked into normalizedLocation at
/// UPLOAD time. Those are different moments, and a cell-size change between them desynchronizes the
/// pair. @see #2040, RhiRenderer::renderTile.
auto inline const samplingProbeLog = logstore::Category(
    "gui.display.sampling", "Logs glyph quads whose pixel span does not match their texel count.");

/// Reports frames whose scissor does not cover the pane being drawn.
///
/// Silent unless the invariant breaks. The clip is composed from inputs captured at different moments
/// (sync-point item extent and origin, the scene graph's node scissor, and the live window target), so
/// a resize can leave them describing different window sizes -- and the strip the clip loses keeps
/// whatever the swapchain buffer already held. @see #2040, RhiRenderer::applyScissor.
auto inline const clipProbeLog =
    logstore::Category("gui.display.clip", "Logs frames whose scissor does not cover the pane.");

} // namespace contour::display
