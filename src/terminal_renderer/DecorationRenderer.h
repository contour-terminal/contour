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

#include <terminal_renderer/Atlas.h>
#include <terminal_renderer/RenderTarget.h>

#include <terminal/RenderBuffer.h>
#include <terminal/Screen.h>

namespace terminal::renderer {

struct GridMetrics;

/// Dectorator, to decorate a grid cell, eventually containing a character
///
/// It should be possible to render multiple decoration onto the same coordinates.
enum class Decorator {
    /// Draws an underline
    Underline,
    /// Draws a doubly underline
    DoubleUnderline,
    /// Draws a curly underline
    CurlyUnderline,
    /// Draws a dotted underline
    DottedUnderline,
    /// Draws a dashed underline
    DashedUnderline,
    /// Draws an overline
    Overline,
    /// Draws a strike-through line
    CrossedOut,
    /// Draws a box around the glyph, this is literally the bounding box of a grid cell.
    /// This could be used for debugging.
    /// TODO: That should span the box around the whole (potentially wide) character
    Framed,
    /// Puts a circle-shape around into the cell (and ideally around the glyph)
    /// TODO: How'd that look like with double-width characters?
    Encircle,
};

std::optional<Decorator> to_decorator(std::string const& _value);

/// Renders any kind of grid cell decorations, ranging from basic underline to surrounding boxes.
class DecorationRenderer : public Renderable {
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param _commandListener
    /// @param _monochromeTextureAtlas
    /// @param _gridMetrics
    DecorationRenderer(GridMetrics const& _gridMetrics,
                       Decorator _hyperlinkNormal,
                       Decorator _hyperlinkHover);

    void setRenderTarget(RenderTarget& _renderTarget) override;
    void clearCache() override;

    void setHyperlinkDecoration(Decorator _normal, Decorator _hover)
    {
        hyperlinkNormal_  = _normal;
        hyperlinkHover_ = _hover;
    }

    void renderCell(RenderCell const& _cell);

    void renderDecoration(Decorator _decoration,
                          crispy::Point _pos,
                          int _columnCount,
                          RGBColor const& _color);

    constexpr Decorator hyperlinkNormal() const noexcept { return hyperlinkNormal_; }
    constexpr Decorator hyperlinkHover() const noexcept { return hyperlinkHover_; }

    constexpr int underlineThickness() const noexcept { return gridMetrics_.underline.thickness; }
    constexpr int underlinePosition() const noexcept { return gridMetrics_.underline.position; }

  private:
    using Atlas = atlas::MetadataTextureAtlas<Decorator, int>; // contains various glyph decorators
    using DataRef = Atlas::DataRef;

    void rebuild();

    std::optional<DataRef> getDataRef(Decorator _decorator);

    // private data members
    //
    GridMetrics const& gridMetrics_;

    Decorator hyperlinkNormal_ = Decorator::DottedUnderline;
    Decorator hyperlinkHover_ = Decorator::Underline;

    std::unique_ptr<Atlas> atlas_;
};

} // end namespace
