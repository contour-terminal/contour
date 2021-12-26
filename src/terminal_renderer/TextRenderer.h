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

#include <terminal/Color.h>
#include <terminal/RenderBuffer.h>
#include <terminal/Screen.h>

#include <terminal_renderer/Atlas.h>
#include <terminal_renderer/BoxDrawingRenderer.h>
#include <terminal_renderer/RenderTarget.h>

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <crispy/FNV.h>
#include <crispy/LRUCache.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <unicode/convert.h>
#include <unicode/run_segmenter.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace terminal::renderer
{

enum class TextStyle
{
    Invalid = 0x00,
    Regular = 0x10,
    Bold = 0x11,
    Italic = 0x12,
    BoldItalic = 0x13,
};

constexpr TextStyle operator|(TextStyle a, TextStyle b) noexcept
{
    return static_cast<TextStyle>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool operator<(TextStyle a, TextStyle b) noexcept
{
    return static_cast<unsigned>(a) < static_cast<unsigned>(b);
}

struct TextCacheKey
{
    std::u32string_view text;
    TextStyle style;

    constexpr bool operator<(TextCacheKey const& _rhs) const noexcept
    {
        if (text < _rhs.text)
            return true;

        return text < _rhs.text || style < _rhs.style;
    }

    constexpr bool operator==(TextCacheKey const& _rhs) const noexcept
    {
        return text == _rhs.text && style == _rhs.style;
    }

    constexpr bool operator!=(TextCacheKey const& _rhs) const noexcept { return !(*this == _rhs); }
};

} // end namespace terminal::renderer

namespace std
{
template <>
struct hash<terminal::renderer::TextCacheKey>
{
    size_t operator()(terminal::renderer::TextCacheKey const& _key) const noexcept
    {
        auto fnv = crispy::FNV<char32_t> {};
        return static_cast<size_t>(fnv(fnv.basis(), _key.text, static_cast<char32_t>(_key.style)));
    }
};
} // namespace std

namespace terminal::renderer
{

struct GridMetrics;

enum class TextShapingEngine
{
    OpenShaper, //!< Uses open-source implementation: harfbuzz/freetype/fontconfig
    DWrite,     //!< native platform support: Windows
    CoreText,   //!< native platform support: OS/X
};

enum class FontLocatorEngine
{
    FontConfig, //!< platform independant font locator API
    DWrite,     //!< native platform support: Windows
    CoreText,   //!< native font locator on OS/X
};

struct FontDescriptions
{
    double dpiScale = 1.0;
    crispy::Point dpi = { 0, 0 }; // 0 => auto-fill with defaults
    text::font_size size;
    text::font_description regular;
    text::font_description bold;
    text::font_description italic;
    text::font_description boldItalic;
    text::font_description emoji;
    text::render_mode renderMode;
    TextShapingEngine textShapingEngine = TextShapingEngine::OpenShaper;
    FontLocatorEngine fontLocator = FontLocatorEngine::FontConfig;
    bool builtinBoxDrawing = true;
};

inline bool operator==(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    return a.size.pt == b.size.pt && a.regular == b.regular && a.bold == b.bold && a.italic == b.italic
           && a.boldItalic == b.boldItalic && a.emoji == b.emoji && a.renderMode == b.renderMode;
}

inline bool operator!=(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    return !(a == b);
}

struct FontKeys
{
    text::font_key regular;
    text::font_key bold;
    text::font_key italic;
    text::font_key boldItalic;
    text::font_key emoji;
};

/// Text Rendering Pipeline
class TextRenderer: public Renderable
{
  public:
    TextRenderer(GridMetrics const& _gridMetrics,
                 text::shaper& _textShaper,
                 FontDescriptions& _fontDescriptions,
                 FontKeys const& _fontKeys);

    void setRenderTarget(RenderTarget& _renderTarget) override;

    void debugCache(std::ostream& _textOutput) const;
    void clearCache() override;

    void updateFontMetrics();

    void setPressure(bool _pressure) noexcept { pressure_ = _pressure; }

    /// Must be invoked before a new terminal frame is rendered.
    void beginFrame();

    /// Renders a given terminal's grid cell that has been
    /// transformed into a RenderCell.
    void renderCell(RenderCell const& _cell);

    /// Must be invoked when rendering the terminal's text has finished for this frame.
    void endFrame();

  private:
    /// Puts a sequence of codepoints that belong to the same grid cell at @p _pos
    /// at the end of the currently filled line.
    void appendCell(gsl::span<char32_t const> _codepoints, TextStyle _style, RGBColor _color);
    text::shape_result const& cachedGlyphPositions();
    text::shape_result requestGlyphPositions();
    text::shape_result shapeRun(unicode::run_segmenter::range const& _run);
    void endSequence();

    void renderRun(crispy::Point _startPos,
                   gsl::span<text::glyph_position const> _glyphPositions,
                   RGBColor _color);

    /// Renders an arbitrary texture.
    void renderTexture(crispy::Point const& _pos,
                       RGBAColor const& _color,
                       atlas::TextureInfo const& _textureInfo);

    // rendering
    //
    struct GlyphMetrics
    {
        ImageSize bitmapSize;  // glyph size in pixels
        crispy::Point bearing; // offset baseline and left to top and left of the glyph's bitmap
    };

    using TextureAtlas = atlas::MetadataTextureAtlas<text::glyph_key, GlyphMetrics>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getTextureInfo(text::glyph_key const& _id,
                                          unicode::PresentationStyle _presentation);

    void renderTexture(crispy::Point const& _pos,
                       RGBAColor const& _color,
                       atlas::TextureInfo const& _textureInfo,
                       GlyphMetrics const& _glyphMetrics,
                       text::glyph_position const& _gpos);

    TextureAtlas* atlasForBitmapFormat(text::bitmap_format _format) noexcept
    {
        switch (_format)
        {
        case text::bitmap_format::alpha_mask: return monochromeAtlas_.get();
        case text::bitmap_format::rgba: return colorAtlas_.get();
        case text::bitmap_format::rgb: return lcdAtlas_.get();
        default: return nullptr; // Should NEVER EVER happen.
        }
    }

    // general properties
    //
    GridMetrics const& gridMetrics_;
    FontDescriptions& fontDescriptions_;
    FontKeys const& fonts_;

    // performance optimizations
    //
    bool pressure_ = false;

    std::unordered_map<text::glyph_key, text::bitmap_format> glyphToTextureMapping_;

    // target surface rendering
    //
    text::shaper& textShaper_;
    std::unique_ptr<TextureAtlas> monochromeAtlas_;
    std::unique_ptr<TextureAtlas> colorAtlas_;
    std::unique_ptr<TextureAtlas> lcdAtlas_;

    // sub-renderer
    //
    BoxDrawingRenderer boxDrawingRenderer_;

    // render states
    TextStyle style_ = TextStyle::Invalid;
    RGBColor color_ {};

    crispy::Point textPosition_;
    std::vector<char32_t> codepoints_;
    std::vector<unsigned> clusters_;
    unsigned cellCount_ = 0;
    bool textStartFound_ = false;
    bool forceCellGroupSplit_ = false;

    // text shaping cache
    //
    std::list<std::u32string> cacheKeyStorage_;
    crispy::LRUCache<TextCacheKey, text::shape_result> cache_;

    // output fields
    //
    std::vector<text::shape_result> shapedLines_;
};

} // namespace terminal::renderer

namespace fmt
{ // {{{
template <>
struct formatter<terminal::renderer::FontDescriptions>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::renderer::FontDescriptions const& fd, FormatContext& ctx)
    {
        return format_to(ctx.out(),
                         "({}, {}, {}, {}, {}, {})",
                         fd.size,
                         fd.regular,
                         fd.bold,
                         fd.italic,
                         fd.boldItalic,
                         fd.emoji,
                         fd.renderMode);
    }
};

