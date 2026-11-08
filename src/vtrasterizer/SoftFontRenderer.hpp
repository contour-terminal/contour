// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/vt/SoftFont.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace vtrasterizer
{

/// Scales a soft glyph to @p target with nearest-neighbour sampling, so it fills the target exactly.
///
/// `dst(x, y) = src(x * w / W, y * h / H)`, where w x h is the glyph's size and W x H is @p target.
/// No filtering: a soft glyph is a bitmap font, and blending its pixels would blur it.
/// @param glyph The glyph to scale.
/// @param target The size to scale it to.
/// @return Row-major pixels, top row first, each 0 (clear) or 0xFF (ink).
[[nodiscard]] std::vector<uint8_t> scaleSoftGlyph(vtbackend::SoftGlyph const& glyph,
                                                  vtbackend::ImageSize target);

/// What SoftFontRenderer::render did.
enum class SoftGlyphDraw : uint8_t
{
    AtlasFull, ///< No tile could be had for the glyph (the atlas refused it); nothing was drawn.
    Drawn,     ///< The glyph was drawn.
};

/// Draws soft font glyphs (DECDLD) as alpha tiles: the atlas holds coverage only, and the foreground
/// colour is applied at draw time, so one tile serves every colour the glyph is shown in.
class SoftFontRenderer: public Renderable
{
  public:
    explicit SoftFontRenderer(GridMetrics const& gridMetrics): Renderable { gridMetrics } {}

    /// Draws @p glyph into the cell at @p position, stretched to fill it.
    ///
    /// A DECDWL row takes two tiles from a glyph of twice the width; a DECDHL half takes the matching half
    /// of a glyph of twice the height.
    /// @param position The cell, relative to the page.
    /// @param glyph The glyph.
    /// @param color The foreground colour, as resolved by the render buffer.
    /// @param flags The row's line flags.
    /// @return Drawn, or AtlasFull, in which case the caller falls back to drawing the cell's text.
    [[nodiscard]] SoftGlyphDraw render(vtbackend::CellLocation position,
                                       vtbackend::SoftGlyph const& glyph,
                                       vtbackend::RGBColor color,
                                       vtbackend::LineFlags flags);

    void inspect(std::ostream& output) const override;

  private:
    [[nodiscard]] AtlasTileAttributes const* getOrCreateCachedTile(vtbackend::SoftGlyph const& glyph,
                                                                   vtbackend::LineFlags flags,
                                                                   int subIndex);
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createSoftTile(
        vtbackend::SoftGlyph const& glyph,
        vtbackend::LineFlags flags,
        atlas::TileLocation tileLocation,
        int subIndex);
};

/// Finds the soft glyph of each cell of a pass, given the glyphs sorted by position and the cells
/// visited in the same order: the cursor only moves forward, so a pass costs O(cells + glyphs).
class SoftGlyphCursor
{
  public:
    /// @param glyphs The pass's soft glyphs, sorted by position; they must outlive the cursor.
    explicit SoftGlyphCursor(std::span<vtbackend::RenderSoftGlyph const> glyphs) noexcept:
        _glyphs { glyphs }, _next { glyphs.begin() }
    {
    }

    /// @param position The cell about to be rendered.
    /// @return The glyph the cell at @p position shows, or null for a cell that shows its text.
    [[nodiscard]] vtbackend::SoftGlyph const* find(vtbackend::CellLocation position) noexcept
    {
        // A cell before the previous one (a pass that does not walk the page in order) starts over.
        if (position < _previous)
            _next = std::ranges::lower_bound(_glyphs, position, {}, &vtbackend::RenderSoftGlyph::position);
        _previous = position;
        while (_next != _glyphs.end() && _next->position < position)
            ++_next;
        return _next != _glyphs.end() && _next->position == position ? _next->glyph.get() : nullptr;
    }

  private:
    std::span<vtbackend::RenderSoftGlyph const> _glyphs;
    std::span<vtbackend::RenderSoftGlyph const>::iterator _next;
    vtbackend::CellLocation _previous {};
};

} // namespace vtrasterizer
