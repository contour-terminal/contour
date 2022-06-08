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

#include <terminal/RenderBuffer.h>
#include <terminal/Screen.h>

#include <terminal_renderer/Decorator.h>
#include <terminal_renderer/RenderTarget.h>
#include <terminal_renderer/TextureAtlas.h>

namespace terminal::renderer
{

struct GridMetrics;

/// Renders any kind of grid cell decorations, ranging from basic underline to surrounding boxes.
class DecorationRenderer: public Renderable
{
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param _commandListener
    /// @param _monochromeTextureAtlas
    /// @param _gridMetrics
    DecorationRenderer(GridMetrics const& _gridMetrics,
                       Decorator _hyperlinkNormal,
                       Decorator _hyperlinkHover);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void setTextureAtlas(TextureAtlas& atlas) override;
    void clearCache() override;
    void inspect(std::ostream& output) const override;

    void setHyperlinkDecoration(Decorator _normal, Decorator _hover)
    {
        hyperlinkNormal_ = _normal;
        hyperlinkHover_ = _hover;
    }

    void renderCell(RenderCell const& _cell);
    void renderLine(RenderLine const& line);

    void renderDecoration(Decorator _decoration,
                          crispy::Point _pos,
                          ColumnCount columnCount,
                          RGBColor const& _color);

    constexpr Decorator hyperlinkNormal() const noexcept { return hyperlinkNormal_; }
    constexpr Decorator hyperlinkHover() const noexcept { return hyperlinkHover_; }

    constexpr int underlineThickness() const noexcept { return _gridMetrics.underline.thickness; }
    constexpr int underlinePosition() const noexcept { return _gridMetrics.underline.position; }

  private:
    void initializeDirectMapping();
    using Renderable::createTileData;
    TextureAtlas::TileCreateData createTileData(Decorator decoration, atlas::TileLocation tileLocation);

    // private data members
    //
    DirectMapping _directMapping;
    Decorator hyperlinkNormal_ = Decorator::DottedUnderline;
    Decorator hyperlinkHover_ = Decorator::Underline;
};

} // namespace terminal::renderer
