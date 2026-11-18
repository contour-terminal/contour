// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <cstdint>
#include <optional>
#include <ostream>

namespace vtrasterizer
{

/// What RegisteredGlyphRenderer::render did.
enum class RegisteredGlyphDraw : uint8_t
{
    AtlasFull, ///< No tile could be had for the glyph (the atlas refused it); nothing was drawn.
    Drawn,     ///< The glyph was drawn.
};

/// Draws the glyphs an application registered through the Glyph Protocol, as alpha tiles: the atlas holds
/// coverage only, and the foreground colour is applied at draw time, so one tile serves every colour the
/// glyph is shown in. A glyph is rasterized one cell-sized tile at a time, which is also what clips it to
/// its render span.
class RegisteredGlyphRenderer: public Renderable
{
  public:
    explicit RegisteredGlyphRenderer(GridMetrics const& gridMetrics): Renderable { gridMetrics } {}

    /// Draws @p glyph from the cell at @p position on, across @p span cells.
    ///
    /// A DECDWL row doubles the width of each of those cells; a DECDHL half takes the matching half of a
    /// glyph of twice the height.
    /// @param position The cell, relative to the page.
    /// @param glyph The glyph.
    /// @param span How many cells to draw it across (@see registeredGlyphSpan).
    /// @param color The foreground colour, as resolved by the render buffer.
    /// @param flags The row's line flags.
    /// @return Drawn, or AtlasFull, in which case the caller falls back to drawing the cell's text.
    [[nodiscard]] RegisteredGlyphDraw render(vtbackend::CellLocation position,
                                             vtbackend::RegisteredGlyph const& glyph,
                                             vtbackend::GlyphRenderSpan span,
                                             vtbackend::RGBColor color,
                                             vtbackend::LineFlags flags);

    void inspect(std::ostream& output) const override;

  private:
    /// One tile of a glyph: which glyph, how it is laid out on its row, and which cell of its span.
    struct Tile
    {
        vtbackend::RegisteredGlyph const& glyph;
        vtbackend::LineFlags flags;
        int tileCount = 1; ///< How many cell-sized tiles the span is cut into.
        int index = 0;     ///< Which of them, from the left.
    };

    [[nodiscard]] AtlasTileAttributes const* getOrCreateCachedTile(Tile const& tile);
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTile(Tile const& tile,
                                                                         atlas::TileLocation tileLocation);
};

} // namespace vtrasterizer
