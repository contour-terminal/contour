/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
                          crispy::Point pos,
                          ColumnCount columnCount,
                          rgb_color const& color);

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
