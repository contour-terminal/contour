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
 * High-level graphics target caching text shaping API.
 */
class TextRenderer {
  public:
    TextRenderer(atlas::CommandListener& _commandListener,
                 atlas::TextureAtlasAllocator& _monochromeAtlasAllocator,
                 atlas::TextureAtlasAllocator& _coloredAtlasAllocator);
    ~TextRenderer();

    void setCellSize(CellSize const& _cellSize);

    void render(QPoint _pos,
                std::vector<GlyphPosition> const& glyphPositions,
                QVector4D const& _color);

    /// Renders an arbitrary texture.
    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       atlas::TextureInfo const& _textureInfo);


    void clearCache();

  private:
    struct Glyph {
        QPoint size;            // glyph size
        QPoint bearing;         // offset from baseline to left/top of glyph
        int height;
        int descender;
        int advance;            // offset to advance to next glyph in line.
    };
    friend struct fmt::formatter<crispy::text::TextRenderer::Glyph>;

    using TextureAtlas = atlas::MetadataTextureAtlas<GlyphId, Glyph>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getTextureInfo(GlyphId const& _id);
    std::optional<DataRef> getTextureInfo(GlyphId const& _id, TextureAtlas& _atlas);

    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       atlas::TextureInfo const& _textureInfo,
                       Glyph const& _glyph,
                       GlyphPosition const& _gpos);

    atlas::CommandListener& commandListener_;
    TextureAtlas monochromeAtlas_;
    TextureAtlas colorAtlas_;
    CellSize cellSize_;
};

} // end namespace

namespace fmt {
    template <>
    struct formatter<crispy::text::GlyphId> {
        using GlyphId = crispy::text::GlyphId;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(GlyphId const& _glyphId, FormatContext& ctx)
        {
            return format_to(ctx.out(), "GlyphId<index:{}>", _glyphId.glyphIndex);
        }
    };

    template <>
    struct formatter<crispy::text::TextRenderer::Glyph> {
        using Glyph = crispy::text::TextRenderer::Glyph;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(Glyph const& _glyph, FormatContext& ctx)
        {
            return format_to(ctx.out(), "size:{}x{}, bearing:{}x{}, height:{}, descender:{}, advance:{}",
                _glyph.size.x(),
                _glyph.size.y(),
                _glyph.bearing.x(),
                _glyph.bearing.y(),
                _glyph.height,
                _glyph.descender,
                _glyph.advance);
        }
    };
}
