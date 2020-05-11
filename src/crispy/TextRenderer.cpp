/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <crispy/TextRenderer.h>
#include <crispy/reference.h>
#include <iostream>

using namespace std;

namespace crispy::text {

constexpr unsigned MaxInstanceCount = 1;
constexpr unsigned MaxTextureDepth = 10;
constexpr unsigned MaxTextureSize = 1024;

TextRenderer::TextRenderer() :
    renderer_{},
    monochromeAtlas_{
        0,
        MaxInstanceCount,
        min(MaxTextureDepth, renderer_.maxTextureDepth()),
        min(MaxTextureSize, renderer_.maxTextureSize()),
        min(MaxTextureSize, renderer_.maxTextureSize()),
        GL_R8,
        renderer_.scheduler(),
        "monochromeAtlas"
    },
    colorAtlas_{
        1,
        MaxInstanceCount,
        min(MaxTextureDepth, renderer_.maxTextureDepth()),
        min(4096u, renderer_.maxTextureSize()),
        min(4096u, renderer_.maxTextureSize()),
        GL_RGBA8,
        renderer_.scheduler(),
        "colorAtlas",
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

void TextRenderer::render(QPoint _pos,
                        vector<GlyphPosition> const& _glyphPositions,
                        QVector4D const& _color,
                        QSize const& _cellSize)
{
    for (GlyphPosition const& gpos : _glyphPositions)
        if (optional<DataRef> const ti = getTextureInfo(GlyphId{gpos.font, gpos.glyphIndex}, _cellSize); ti.has_value())
            renderTexture(_pos,
                          _color,
                          get<0>(*ti).get(), // TextureInfo
                          get<1>(*ti).get(), // Metadata
                          gpos);
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id, QSize const& _cellSize)
{
    TextureAtlas& atlas = _id.font.get().hasColor()
        ? colorAtlas_
        : monochromeAtlas_;

    return getTextureInfo(_id, _cellSize, atlas);
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id,
                                                         QSize const& _cellSize,
                                                         TextureAtlas& _atlas)
{
    if (optional<DataRef> const dataRef = _atlas.get(_id); dataRef.has_value())
        return dataRef;

    Font& font = _id.font.get();
    GlyphBitmap bitmap = font.loadGlyphByIndex(_id.glyphIndex);

    auto const format = _id.font.get().hasColor() ? GL_BGRA : GL_RED;
    auto const colored = _id.font.get().hasColor() ? 1 : 0;

    //auto const cw = _id.font.get()->glyph->advance.x >> 6;
    // FIXME: this `* 2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.
    auto const ratioX = colored ? static_cast<float>(_cellSize.width()) * 2.0f / static_cast<float>(_id.font.get().bitmapWidth()) : 1.0f;
    auto const ratioY = colored ? static_cast<float>(_cellSize.height()) / static_cast<float>(_id.font.get().bitmapHeight()) : 1.0f;

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

    return _atlas.insert(_id, bitmap.width, bitmap.height,
                         bitmap.width * ratioX,
                         bitmap.height * ratioY,
                         format, move(bitmap.buffer), colored, move(metadata));
}

void TextRenderer::renderTexture(QPoint const& _pos,
                               QVector4D const& _color,
                               atlas::TextureInfo const& _textureInfo,
                               Glyph const& _glyph,
                               GlyphPosition const& _gpos)
{
    unsigned const px = _pos.x() + _gpos.x;
    unsigned const py = _pos.y() + _gpos.y;

    auto const x = static_cast<unsigned>(px + _glyph.bearing.x());
    auto const y = static_cast<unsigned>(py + _gpos.font.get().baseline() - _glyph.descender);
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
