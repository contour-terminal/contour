// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/GlyphOutline.hpp>

#include <vtrasterizer/RegisteredGlyphPlacement.hpp>

#include <cstdint>
#include <vector>

namespace vtrasterizer
{

/// The top-left pixel of a window cut out of a glyph's render span.
struct OutlineWindowOrigin
{
    int x = 0;
    int y = 0;
};

/// Fills a TrueType outline, antialiased, into a window of its render span.
///
/// The contours are walked the TrueType way -- two on-curve points make a line, an off-curve point between
/// them a quadratic Bezier, and two off-curve points in a row imply an on-curve point halfway -- and filled
/// by the non-zero winding rule. Whatever of the outline lies outside the window is clipped, exactly: a
/// tile cut from a wide span holds what the whole span would show there.
/// @param outline The outline, in authored units, Y up.
/// @param transform Where the outline lands in its render span.
/// @param window The size of the window to fill.
/// @param origin Where in the span the window starts.
/// @return The window's coverage, row-major, top row first, 0 (clear) to 0xFF (ink).
[[nodiscard]] std::vector<uint8_t> rasterizeOutline(vtbackend::GlyphOutline const& outline,
                                                    GlyphSpanTransform const& transform,
                                                    vtbackend::ImageSize window,
                                                    OutlineWindowOrigin origin = {});

} // namespace vtrasterizer
