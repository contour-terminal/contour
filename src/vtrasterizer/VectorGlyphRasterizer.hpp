// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/VectorGlyph.hpp>

#include <vtrasterizer/RegisteredGlyphPlacement.hpp>

#include <cstdint>
#include <vector>

namespace vtrasterizer
{

/// The top-left pixel of a window cut out of a glyph's render span.
struct GlyphWindowOrigin
{
    int x = 0;
    int y = 0;
};

/// What the pixels of a rasterized glyph are.
enum class GlyphPixels : uint8_t
{
    Coverage, ///< One byte a pixel: how much of it the glyph covers, to be tinted with the foreground.
    Color,    ///< Four bytes a pixel: red, green, blue and alpha, the colours premultiplied by the alpha.
};

/// A window of a vector glyph, drawn.
struct RasterizedGlyph
{
    GlyphPixels format = GlyphPixels::Coverage;
    std::vector<uint8_t> pixels; ///< Row-major, top row first.
};

/// Draws a window of a vector glyph (the Glyph Protocol's `vg` format).
///
/// The layers are painted in order, each over what is there: a fill or a stroke with its colour, through
/// the clip that covers it. What lies outside the window is clipped, so a tile cut from a wide span holds
/// what the whole span would show there.
///
/// A glyph whose every colour is the foreground is drawn as coverage, which the caller tints: one drawing
/// serves every foreground. Any other is drawn in colour, with the foreground it was given.
/// @param glyph The glyph.
/// @param placement Where its viewBox lands in its render span.
/// @param window The size of the window to draw.
/// @param origin Where in the span the window starts.
/// @param foreground The cell's foreground, for a glyph drawn in colour that uses it.
/// @return The window's pixels.
[[nodiscard]] RasterizedGlyph rasterizeVectorGlyph(vtbackend::VectorGlyph const& glyph,
                                                   GlyphSpanTransform const& placement,
                                                   vtbackend::ImageSize window,
                                                   GlyphWindowOrigin origin,
                                                   vtbackend::RGBColor foreground);

} // namespace vtrasterizer
