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

#include <terminal_view/ShaderConfig.h>

#include <crispy/Atlas.h>
#include <crispy/text/Font.h>

#include <terminal/Screen.h>

namespace terminal::view {

struct ScreenCoordinates;

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
class DecorationRenderer {
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param _commandListener
    /// @param _monochromeTextureAtlas
    /// @param _screenCoordinates
    /// @param _font used to retrieve font metrics
    /// @param _colorProfile
    /// @param _curlyAmplitude the total hight in pixels the sine wave will take, that is: abs(minimum, maximum).
    /// @param _curlyFrequency the number of complete sine waves that one grid cell width will cover
    DecorationRenderer(crispy::atlas::CommandListener& _commandListener,
                       crispy::atlas::TextureAtlasAllocator& _monochromeTextureAtlas,
                       ScreenCoordinates const& _screenCoordinates,
                       ColorProfile const& _colorProfile,
                       Decorator _hyperlinkNormal,
                       Decorator _hyperlinkHover,
                       crispy::text::Font const& _font,
                       float _curlyAmplitude,
                       float _curlyFrequency);

    void setColorProfile(ColorProfile const& _colorProfile);

    void setFontMetrics(crispy::text::Font const& _font);

    void setHyperlinkDecoration(Decorator _normal, Decorator _hover)
    {
        std::cout << fmt::format("setHyperlinkDecoration: {}; {}\n", _normal, _hover);
        hyperlinkNormal_  = _normal;
        hyperlinkHover_ = _hover;
    }

    void renderCell(Coordinate const& _pos, Cell const& _cell);

    void renderDecoration(Decorator _decoration,
                          Coordinate const& _pos,
                          int _columnCount,
                          RGBColor const& _color);

    void clearCache();

  private:
    using Atlas = crispy::atlas::MetadataTextureAtlas<Decorator, int>; // contains various glyph decorators
    using DataRef = Atlas::DataRef;

    void rebuild();

    std::optional<DataRef> getDataRef(Decorator _decorator);

    // private data members
    //
    ScreenCoordinates const& screenCoordinates_;

    Decorator hyperlinkNormal_ = Decorator::DottedUnderline;
    Decorator hyperlinkHover_ = Decorator::Underline;
    int underlinePosition_ = 0;     // Center of the underline position, relative to cell bottom.
    int lineThickness_ = 1;
    int ascender_;
    int descender_;
    float curlyAmplitude_ = 1.0f;
    float curlyFrequency_ = 1.0f;

    ColorProfile colorProfile_; // TODO: make const&, maybe reference_wrapper<>?

    crispy::atlas::CommandListener& commandListener_;
    Atlas atlas_;
};

} // end namespace
