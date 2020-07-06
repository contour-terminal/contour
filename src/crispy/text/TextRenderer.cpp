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
#include <crispy/text/TextRenderer.h>
#include <crispy/reference.h>
#include <iostream>

using namespace std;

namespace crispy::text {

constexpr unsigned MaxInstanceCount = 1;
constexpr unsigned MaxMonochromeTextureSize = 1024;
constexpr unsigned MaxColorTextureSize = 2048;

TextRenderer::TextRenderer() :
    renderer_{},
    monochromeAtlas_{
        0,
        MaxInstanceCount,
        renderer_.maxTextureSize() / renderer_.maxTextureDepth(),
        min(MaxMonochromeTextureSize, renderer_.maxTextureSize()),
        min(MaxMonochromeTextureSize, renderer_.maxTextureSize()),
        GL_R8,
        renderer_.scheduler(),
        "monochromeAtlas"
    },
    colorAtlas_{
        1,
        MaxInstanceCount,
        renderer_.maxTextureSize() / renderer_.maxTextureDepth(),
        min(MaxColorTextureSize, renderer_.maxTextureSize()),
        min(MaxColorTextureSize, renderer_.maxTextureSize()),
        GL_RGBA8,
        renderer_.scheduler(),
        "colorAtlas"
    }
{
}

TextRenderer::~TextRenderer()
{
}

void TextRenderer::setProjection(QMatrix4x4 const& _projection)
{
    renderer_.setProjection(_projection);
}

void TextRenderer::setCellSize(CellSize const& _cellSize)
{
    cellSize_ = _cellSize;
}

void TextRenderer::render(QPoint _pos,
                          vector<GlyphPosition> const& _glyphPositions,
                          QVector4D const& _color)
{
    #if 1
    for (GlyphPosition const& gpos : _glyphPositions)
        if (optional<DataRef> const ti = getTextureInfo(GlyphId{gpos.font, gpos.glyphIndex}); ti.has_value())
            renderTexture(_pos,
                          _color,
                          get<0>(*ti).get(), // TextureInfo
                          get<1>(*ti).get(), // Metadata
                          gpos);
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id)
{
    TextureAtlas& atlas = _id.font.get().hasColor()
        ? colorAtlas_
        : monochromeAtlas_;

    return getTextureInfo(_id, atlas);
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id, TextureAtlas& _atlas)
{
    if (optional<DataRef> const dataRef = _atlas.get(_id); dataRef.has_value())
        return dataRef;

    Font& font = _id.font.get();
    optional<GlyphBitmap> bitmap = font.loadGlyphByIndex(_id.glyphIndex);
    if (!bitmap.has_value())
        return nullopt;

    auto const format = _id.font.get().hasColor() ? GL_RGBA : GL_RED;
    auto const colored = _id.font.get().hasColor() ? 1 : 0;

    //auto const cw = _id.font.get()->glyph->advance.x >> 6;
    // FIXME: this `* 2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.
    auto const ratioX = colored ? static_cast<float>(cellSize_.width) * 2.0f / static_cast<float>(_id.font.get().bitmapWidth()) : 1.0f;
    auto const ratioY = colored ? static_cast<float>(cellSize_.height) / static_cast<float>(_id.font.get().bitmapHeight()) : 1.0f;

    auto metadata = Glyph{};
    metadata.advance = _id.font.get()->glyph->advance.x >> 6;
    metadata.bearing = QPoint(font->glyph->bitmap_left * ratioX, font->glyph->bitmap_top * ratioY);
    metadata.descender = (font->glyph->metrics.height >> 6) - font->glyph->bitmap_top;
    metadata.height = static_cast<unsigned>(font->height) >> 6;
    metadata.size = QPoint(static_cast<int>(font->glyph->bitmap.width), static_cast<int>(font->glyph->bitmap.rows));

#if 0
    if (_id.font.get().hasColor())
    {
        cout << "TextRenderer.insert: colored glyph "
             << _id.glyphIndex
             << ", advance:" << metadata.advance
             << ", descender:" << metadata.descender
             << ", height:" << metadata.height
             << " @ " << _id.font.get().filePath() << endl;
    }
#endif

    auto& bmp = bitmap.value();
    return _atlas.insert(_id, bmp.width, bmp.height,
                         static_cast<unsigned>(static_cast<float>(bmp.width) * ratioX),
                         static_cast<unsigned>(static_cast<float>(bmp.height) * ratioY),
                         format,
                         move(bmp.buffer),
                         colored,
                         metadata);
}

#define LIBTERMINAL_VIEW_NATURAL_COORDS 1
void TextRenderer::renderTexture(QPoint const& _pos,
                                 QVector4D const& _color,
                                 atlas::TextureInfo const& _textureInfo,
                                 Glyph const& _glyph,
                                 GlyphPosition const& _gpos)
{
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
    auto const x = _pos.x() + _gpos.x + _glyph.bearing.x();
    auto const y = _pos.y() + _gpos.y + _gpos.font.get().baseline() - _glyph.descender;
#else
    auto const x = _pos.x()
                 + _gpos.x
                 + _glyph.bearing.x();

    auto const y = _pos.y()
                 + _gpos.font.get().bitmapHeight()
                 + _gpos.y
                 ;
#endif

    // cout << fmt::format(
    //     "Text.render: xy={}:{} pos=({}:{}) gpos=({}:{}), baseline={}, lineHeight={}/{}, descender={}\n",
    //     x, y,
    //     _pos.x(), _pos.y(),
    //     _gpos.x, _gpos.y,
    //     _gpos.font.get().baseline(),
    //     _gpos.font.get().lineHeight(),
    //     _gpos.font.get().bitmapHeight(),
    //     _glyph.descender
    // );

    renderTexture(QPoint(x, y), _color, _textureInfo);

    //auto const z = 0u;
    //renderer_.scheduler().renderTexture({_textureInfo, x, y, z, _color});
}

void TextRenderer::renderTexture(QPoint const& _pos,
                                 QVector4D const& _color,
                                 atlas::TextureInfo const& _textureInfo)
{
    // TODO: actually make x/y/z all signed (for future work, i.e. smooth scrolling!)
    auto const x = static_cast<unsigned>(_pos.x());
    auto const y = static_cast<unsigned>(_pos.y());
    auto const z = 0u;
    renderer_.scheduler().renderTexture({_textureInfo, x, y, z, _color});
}

void TextRenderer::execute()
{
    renderer_.execute();
}

void TextRenderer::clearCache()
{
    monochromeAtlas_.clear();
    colorAtlas_.clear();
}

} // end namespace
