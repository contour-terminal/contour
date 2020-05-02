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
#include <crispy/TextShaper.h>
#include <iostream>

using namespace std;

namespace crispy::text {

TextShaper::TextShaper() :
    renderer_{},
    monochromeAtlas_{
        10,
        min(10u, renderer_.maxTextureDepth()),
        min(1024u, renderer_.maxTextureSize()),
        min(1024u, renderer_.maxTextureSize()),
        "monochromeAtlas"
    },
    colorAtlas_{
        10,
        min(10u, renderer_.maxTextureDepth()),
        min(1024u, renderer_.maxTextureSize()),
        min(1024u, renderer_.maxTextureSize()),
        "colorAtlas",
    },
    glyphCache_{}
{
}

void TextShaper::setProjection(QMatrix4x4 const& _projection)
{
    renderer_.setProjection(_projection);
}

void TextShaper::render(QPoint _pos,
                        vector<Font::GlyphPosition> const& _glyphPositions,
                        QVector4D const& _color)
{
    for (Font::GlyphPosition const& gpos : _glyphPositions)
        if (auto const ti = getTextureInfo(GlyphId{gpos.font, gpos.glyphIndex}); ti.has_value())
            renderTexture(_pos, _color, get<atlas::TextureInfo>(*ti), get<Glyph>(*ti), gpos);
}

optional<tuple<atlas::TextureInfo, TextShaper::Glyph>> TextShaper::getTextureInfo(GlyphId const& _id)
{
    TextureAtlas& atlas = _id.font.get().hasColor()
        ? colorAtlas_
        : monochromeAtlas_;

    return getTextureInfo(_id, atlas);
}

optional<tuple<atlas::TextureInfo, TextShaper::Glyph>> TextShaper::getTextureInfo(GlyphId const& _id,
                                                                                  TextureAtlas& _atlas)
{
    if (atlas::TextureInfo const* ti = _atlas.get(_id); ti != nullptr)
        return tuple{*ti, _atlas.metadata(_id)};

    Font& font = _id.font.get();
    Font::Glyph fg = font.loadGlyphByIndex(_id.glyphIndex);

    auto metadata = Glyph{};
    metadata.advance = _id.font.get()->glyph->advance.x >> 6;
    metadata.bearing = QPoint(font->glyph->bitmap_left, font->glyph->bitmap_top);
    metadata.atlasId = 0; // TODO
    metadata.descender = font->glyph->metrics.height / 64 - font->glyph->bitmap_top;
    metadata.height = static_cast<unsigned>(font->height) / 64;
    metadata.size = QPoint(static_cast<int>(font->glyph->bitmap.width), static_cast<int>(font->glyph->bitmap.rows));

    if (_atlas.insert(_id, fg.width, fg.height, fg.buffer, move(metadata)))
    {
        renderer_.schedule(_atlas.commandQueue());
        _atlas.commandQueue().clear();
        return tuple{*_atlas.get(_id), _atlas.metadata(_id)};
    }

    return nullopt;
}

void TextShaper::renderTexture(QPoint const& _pos,
                               QVector4D const& _color,
                               atlas::TextureInfo const& _textureInfo,
                               Glyph const& _glyph,
                               Font::GlyphPosition const& _gpos)
{
    unsigned const x = _pos.x() + _gpos.x;
    unsigned const y = _pos.y() + _gpos.y;

    auto const xpos = static_cast<unsigned>(x + _glyph.bearing.x());
    auto const ypos = static_cast<unsigned>(y + _gpos.font.get().baseline() - _glyph.descender);
    auto const w = static_cast<unsigned>(_glyph.size.x());
    auto const h = static_cast<unsigned>(_glyph.size.y());

    renderer_.schedule({
        atlas::RenderTexture{
            _textureInfo,
            xpos,
            ypos,
            0, // z
            w,
            h,
            _color
        }
    });
}

void TextShaper::execute()
{
    renderer_.execute();
}

void TextShaper::clearCache()
{
    monochromeAtlas_.clear();
    renderer_.schedule(monochromeAtlas_.commandQueue());
    monochromeAtlas_.commandQueue().clear();

    colorAtlas_.clear();
    renderer_.schedule(colorAtlas_.commandQueue());
    colorAtlas_.commandQueue().clear();

    // for (auto& atlas : {ref(monochromeAtlas_), ref(colorAtlas_)})
    // {
    //     atlas.get().clear();
    //     renderer_.schedule(atlas.get().commandQueue());
    //     atlas.get().commandQueue().clear();
    // }
}


} // end namespace
