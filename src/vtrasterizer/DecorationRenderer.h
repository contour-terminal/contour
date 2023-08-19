// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Screen.h>

#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

namespace terminal::rasterizer
{

struct GridMetrics;

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

    void renderCell(RenderCell const& cell);
    void renderLine(RenderLine const& line);

    void renderDecoration(Decorator decoration,
                          crispy::point pos,
                          ColumnCount columnCount,
                          RGBColor const& color);

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

    // private data members
    //
    DirectMapping _directMapping;
    Decorator _hyperlinkNormal = Decorator::DottedUnderline;
    Decorator _hyperlinkHover = Decorator::Underline;
};

} // namespace terminal::rasterizer
