/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal_view/CursorRenderer.h>

#include <QtCore/QSize>

#include <QtGui/QOpenGLFunctions> // for GL_RED

#include <stdexcept>
#include <vector>

using std::get;
using std::max;
using std::nullopt;
using std::optional;
using std::runtime_error;
using std::string;

namespace terminal::view {

CursorShape makeCursorShape(string const& _name)
{
    auto static const toLower = [](string const& _value) -> string {
        string result;
        result.reserve(_value.size());
        transform(begin(_value), end(_value), back_inserter(result), [](auto ch) { return tolower(ch); });
        return result;
    };

    auto const name = toLower(_name);

    if (name == "block")
        return CursorShape::Block;

    if (name == "rectangle")
        return CursorShape::Rectangle;

    if (name == "underscore")
        return CursorShape::Underscore;

    if (name == "bar")
        return CursorShape::Bar;

    throw runtime_error("Invalid cursor shape. Use one of block, underscore, beam.");
}

string to_string(CursorShape _value)
{
    switch (_value)
    {
        case CursorShape::Block:
            return "block";
        case CursorShape::Rectangle:
            return "rectangle";
        case CursorShape::Underscore:
            return "underscore";
        case CursorShape::Bar:
            return "bar";
    }
    return "block";
}

CursorRenderer::CursorRenderer(crispy::atlas::CommandListener& _commandListener,
                               crispy::atlas::TextureAtlasAllocator& _monochromeTextureAtlas,
                               ScreenCoordinates const& _screenCoordinates,
                               CursorShape _shape,
                               QVector4D const& _color) :
    commandListener_{ _commandListener },
    textureAtlas_{ _monochromeTextureAtlas },
    screenCoordinates_{ _screenCoordinates },
    shape_{ _shape },
    color_{ _color },
    columnWidth_{ 1 }
{
}

void CursorRenderer::setShape(CursorShape _shape)
{
    if (_shape != shape_)
    {
        shape_ = _shape;
        rebuild();
    }
}

void CursorRenderer::setColor(QVector4D const& _color)
{
    color_ = _color;
}

void CursorRenderer::clearCache()
{
    textureAtlas_.clear();
}

void CursorRenderer::rebuild()
{
    clearCache();

    auto const width = screenCoordinates_.cellWidth;
    auto const baseline = screenCoordinates_.textBaseline;
    auto constexpr LineThickness = 1;

    { // {{{ CursorShape::Block
        auto const height = screenCoordinates_.cellHeight;
        auto image = crispy::atlas::Buffer(width * height, 0xFFu);

        textureAtlas_.insert(
            CursorShape::Block,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ CursorShape::Underscore
        auto const thickness = max(LineThickness * baseline / 3, 1u);
        auto const height = baseline;
        auto const base_y = max((height - thickness) / 2, 0u);
        auto image = crispy::atlas::Buffer(width * height, 0u);

        for (unsigned y = 1; y <= thickness; ++y)
            for (unsigned x = 0; x < width; ++x)
                image[(base_y + y) * width + x] = 0xFF;

        textureAtlas_.insert(
            CursorShape::Underscore,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ CursorShape::Bar
        auto const thickness = max(LineThickness * baseline / 3, 1u);
        auto const height = screenCoordinates_.cellHeight;
        //auto const base_y = max((height - thickness) / 2, 0u);
        auto image = crispy::atlas::Buffer(width * height, 0u);

        for (unsigned x = 0; x < thickness; ++x)
            for (unsigned y = 0; y < height; ++y)
                image[y * width + x] = 0xFF;

        textureAtlas_.insert(
            CursorShape::Bar,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
    { // {{{ CursorShape::Rectangle
        auto const height = screenCoordinates_.cellHeight;
        auto image = crispy::atlas::Buffer(width * height, 0xFFu);
        auto const thickness = max(width / 12, 1u);

        auto const innerWidth = width - 2 * thickness;
        auto const innerHeight = height - 2 * thickness;

        for (unsigned y = thickness; y <= innerHeight; ++y)
            for (unsigned x = thickness; x <= innerWidth; ++x)
                image[y * width + x] = 0;

        textureAtlas_.insert(
            CursorShape::Rectangle,
            width, height,
            width, height,
            GL_RED,
            move(image)
        );
    } // }}}
}

optional<CursorRenderer::DataRef> CursorRenderer::getDataRef(CursorShape _shape)
{
    if (optional<DataRef> const dataRef = textureAtlas_.get(_shape); dataRef.has_value())
        return dataRef;

    if (textureAtlas_.empty())
        rebuild();

    if (optional<DataRef> const dataRef = textureAtlas_.get(_shape); dataRef.has_value())
        return dataRef;

    return nullopt;
}

void CursorRenderer::render(QPoint _pos, unsigned _columnWidth)
{
    if (columnWidth_ != _columnWidth)
    {
        // XXX we could optimize here  by keying for (shape, columnWidth) and just cache them all, but that's a minor.
        columnWidth_ = _columnWidth;
        rebuild();
    }

    if (optional<DataRef> const dataRef = getDataRef(shape_); dataRef.has_value())
    {
        auto const& textureInfo = get<0>(dataRef.value()).get();
        auto const x = _pos.x();
        auto const y = _pos.y();
        auto constexpr z = 0;
        commandListener_.renderTexture({textureInfo, x, y, z, color_});
    }
}

} // namespace terminal::view
