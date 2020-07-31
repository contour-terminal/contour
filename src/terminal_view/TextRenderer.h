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
#pragma once

#include <terminal/Screen.h>
#include <terminal_view/ScreenCoordinates.h>
#include <terminal_view/ShaderConfig.h>
#include <terminal_view/FontConfig.h>

#include <crispy/Atlas.h>
#include <crispy/AtlasRenderer.h>
#include <crispy/FNV.h>
#include <crispy/text/Font.h>
#include <crispy/text/TextShaper.h>

#include <unicode/run_segmenter.h>

#include <QtCore/QPoint>

#include <QtGui/QVector4D>

#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

namespace terminal::view
{
    struct GlyphId {
        std::reference_wrapper<crispy::text::Font> font;
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

    struct CacheKey {
        std::u32string_view text;
        CharacterStyleMask styles;

        bool operator==(CacheKey const& _rhs) const noexcept
        {
            return text == _rhs.text && styles == _rhs.styles;
        }

        bool operator!=(CacheKey const& _rhs) const noexcept
        {
            return !(*this == _rhs);
        }

        bool operator<(CacheKey const& _rhs) const noexcept
        {
            if (text < _rhs.text)
                return true;

            if (static_cast<unsigned>(styles) < static_cast<unsigned>(_rhs.styles))
                return true;

            return false;
        }
    };
}

namespace std
{
    template<>
    struct hash<terminal::view::GlyphId> {
        size_t operator()(terminal::view::GlyphId const& _glyphId) const noexcept
        {
            return hash<crispy::text::Font>{}(_glyphId.font.get()) + _glyphId.glyphIndex;
        }
    };

    template <>
    struct hash<terminal::view::CacheKey> {
        size_t operator()(terminal::view::CacheKey const& _key) const noexcept
        {
            auto fnv = crispy::FNV<char32_t>{};
            return static_cast<size_t>(fnv(fnv(_key.text.data(), _key.text.size()), static_cast<char32_t>(_key.styles)));
        }
    };
}

namespace terminal::view {

struct CellSize {
    int width;
    int height;
};
constexpr bool operator==(CellSize const& a, CellSize const& b) noexcept { return a.width == b.width && a.height == b.height; }
constexpr bool operator!=(CellSize const& a, CellSize const& b) noexcept { return !(a == b); }
// TODO: fmt::formatter<CellSize>

struct RenderMetrics;

/// Text Rendering Pipeline
class TextRenderer {
  public:
    TextRenderer(RenderMetrics& _renderMetrics,
                 crispy::atlas::CommandListener& _commandListener,
                 crispy::atlas::TextureAtlasAllocator& _monochromeAtlasAllocator,
                 crispy::atlas::TextureAtlasAllocator& _colorAtlasAllocator,
                 ScreenCoordinates const& _screenCoordinates,
                 ColorProfile const& _colorProfile,
                 FontConfig const& _fonts);

    void setFont(FontConfig const& _fonts);

    void setCellSize(CellSize const& _cellSize);
    void setColorProfile(ColorProfile const& _colorProfile);

    void schedule(Coordinate const& _pos, Cell const& _cell);
    void flushPendingSegments();
    void finish();

    void debugCache(std::ostream& _textOutput) const;
    void clearCache();

  private:
    void reset(Coordinate const& _pos, GraphicsAttributes const& _attr);
    void extend(Cell const& _cell, cursor_pos_t _column);
    crispy::text::GlyphPositionList prepareRun(unicode::run_segmenter::range const& _range);

    crispy::text::GlyphPositionList const& cachedGlyphPositions();
    crispy::text::GlyphPositionList requestGlyphPositions();

    void render(QPoint _pos,
                std::vector<crispy::text::GlyphPosition> const& glyphPositions,
                QVector4D const& _color);

    /// Renders an arbitrary texture.
    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       crispy::atlas::TextureInfo const& _textureInfo);

  private:
    // rendering
    //
    struct Glyph {
        QPoint size;            // glyph size
        QPoint bearing;         // offset from baseline to left/top of glyph
        int height;
        int descender;
        int advance;            // offset to advance to next glyph in line.
    };
    friend struct fmt::formatter<TextRenderer::Glyph>;

    using TextureAtlas = crispy::atlas::MetadataTextureAtlas<GlyphId, Glyph>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getTextureInfo(GlyphId const& _id);
    std::optional<DataRef> getTextureInfo(GlyphId const& _id, TextureAtlas& _atlas);

    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       crispy::atlas::TextureInfo const& _textureInfo,
                       Glyph const& _glyph,
                       crispy::text::GlyphPosition const& _gpos);

    // general properties
    //
    RenderMetrics& renderMetrics_;
    ScreenCoordinates const& screenCoordinates_;
    ColorProfile colorProfile_; // TODO: make const&, maybe reference_wrapper<>?
    FontConfig fonts_;

    // text run segmentation
    //
    enum class State { Empty, Filling };
    State state_ = State::Empty;
    cursor_pos_t row_ = 1;
    cursor_pos_t startColumn_ = 1;
    GraphicsAttributes attributes_ = {};
    std::vector<char32_t> codepoints_{};
    std::vector<int> clusters_{};
    unsigned clusterOffset_ = 0;

    // text shaping cache
    //
    std::list<std::u32string> cacheKeyStorage_;
    std::unordered_map<CacheKey, crispy::text::GlyphPositionList> cache_;
#if !defined(NDEBUG)
    std::unordered_map<CacheKey, int64_t> cacheHits_;
#endif

    // target surface rendering
    //
    CellSize cellSize_;
    crispy::text::TextShaper textShaper_;
    crispy::atlas::CommandListener& commandListener_;
    TextureAtlas monochromeAtlas_;
    TextureAtlas colorAtlas_;
};

} // end namespace

namespace fmt {
    template <>
    struct formatter<terminal::view::GlyphId> {
        using GlyphId = terminal::view::GlyphId;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(GlyphId const& _glyphId, FormatContext& ctx)
        {
            return format_to(ctx.out(), "GlyphId<index:{}>", _glyphId.glyphIndex);
        }
    };

    template <>
    struct formatter<terminal::view::TextRenderer::Glyph> {
        using Glyph = terminal::view::TextRenderer::Glyph;
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
