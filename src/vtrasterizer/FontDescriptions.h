#pragma once

#include <text_shaper/font.h>

#include <crispy/point.h>

namespace vtrasterizer
{

enum class TextShapingEngine
{
    OpenShaper, //!< Uses open-source implementation: harfbuzz/freetype/fontconfig
    DWrite,     //!< native platform support: Windows
    CoreText,   //!< native platform support: OS/X
};

enum class FontLocatorEngine
{
    Mock,       //!< mock font locator API
    FontConfig, //!< platform independant font locator API
    DWrite,     //!< native platform support: Windows
    CoreText,   //!< native font locator on OS/X
};

using DPI = text::DPI;

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
    FontLocatorEngine fontLocator = FontLocatorEngine::FontConfig;
    bool builtinBoxDrawing = true;
};

inline bool operator==(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    // clang-format off
    return a.size.pt == b.size.pt
        && a.regular == b.regular
        && a.bold == b.bold
        && a.italic == b.italic
        && a.boldItalic == b.boldItalic
        && a.emoji == b.emoji
        && a.renderMode == b.renderMode;
    // clang-format on
}

inline bool operator!=(FontDescriptions const& a, FontDescriptions const& b) noexcept
{
    return !(a == b);
}

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

} // namespace vtrasterizer

// {{{ fmt formatter
template <>
struct fmt::formatter<vtrasterizer::FontLocatorEngine>: fmt::formatter<std::string_view>
{
    auto format(vtrasterizer::FontLocatorEngine value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case vtrasterizer::FontLocatorEngine::CoreText: name = "CoreText"; break;
            case vtrasterizer::FontLocatorEngine::DWrite: name = "DirectWrite"; break;
            case vtrasterizer::FontLocatorEngine::FontConfig: name = "Fontconfig"; break;
            case vtrasterizer::FontLocatorEngine::Mock: name = "Mock"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtrasterizer::TextShapingEngine>: fmt::formatter<std::string_view>
{
    auto format(vtrasterizer::TextShapingEngine value, format_context& ctx) -> format_context::iterator
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
struct fmt::formatter<vtrasterizer::FontDescriptions>: fmt::formatter<std::string>
{
    auto format(vtrasterizer::FontDescriptions const& fd, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("({}, {}, {}, {}, {}, {}, {}, {})",
                                                          fd.size,
                                                          fd.dpi,
                                                          fd.dpiScale,
                                                          fd.regular,
                                                          fd.bold,
                                                          fd.italic,
                                                          fd.boldItalic,
                                                          fd.emoji,
                                                          fd.renderMode),
                                              ctx);
    }
};
// }}}
