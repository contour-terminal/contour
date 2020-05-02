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

#include <crispy/Atlas.h>
#include <crispy/FontManager.h>
#include <crispy/AtlasRenderer.h>

#include <QtCore/QPoint>

#include <map>

namespace crispy::text {

/**
 * High-level OpenGL text shaping API
 */
class TextShaper {
  public:
    TextShaper();

    void setProjection(QMatrix4x4 const& _projection);

    void render(QPoint _pos,
                std::vector<Font::GlyphPosition> const& glyphPositions,
                QVector4D const& _color);

    void execute();

    void clearCache();

  private:
    struct GlyphId {
        std::reference_wrapper<Font> font;
        unsigned glyphIndex;

        bool operator<(GlyphId const& _rhs) const noexcept {
            if (font.get().filePath() < _rhs.font.get().filePath())
                return true;

            if (font.get().filePath() == _rhs.font.get().filePath())
                if (glyphIndex < _rhs.glyphIndex)
                    return true;

            return false;
        }
    };

    struct Glyph {
        unsigned atlasId;
        QPoint size;            // glyph size
        QPoint bearing;         // offset from baseline to left/top of glyph
        unsigned height;
        unsigned descender;
        unsigned advance;       // offset to advance to next glyph in line.
    };

    using TextureAtlas = atlas::TextureAtlas<GlyphId, Glyph>;

    std::optional<std::tuple<atlas::TextureInfo, Glyph>> getTextureInfo(GlyphId const& _id);
    std::optional<std::tuple<atlas::TextureInfo, Glyph>> getTextureInfo(GlyphId const& _id, TextureAtlas& _atlas);

    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       atlas::TextureInfo const& _textureInfo,
                       Glyph const& _glyph,
                       Font::GlyphPosition const& _gpos);

    atlas::Renderer renderer_;
    TextureAtlas monochromeAtlas_;
    TextureAtlas colorAtlas_;
    std::map<GlyphId, Glyph> glyphCache_;
};

} // end namespace
