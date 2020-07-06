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
#include <crispy/AtlasRenderer.h>
#include <crispy/text/Font.h>

#include <QtCore/QPoint>

#include <functional>
#include <unordered_map>

namespace crispy::text {
    struct GlyphId {
        std::reference_wrapper<Font> font;
        unsigned glyphIndex;
    };

    inline bool operator==(GlyphId const& _lhs, GlyphId const& _rhs) noexcept {
        return _lhs.font.get().filePath() == _rhs.font.get().filePath() && _lhs.glyphIndex == _rhs.glyphIndex;
    }

    inline bool operator<(GlyphId const& _lhs, GlyphId const& _rhs) noexcept {
        if (_lhs.font.get().filePath() < _rhs.font.get().filePath())
            return true;

        if (_lhs.font.get().filePath() == _rhs.font.get().filePath())
            return _lhs.glyphIndex < _rhs.glyphIndex;

        return false;
    }
}

namespace std {
    template<>
    struct hash<crispy::text::GlyphId> {
        size_t operator()(crispy::text::GlyphId const& _glyphId) const noexcept
        {
            return hash<crispy::text::Font>{}(_glyphId.font.get()) + _glyphId.glyphIndex;
        }
    };
}

namespace crispy::text {

struct CellSize {
    unsigned width;
    unsigned height;
};
constexpr bool operator==(CellSize const& a, CellSize const& b) noexcept { return a.width == b.width && a.height == b.height; }
constexpr bool operator!=(CellSize const& a, CellSize const& b) noexcept { return !(a == b); }
// TODO: fmt::formatter<CellSize>

/**
 * High-level OpenGL text shaping API
 */
class TextRenderer {
  public:
    TextRenderer();
    ~TextRenderer();

    void setProjection(QMatrix4x4 const& _projection);
    void setCellSize(CellSize const& _cellSize);

    void render(QPoint _pos,
                std::vector<GlyphPosition> const& glyphPositions,
                QVector4D const& _color);

    void execute();

    /// Clears the render cache.
    void clearCache();

    /// Renders an arbitrary texture.
    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       atlas::TextureInfo const& _textureInfo);

    bool empty() const noexcept { return renderer_.empty(); }

  private:
    struct Glyph {
        QPoint size;            // glyph size
        QPoint bearing;         // offset from baseline to left/top of glyph
        int height;
        int descender;
        int advance;            // offset to advance to next glyph in line.
    };

    using TextureAtlas = atlas::TextureAtlas<GlyphId, Glyph>;
    using DataRef = TextRenderer::TextureAtlas::DataRef;

    std::optional<DataRef> getTextureInfo(GlyphId const& _id);
    std::optional<DataRef> getTextureInfo(GlyphId const& _id, TextureAtlas& _atlas);

    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       atlas::TextureInfo const& _textureInfo,
                       Glyph const& _glyph,
                       GlyphPosition const& _gpos);

    atlas::Renderer renderer_;
    TextureAtlas monochromeAtlas_;
    TextureAtlas colorAtlas_;
    CellSize cellSize_;
};

} // end namespace
