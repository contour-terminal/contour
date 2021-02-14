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

#include <terminal_renderer/Atlas.h>

#include <terminal/Color.h>
#include <terminal/Screen.h>

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <crispy/FNV.h>
#include <crispy/point.h>

#include <unicode/run_segmenter.h>

#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace terminal::renderer
{
    using GlyphId = text::glyph_key;

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
    template <>
    struct hash<terminal::renderer::CacheKey> {
        size_t operator()(terminal::renderer::CacheKey const& _key) const noexcept
        {
            auto fnv = crispy::FNV<char32_t>{};
            return static_cast<size_t>(fnv(fnv(_key.text.data(), _key.text.size()), static_cast<char32_t>(_key.styles)));
        }
    };
}

namespace terminal::renderer {

struct GridMetrics;

struct FontDescriptions {
    text::font_size size;
    bool onlyMonospace;                 // indication on how font fallback should search
    text::font_description regular;
    text::font_description bold;
    text::font_description italic;
    text::font_description boldItalic;
    text::font_description emoji;
    text::render_mode renderMode;
};

inline bool operator==(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    return a.size.pt == b.size.pt
        && a.regular == b.regular
        && a.bold == b.bold
        && a.italic == b.italic
        && a.boldItalic == b.boldItalic
        && a.emoji == b.emoji
        && a.renderMode == b.renderMode;
}

inline bool operator!=(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    return !(a == b);
}

struct FontKeys {
    text::font_key regular;
    text::font_key bold;
    text::font_key italic;
    text::font_key boldItalic;
    text::font_key emoji;
};

/// Text Rendering Pipeline
class TextRenderer {
  public:
    TextRenderer(atlas::CommandListener& _commandListener,
                 atlas::TextureAtlasAllocator& _monochromeAtlasAllocator,
                 atlas::TextureAtlasAllocator& _colorAtlasAllocator,
                 atlas::TextureAtlasAllocator& _lcdAtlasAllocator,
                 GridMetrics const& _gridMetrics,
                 text::shaper& _textShaper,
                 FontDescriptions& _fontDescriptions,
                 FontKeys const& _fontKeys);

    void updateFontMetrics();

    void setPressure(bool _pressure) noexcept { pressure_ = _pressure; }

    void schedule(Coordinate const& _pos, Cell const& _cell, RGBColor const& _color);
    void flushPendingSegments();
    void finish();

    void debugCache(std::ostream& _textOutput) const;
    void clearCache();

  private:
    void reset(Coordinate const& _pos, CharacterStyleMask const& _styles, RGBColor const& _color);
    void extend(Cell const& _cell, int _column);
    text::shape_result shapeRun(unicode::run_segmenter::range const& _range);

    text::shape_result const& cachedGlyphPositions();
    text::shape_result requestGlyphPositions();

    void render(crispy::Point _pos,
                std::vector<text::glyph_position> const& glyphPositions,
                RGBAColor const& _color);

    /// Renders an arbitrary texture.
    void renderTexture(crispy::Point const& _pos,
                       RGBAColor const& _color,
                       atlas::TextureInfo const& _textureInfo);

    // rendering
    //
    struct GlyphMetrics {
        text::vec2 bitmapSize;        // glyph size in pixels
        text::vec2 bearing;           // offset baseline and left to top and left of the glyph's bitmap
    };
    friend struct fmt::formatter<GlyphMetrics>;

    using TextureAtlas = atlas::MetadataTextureAtlas<text::glyph_key, GlyphMetrics>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getTextureInfo(GlyphId const& _id);

    void renderTexture(crispy::Point const& _pos,
                       RGBAColor const& _color,
                       atlas::TextureInfo const& _textureInfo,
                       GlyphMetrics const& _glyphMetrics,
                       text::glyph_position const& _gpos);

    TextureAtlas& atlasForFont(text::font_key _font);

    // general properties
    //
    GridMetrics const& gridMetrics_;
    FontDescriptions& fontDescriptions_;
    FontKeys const& fonts_;

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
    std::unordered_map<CacheKey, text::shape_result> cache_;
#if !defined(NDEBUG)
    std::unordered_map<CacheKey, int64_t> cacheHits_;
#endif

    // target surface rendering
    //
    text::shaper& textShaper_;
    atlas::CommandListener& commandListener_;
    TextureAtlas monochromeAtlas_;
    TextureAtlas colorAtlas_;
    TextureAtlas lcdAtlas_;
};

} // end namespace