template <>
struct formatter<terminal::renderer::FontLocatorEngine>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::renderer::FontLocatorEngine value, FormatContext& ctx)
    {
        using terminal::renderer::FontLocatorEngine;
        switch (value)
        {
        case FontLocatorEngine::FontConfig: return format_to(ctx.out(), "FontConfig");
        case FontLocatorEngine::DWrite: return format_to(ctx.out(), "DirectWrite");
        case FontLocatorEngine::CoreText: return format_to(ctx.out(), "CoreText");
        }
        return format_to(ctx.out(), "UNKNOWN");
    }
};

template <>
struct formatter<terminal::renderer::TextShapingEngine>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::renderer::TextShapingEngine value, FormatContext& ctx)
    {
        using terminal::renderer::TextShapingEngine;
        switch (value)
        {
        case TextShapingEngine::OpenShaper: return format_to(ctx.out(), "OpenShaper");
        case TextShapingEngine::DWrite: return format_to(ctx.out(), "DirectWrite");
        case TextShapingEngine::CoreText: return format_to(ctx.out(), "CoreText");
        }
        return format_to(ctx.out(), "UNKNOWN");
    }
};

template <>
struct formatter<terminal::renderer::TextCacheKey>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::renderer::TextCacheKey value, FormatContext& ctx)
    {
        return format_to(ctx.out(), "({}, \"{}\")", value.style, unicode::convert_to<char>(value.text));
    }
};

} // namespace fmt
