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
#include <terminal_view/DecorationRenderer.h>
#include <terminal_view/ScreenCoordinates.h>

#include <crispy/Atlas.h>
#include <crispy/AtlasRenderer.h>
#include <crispy/logger.h>
#include <crispy/times.h>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

using std::array;
using std::get;
using std::max;
using std::min;
using std::nullopt;
using std::optional;
using std::pair;
using std::string;

namespace atlas = crispy::atlas;

namespace terminal::view {

optional<Decorator> to_decorator(std::string const& _value)
{
    auto constexpr mappings = array{
        pair{"underline", Decorator::Underline},
        pair{"double-underline", Decorator::DoubleUnderline},
        pair{"curly-underline", Decorator::CurlyUnderline},
        pair{"dashed-underline", Decorator::DashedUnderline},
        pair{"overline", Decorator::Overline},
        pair{"crossed-out", Decorator::CrossedOut},
        pair{"framed", Decorator::Framed},
        pair{"encircle", Decorator::Encircle},
    };

    for (auto const& mapping : mappings)
        if (mapping.first == _value)
            return {mapping.second};

    return nullopt;
}

DecorationRenderer::DecorationRenderer(atlas::CommandListener& _commandListener,
                                       atlas::TextureAtlasAllocator& _monochromeTextureAtlas,
                                       ScreenCoordinates const& _screenCoordinates,
                                       ColorProfile const& _colorProfile,
                                       Decorator _hyperlinkNormal,
                                       Decorator _hyperlinkHover,
                                       crispy::text::Font const& _font,
                                       float _curlyAmplitude,
                                       float _curlyFrequency) :
    screenCoordinates_{ _screenCoordinates },
    hyperlinkNormal_{ _hyperlinkNormal },
    hyperlinkHover_{ _hyperlinkHover },
    underlinePosition_{ 0 }, // TODO
    curlyAmplitude_{ _curlyAmplitude },
    curlyFrequency_{ _curlyFrequency },
    colorProfile_{ _colorProfile },
    commandListener_{ _commandListener },
    atlas_{ _monochromeTextureAtlas }
{
    setFontMetrics(_font);
}

void DecorationRenderer::clearCache()
{
    atlas_.clear();
}

void DecorationRenderer::setColorProfile(ColorProfile const& _colorProfile)
{
    colorProfile_ = _colorProfile;
}

void DecorationRenderer::setFontMetrics(crispy::text::Font const& _font)
{
    underlinePosition_ = screenCoordinates_.textBaseline
                       + _font.underlineOffset();
    lineThickness_ = _font.underlineThickness();
    ascender_ = _font.ascender();
    descender_ = _font.descender();

    atlas_.clear();
}

void DecorationRenderer::rebuild()
{
    auto const width = screenCoordinates_.cellSize.width;
    auto const baseline = screenCoordinates_.textBaseline;


    { // {{{ underline
        auto const thickness_half = int(ceil(lineThickness_ / 2.0));
        auto const thickness = min(1, thickness_half * 2);
        auto const y0 = max(0, underlinePosition_ - thickness_half);
        auto const height = y0 + thickness;
        auto image = atlas::Buffer(width * height, 0);

        for (int y = 1; y <= thickness; ++y)
            for (int x = 0; x < width; ++x)
                image[(height - y0 - y) * width + x] = 0xFF;

        atlas_.insert(
            Decorator::Underline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ double underline
        auto const thickness_half = int(ceil(lineThickness_ / 2.0));
        auto const thickness = min(1, thickness_half * 2);
        auto const y1 = max(0, underlinePosition_ - thickness_half);
        auto const y0 = max(0, y1 - 2 * thickness);
        auto const height = y1 + thickness;
        auto image = atlas::Buffer(width * height, 0);

        for (int y = 1; y <= thickness; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                image[(height - y1 - y) * width + x] = 0xFF; // top line
                image[(height - y0 - y) * width + x] = 0xFF; // bottom line
            }
        }

        atlas_.insert(
            Decorator::DoubleUnderline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ curly underline
        auto const height = int(ceil(double(screenCoordinates_.cellSize.height - ascender_) * 2.0 / 3.0));
        auto image = atlas::Buffer(width * height, 0);

        for (int x = 0; x < width; ++x)
        {
            auto const normalizedX = static_cast<double>(x) / static_cast<double>(width);
            auto const sin_x = curlyFrequency_ * normalizedX * 2.0 * M_PI;
            auto const normalizedY = (cosf(sin_x) + 1.0f) / 2.0f;
            assert(0.0f <= normalizedY && normalizedY <= 1.0f);
            auto const y = static_cast<int>(normalizedY * static_cast<float>(height - lineThickness_));
            assert(y < height);
            for (int yi = 0; yi < lineThickness_; ++yi)
                image[(y + yi) * width + x] = 0xFF;
        }

        atlas_.insert(
            Decorator::CurlyUnderline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ dotted underline
        auto const thickness = lineThickness_;
        auto const height = thickness;
        auto image = atlas::Buffer(width * height, 0);

        for (int x = 0; x < width; ++x)
            if ((x / thickness) % 3 == 1)
                for (int y = 0; y < height; ++y)
                    image[y * width + x] = 0xFF;

        atlas_.insert(
            Decorator::DottedUnderline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ dashed underline
        // Devides a grid cell's underline in three sub-ranges and only renders first and third one,
        // whereas the middle one is being skipped.
        auto const thickness = lineThickness_;
        auto const height = thickness;
        auto image = atlas::Buffer(width * height, 0);

        for (int x = 0; x < width; ++x)
            if (fabs(float(x) / float(width) - 0.5f) >= 0.25f)
                for (int y = 0; y < height; ++y)
                    image[y * width + x] = 0xFF;

        atlas_.insert(
            Decorator::DashedUnderline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ framed
        auto const cellHeight = screenCoordinates_.cellSize.height;
        auto const thickness = lineThickness_;
        auto image = atlas::Buffer(width * cellHeight, 0u);
        auto const gap = 0; // thickness;

        // Draws the top and bottom horizontal lines
        for (int y = gap; y < thickness + gap; ++y)
            for (int x = gap; x < width - gap; ++x)
            {
                image[y * width + x] = 0xFF;
                image[(cellHeight - 1 - y) * width + x] = 0xFF;
            }

        // Draws the left and right vertical lines
        for (int y = gap; y < cellHeight - gap; y++)
            for (int x = gap; x < thickness + gap; ++x)
            {
                image[y * width + x] = 0xFF;
                image[y * width + (width - 1 - x)] = 0xFF;
            }

        atlas_.insert(
            Decorator::Framed,
            width, cellHeight,
            width, cellHeight,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ overline
        auto const cellHeight = screenCoordinates_.cellSize.height;
        auto const thickness = lineThickness_;
        auto image = atlas::Buffer(width * cellHeight, 0);

        for (int y = 0; y < thickness; ++y)
            for (int x = 0; x < width; ++x)
                image[y * width + x] = 0xFF;

        atlas_.insert(
            Decorator::Overline,
            width, cellHeight,
            width, cellHeight,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ crossed-out
        auto const middleCell = screenCoordinates_.cellSize.height / 2;
        auto const thickness = max(lineThickness_ * baseline / 3, 1);
        auto image = atlas::Buffer(width * middleCell, 0u);

        for (int y = 0; y < thickness; ++y)
            for (int x = 0; x < width; ++x)
                image[y * width + x] = 0xFF;

        atlas_.insert(
            Decorator::CrossedOut,
            width, middleCell - thickness / 2,
            width, middleCell - thickness / 2,
            GL_RED,
            move(image)
        );
    } // }}}
    // TODO: Encircle
}

void DecorationRenderer::renderCell(Coordinate const& _pos,
                                    Cell const& _cell)
{
    if (_cell.hyperlink())
    {
        auto const& color = _cell.hyperlink()->state == HyperlinkState::Hover
                            ? colorProfile_.hyperlinkDecoration.hover
                            : colorProfile_.hyperlinkDecoration.normal;
        auto const decoration = _cell.hyperlink()->state == HyperlinkState::Hover
                            ? hyperlinkHover_
                            : hyperlinkNormal_;
        renderDecoration(decoration, _pos, 1, color);
    }
    else
    {
        auto constexpr underlineMappings = array{
            pair{CharacterStyleMask::Underline, Decorator::Underline},
            pair{CharacterStyleMask::DoublyUnderlined, Decorator::DoubleUnderline},
            pair{CharacterStyleMask::CurlyUnderlined, Decorator::CurlyUnderline},
            pair{CharacterStyleMask::DottedUnderline, Decorator::DottedUnderline},
            pair{CharacterStyleMask::DashedUnderline, Decorator::DashedUnderline},
        };

        for (auto const& mapping : underlineMappings)
            if (_cell.attributes().styles & mapping.first)
                renderDecoration(mapping.second, _pos, 1, _cell.attributes().getUnderlineColor(colorProfile_));
    }

    auto constexpr supplementalMappings = array{
        pair{CharacterStyleMask::Overline, Decorator::Overline},
        pair{CharacterStyleMask::CrossedOut, Decorator::CrossedOut},
        pair{CharacterStyleMask::Framed, Decorator::Framed},
        pair{CharacterStyleMask::Encircled, Decorator::Encircle},
    };

    for (auto const& mapping : supplementalMappings)
        if (_cell.attributes().styles & mapping.first)
            renderDecoration(mapping.second, _pos, 1, _cell.attributes().getUnderlineColor(colorProfile_));
}

optional<DecorationRenderer::DataRef> DecorationRenderer::getDataRef(Decorator _decoration)
{
    if (optional<DataRef> const dataRef = atlas_.get(_decoration); dataRef.has_value())
        return dataRef;

    if (atlas_.empty())
        rebuild();

    if (optional<DataRef> const dataRef = atlas_.get(_decoration); dataRef.has_value())
        return dataRef;

    return nullopt;
}

void DecorationRenderer::renderDecoration(Decorator _decoration,
                                          Coordinate const& _pos,
                                          int _columnCount,
                                          RGBColor const& _color)
{
    if (optional<DataRef> const dataRef = getDataRef(_decoration); dataRef.has_value())
    {
#if 0 // !defined(NDEBUG)
        cout << fmt::format(
            "DecorationRenderer.renderDecoration: {} from {} with {} cells, color {}\n",
            _decoration, _pos, _columnCount, _color
        );
#endif
        auto const pos = screenCoordinates_.map(_pos);
        auto const x = pos.x();
        auto const y = pos.y();
        auto const z = 0;
        auto const color = QVector4D(
            static_cast<float>(_color.red) / 255.0f,
            static_cast<float>(_color.green) / 255.0f,
            static_cast<float>(_color.blue) / 255.0f,
            1.0f
        );
        atlas::TextureInfo const& textureInfo = get<0>(dataRef.value()).get();
        auto const advanceX = static_cast<int>(screenCoordinates_.cellSize.width);
        for (int const i : crispy::times(_columnCount))
            commandListener_.renderTexture({textureInfo, i * advanceX + x, y, z, color});
    }
    else
    {
#if 0 // !defined(NDEBUG)
        cout << fmt::format(
            "DecorationRenderer.renderDecoration: {} from {} with {} cells (MISSING IMPLEMENTATION)\n",
            _decoration, _pos, _columnCount, _color
        );
#endif
    }
}

} // end namespace
