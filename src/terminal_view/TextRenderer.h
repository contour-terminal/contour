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
#include <terminal_view/ShaderConfig.h>
#include <terminal_view/FontConfig.h>

#include <crispy/Atlas.h>
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

struct GridMetrics;
struct RenderMetrics;

/// Text Rendering Pipeline
class TextRenderer {
  public:
    TextRenderer(RenderMetrics& _renderMetrics,
                 crispy::atlas::CommandListener& _commandListener,
                 crispy::atlas::TextureAtlasAllocator& _monochromeAtlasAllocator,
                 crispy::atlas::TextureAtlasAllocator& _colorAtlasAllocator,
                 crispy::atlas::TextureAtlasAllocator& _lcdAtlasAllocator,
                 GridMetrics const& _gridMetrics,
                 FontConfig const& _fonts);

    void setFont(FontConfig const& _fonts);

    void setPressure(bool _pressure) noexcept { pressure_ = _pressure; }

    void schedule(Coordinate const& _pos, Cell const& _cell, RGBColor const& _color);
    void flushPendingSegments();
    void finish();

    void debugCache(std::ostream& _textOutput) const;
    void clearCache();

  private:
    void reset(Coordinate const& _pos, CharacterStyleMask const& _styles, RGBColor const& _color);
    void extend(Cell const& _cell, int _column);
    crispy::text::GlyphPositionList shapeRun(unicode::run_segmenter::range const& _range);

    crispy::text::GlyphPositionList const& cachedGlyphPositions();
    crispy::text::GlyphPositionList requestGlyphPositions();

    void render(QPoint _pos,
                std::vector<crispy::text::GlyphPosition> const& glyphPositions,
                QVector4D const& _color);

    /// Renders an arbitrary texture.
    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       crispy::atlas::TextureInfo const& _textureInfo);

    // rendering
    //
    using Glyph = crispy::text::Glyph;
    using GlyphMetrics = crispy::text::GlyphMetrics;
    friend struct fmt::formatter<GlyphMetrics>;

    using TextureAtlas = crispy::atlas::MetadataTextureAtlas<GlyphId, GlyphMetrics>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getTextureInfo(GlyphId const& _id);

    void renderTexture(QPoint const& _pos,
                       QVector4D const& _color,
                       crispy::atlas::TextureInfo const& _textureInfo,
                       GlyphMetrics const& _glyphMetrics,
                       crispy::text::GlyphPosition const& _gpos);

    // general properties
    //
    RenderMetrics& renderMetrics_;
    GridMetrics const& gridMetrics_;
    FontConfig fonts_;

    // text run segmentation
    //
    enum class State { Empty, Filling };
    State state_ = State::Empty;
    int row_ = 1;
    int startColumn_ = 1;
    CharacterStyleMask characterStyleMask_ = {};
    RGBColor color_{};
    std::vector<char32_t> codepoints_{};
    std::vector<int> clusters_{};
    unsigned clusterOffset_ = 0;

    // performance optimizations
    //
    bool pressure_ = false;

    // text shaping cache
    //
    std::list<std::u32string> cacheKeyStorage_;
    std::unordered_map<CacheKey, crispy::text::GlyphPositionList> cache_;
#if !defined(NDEBUG)
    std::unordered_map<CacheKey, int64_t> cacheHits_;
#endif

    // target surface rendering
    //
    crispy::text::TextShaper textShaper_;
    crispy::atlas::CommandListener& commandListener_;
    TextureAtlas monochromeAtlas_;
    TextureAtlas colorAtlas_;
    TextureAtlas lcdAtlas_;
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
}
