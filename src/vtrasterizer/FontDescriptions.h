#pragma once

#include <text_shaper/font.h>

#include <crispy/flags.h>
#include <crispy/point.h>

namespace vtrasterizer
{

enum class TextShapingEngine : uint8_t
{
    OpenShaper, //!< Uses open-source implementation: harfbuzz/freetype/fontconfig
    DWrite,     //!< native platform support: Windows
    CoreText,   //!< native platform support: macOS
};

enum class FontLocatorEngine : uint8_t
{
    Mock,   //!< mock font locator API
    Native, //!< native platform support
};

using DPI = text::DPI;

/// Default maximum number of fallback fonts per key. -1 = unlimited, 0 = disabled.
inline constexpr int DefaultMaxFallbackCount = 16;

struct FontDescriptions
{
    double dpiScale = 1.0;
    DPI dpi = { 0, 0 }; // 0 => auto-fill with defaults
    text::font_size size { 12.0 };
    text::font_description regular;
    text::font_description bold;
    text::font_description italic;
    text::font_description boldItalic;
    text::font_description emoji;
    text::render_mode renderMode;
    TextShapingEngine textShapingEngine = TextShapingEngine::OpenShaper;
    FontLocatorEngine fontLocator = FontLocatorEngine::Native;
    bool builtinBoxDrawing = true;
    int maxFallbackCount =
        DefaultMaxFallbackCount; ///< Maximum fallback fonts per key. -1 = unlimited, 0 = disabled.
};

inline bool operator==(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    // clang-format off
    return a.dpiScale == b.dpiScale
        && a.dpi == b.dpi
        && a.size.pt == b.size.pt
        && a.regular == b.regular
        && a.bold == b.bold
        && a.italic == b.italic
        && a.boldItalic == b.boldItalic
        && a.emoji == b.emoji
        && a.renderMode == b.renderMode
        && a.textShapingEngine == b.textShapingEngine
        && a.fontLocator == b.fontLocator
        && a.builtinBoxDrawing == b.builtinBoxDrawing
        && a.maxFallbackCount == b.maxFallbackCount;
    // clang-format on
}

inline bool operator!=(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    return !(a == b);
}

enum class TextSizeFlag : uint8_t
{
    Normal = 0x00,
    DoubleHeightTop = 0x01,
    DoubleHeightBottom = 0x02,
    DoubleWidth = 0x04,
};

using TextSizeFlags = crispy::flags<TextSizeFlag>;

enum class TextStyle : uint8_t
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

} // namespace vtrasterizer

// {{{ fmt formatter
template <>
struct std::formatter<vtrasterizer::TextStyle>: std::formatter<std::string_view>
{
    auto format(vtrasterizer::TextStyle value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtrasterizer::TextStyle::Invalid: name = "Invalid"; break;
            case vtrasterizer::TextStyle::Regular: name = "Regular"; break;
            case vtrasterizer::TextStyle::Bold: name = "Bold"; break;
            case vtrasterizer::TextStyle::Italic: name = "Italic"; break;
            case vtrasterizer::TextStyle::BoldItalic: name = "BoldItalic"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtrasterizer::TextSizeFlag>: std::formatter<std::string_view>
{
    auto format(vtrasterizer::TextSizeFlag value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtrasterizer::TextSizeFlag::Normal: name = "Normal"; break;
            case vtrasterizer::TextSizeFlag::DoubleHeightTop: name = "DoubleHeightTop"; break;
            case vtrasterizer::TextSizeFlag::DoubleHeightBottom: name = "DoubleHeightBottom"; break;
            case vtrasterizer::TextSizeFlag::DoubleWidth: name = "DoubleWidth"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtrasterizer::FontLocatorEngine>: std::formatter<std::string_view>
{
    auto format(vtrasterizer::FontLocatorEngine value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtrasterizer::FontLocatorEngine::Native: name = "Native"; break;
            case vtrasterizer::FontLocatorEngine::Mock: name = "Mock"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtrasterizer::TextShapingEngine>: std::formatter<std::string_view>
{
    auto format(vtrasterizer::TextShapingEngine value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtrasterizer::TextShapingEngine::CoreText: name = "CoreText"; break;
            case vtrasterizer::TextShapingEngine::DWrite: name = "DirectWrite"; break;
            case vtrasterizer::TextShapingEngine::OpenShaper: name = "harfbuzz"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtrasterizer::FontDescriptions>: std::formatter<std::string>
{
    auto format(vtrasterizer::FontDescriptions const& fd, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("({}, {}, {}, {}, {}, {}, {}, {}, maxFallback={})",
                                                          fd.size,
                                                          fd.dpi,
                                                          fd.dpiScale,
                                                          fd.regular,
                                                          fd.bold,
                                                          fd.italic,
                                                          fd.boldItalic,
                                                          fd.emoji,
                                                          fd.renderMode,
                                                          fd.maxFallbackCount),
                                              ctx);
    }
};
// }}}
