// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/LinkDecoration.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Screen.hpp>

#include <vtrasterizer/Decorator.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextureAtlas.hpp>
#include <vtrasterizer/UnderlineGeometry.hpp>

#include <optional>

namespace vtrasterizer
{

struct GridMetrics;

/// The decorator a link cell is drawn with.
/// @param role The cell's link role.
/// @param normal The profile's hyperlink_decoration.normal.
/// @param hover The profile's hyperlink_decoration.hover.
/// @return The decorator; nullopt for LinkDecorationRole::None.
[[nodiscard]] constexpr std::optional<Decorator> decoratorFor(vtbackend::LinkDecorationRole role,
                                                              Decorator normal,
                                                              Decorator hover) noexcept
{
    switch (role)
    {
        case vtbackend::LinkDecorationRole::None: return std::nullopt;
        case vtbackend::LinkDecorationRole::Normal: return normal;
        case vtbackend::LinkDecorationRole::Hover: return hover;
    }
    return std::nullopt;
}

/// Renders any kind of grid cell decorations, ranging from basic underline to surrounding boxes.
class DecorationRenderer: public Renderable
{
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param commandListener
    /// @param monochromeTextureAtlas
    /// @param gridMetrics
    DecorationRenderer(GridMetrics const& gridMetrics, Decorator hyperlinkNormal, Decorator hyperlinkHover);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void setTextureAtlas(TextureAtlas& atlas) override;
    void clearCache() override;
    void inspect(std::ostream& output) const override;

    void setHyperlinkDecoration(Decorator normal, Decorator hover)
    {
        _hyperlinkNormal = normal;
        _hyperlinkHover = hover;
    }

    void renderCell(vtbackend::RenderCell const& cell);
    void renderLine(vtbackend::RenderLine const& line);

    /// Draws @p decoration across @p columnCount grid columns, one atlas tile per column.
    ///
    /// The dotted underline's pattern runs on one pitch across the whole line, so column `i` draws the
    /// phase tile of display column `firstColumn + i` (@see dottedUnderlinePhaseOf); every other decorator
    /// draws its single tile in every column.
    ///
    /// @param decoration The decoration to draw.
    /// @param pos The bottom-left corner of the first column, in pixels.
    /// @param firstColumn The display column, counted from the line's start, that @p pos is the left
    ///                    edge of -- on a double-width line already twice the grid column.
    /// @param columnCount How many columns to draw.
    /// @param color The decoration's color.
    void renderDecoration(Decorator decoration,
                          crispy::Point pos,
                          vtbackend::ColumnOffset firstColumn,
                          vtbackend::ColumnCount columnCount,
                          vtbackend::RGBColor const& color);

    [[nodiscard]] constexpr Decorator hyperlinkNormal() const noexcept { return _hyperlinkNormal; }
    [[nodiscard]] constexpr Decorator hyperlinkHover() const noexcept { return _hyperlinkHover; }

    [[nodiscard]] constexpr int underlineThickness() const noexcept
    {
        return _gridMetrics.underline.thickness;
    }

    [[nodiscard]] constexpr int underlinePosition() const noexcept { return _gridMetrics.underline.position; }

  private:
    void initializeDirectMapping();
    using Renderable::createTileData;
    TextureAtlas::TileCreateData createTileData(Decorator decoration, atlas::TileLocation tileLocation);

    /// Rasterizes the dotted underline's tile for @p phase.
    /// @param phase The phase, in `[0, phaseCount)`; phase 0 is the DottedUnderline tile itself.
    /// @param tileLocation Where the tile lives in the atlas.
    /// @return The tile's antialiased coverage, top-origin.
    [[nodiscard]] TextureAtlas::TileCreateData createDottedPhaseTile(int phase,
                                                                     atlas::TileLocation tileLocation);

    // private data members
    //
    DirectMapping _directMapping;
    Decorator _hyperlinkNormal = Decorator::DottedUnderline;
    Decorator _hyperlinkHover = Decorator::Underline;

    /// The dotted underline's geometry the phase tiles were rasterized with; refreshed by
    /// initializeDirectMapping(), so the phase picked at render time always names an uploaded tile.
    DottedUnderlineGeometry _dottedGeometry;
};

} // namespace vtrasterizer
