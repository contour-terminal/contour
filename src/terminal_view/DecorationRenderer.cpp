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

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

using namespace std;
using namespace crispy;

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
        pair{"framed", Decorator::Frame},
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
                                       unsigned _lineThickness,
                                       float _curlyAmplitude,
                                       float _curlyFrequency) :
    screenCoordinates_{ _screenCoordinates },
    hyperlinkNormal_{ _hyperlinkNormal },
    hyperlinkHover_{ _hyperlinkHover },
    lineThickness_{ _lineThickness },
    curlyAmplitude_{ _curlyAmplitude },
    curlyFrequency_{ _curlyFrequency },
    colorProfile_{ _colorProfile },
    commandListener_{ _commandListener },
    atlas_{ _monochromeTextureAtlas }
{
}

void DecorationRenderer::clearCache()
{
    atlas_.clear();
}

void DecorationRenderer::setColorProfile(ColorProfile const& _colorProfile)
{
    colorProfile_ = _colorProfile;
}

void DecorationRenderer::rebuild()
{
    auto const width = screenCoordinates_.cellWidth;
    auto const baseline = screenCoordinates_.textBaseline;

    { // {{{ underline
        auto const thickness = max(lineThickness_ * baseline / 3, 1u);
        auto const height = baseline;
        auto const base_y = max((height - thickness) / 2, 0u);
        auto image = atlas::Buffer(width * height, 0u);

        for (unsigned y = 1; y <= thickness; ++y)
            for (unsigned x = 0; x < width; ++x)
                image[(base_y + y) * width + x] = 0xFF;

        atlas_.insert(
            Decorator::Underline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ double underline
        auto const height = max(baseline - 1, 3u);
        auto const thickness = height / (3 * lineThickness_);
        auto image = atlas::Buffer(width * height, 0u);

        for (unsigned y = 0; y < thickness; ++y)
        {
            for (unsigned x = 0; x < width; ++x)
            {
                image[y * width + x] = 0xFF;
                image[(height - 1 - y) * width + x] = 0xFF;
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
        auto const height = max(static_cast<unsigned>(curlyAmplitude_ * static_cast<float>(screenCoordinates_.textBaseline)), lineThickness_ * 3) - lineThickness_;
        auto image = atlas::Buffer(width * height, 0u);

        for (unsigned x = 0; x < width; ++x)
        {
            auto const normalizedX = static_cast<double>(x) / static_cast<double>(width);
            auto const sin_x = curlyFrequency_ * normalizedX * 2.0 * M_PI;
            auto const normalizedY = (cosf(sin_x) + 1.0f) / 2.0f;
            assert(0.0f <= normalizedY && normalizedY <= 1.0f);
            auto const y = static_cast<unsigned>(normalizedY * static_cast<float>(height - lineThickness_));
            assert(y < height);
            for (unsigned yi = 0; yi < lineThickness_; ++yi)
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
        auto const thickness = max(lineThickness_ * width / 6, 1u);
        auto const height = thickness;
        auto image = atlas::Buffer(width * height, 0u);

        for (unsigned x = 0; x < width; ++x)
            if ((x / thickness) % 3 == 1)
                for (unsigned y = 0; y < height; ++y)
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
        auto const thickness = max(lineThickness_ * width / 4, 1u);
        auto const height = thickness;
        auto image = atlas::Buffer(width * height, 0u);

        for (unsigned x = 0; x < width; ++x)
            if (fabs(float(x) / float(width) - 0.5f) >= 0.25f)
                for (unsigned y = 0; y < height; ++y)
                    image[y * width + x] = 0xFF;

        atlas_.insert(
            Decorator::DashedUnderline,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ overline
        auto const cellHeight = screenCoordinates_.cellHeight;
        auto const thickness = max(lineThickness_ * baseline / 3, 1u);
        auto image = atlas::Buffer(width * cellHeight, 0u);

        for (unsigned y = 0; y < thickness; ++y)
            for (unsigned x = 0; x < width; ++x)
                image[y * width + x] = 0xFF;

        atlas_.insert(
            Decorator::Overline,
            width, cellHeight,
            width, cellHeight,
            GL_RED,
            move(image)
        );
    } // }}}
    // TODO: CrossedOut
    // TODO: Framed
    // TODO: Encircle
}

void DecorationRenderer::renderCell(cursor_pos_t _row,
                                    cursor_pos_t _col,
                                    ScreenBuffer::Cell const& _cell)
{
    if (_cell.hyperlink())
    {
        auto const& color = _cell.hyperlink()->state == HyperlinkState::Hover
                            ? colorProfile_.hyperlinkDecoration.hover
                            : colorProfile_.hyperlinkDecoration.normal;
        auto const decoration = _cell.hyperlink()->state == HyperlinkState::Hover
                            ? hyperlinkHover_
                            : hyperlinkNormal_;
        renderDecoration(decoration, _row, _col, 1, color);
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
                renderDecoration(mapping.second, _row, _col, 1, _cell.attributes().getUnderlineColor(colorProfile_));
    }

    auto constexpr supplementalMappings = array{
        pair{CharacterStyleMask::Overline, Decorator::Overline},
        pair{CharacterStyleMask::CrossedOut, Decorator::CrossedOut},
        pair{CharacterStyleMask::Framed, Decorator::Frame},
        pair{CharacterStyleMask::Encircled, Decorator::Encircle},
    };

    for (auto const& mapping : supplementalMappings)
        if (_cell.attributes().styles & mapping.first)
            renderDecoration(mapping.second, _row, _col, 1, _cell.attributes().getUnderlineColor(colorProfile_));
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
                                          cursor_pos_t _row,
                                          cursor_pos_t _col,
                                          unsigned _columnCount,
                                          RGBColor const& _color)
{
    if (optional<DataRef> const dataRef = getDataRef(_decoration); dataRef.has_value())
    {
#if 0 // !defined(NDEBUG)
        cout << fmt::format(
            "DecorationRenderer.renderDecoration: {} from {}:{} with {} cells, color {}\n",
            _decoration, _row, _col, _columnCount, _color
        );
#endif
        auto const pos = screenCoordinates_.map(_col, _row);
        auto const x = pos.x();
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
        auto const y = pos.y();
#else
        auto const y = pos.y() + screenCoordinates_.cellHeight;
#endif
        auto const z = 0u;
        auto const color = QVector4D(
            static_cast<float>(_color.red) / 255.0f,
            static_cast<float>(_color.green) / 255.0f,
            static_cast<float>(_color.blue) / 255.0f,
            1.0f
        );
        atlas::TextureInfo const& textureInfo = get<0>(dataRef.value()).get();
        auto const advanceX = static_cast<int>(screenCoordinates_.cellWidth);
        for (int i = 0; i < static_cast<int>(_columnCount); ++i)
        {
#if 0 // !defined(NDEBUG)
            cout << fmt::format(
                " at: {}:{}\n",
                x + advanceX + i,
                y
            );
#endif
            commandListener_.renderTexture({textureInfo, x + advanceX * i, y, z, color});
        }
    }
    else
    {
#if 0 // !defined(NDEBUG)
        cout << fmt::format(
            "DecorationRenderer.renderDecoration: {} from {}:{} with {} cells (MISSING IMPLEMENTATION)\n",
            _decoration, _row, _col, _columnCount, _color
        );
#endif
    }
}

} // end namespace
