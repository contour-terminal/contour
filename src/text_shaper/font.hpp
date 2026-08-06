// SPDX-License-Identifier: Apache-2.0
#pragma once

#define GLYPH_KEY_DEBUG 1

#if defined(NDEBUG) && defined(GLYPH_KEY_DEBUG)
    #undef GLYPH_KEY_DEBUG
#endif

#include <crispy/FNV.hpp>
#include <crispy/logstore.hpp>
#include <crispy/point.hpp>

#ifdef GLYPH_KEY_DEBUG
    #include <libunicode/convert.h>
    #include <libunicode/width.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace text
{

auto inline const locatorLog = logstore::Category("font.locator", "Logs about font loads.");

namespace detail
{
    template <typename T>
    constexpr std::optional<T> tryMatch(std::string_view text,
                                        std::initializer_list<std::pair<std::string_view, T>> mappings)
    {
        for (auto const& mapping: mappings)
            if (mapping.first == text) // TODO: improvable (ignore case, '_' can be one or many ' ')
                return mapping.second;

        return std::nullopt;
    }
} // namespace detail

struct [[nodiscard]] DPI // NOLINT(readability-identifier-naming)
{
    int x;
    int y;

    // constexpr DPI(DPI const&) = default;
    // DPI& operator=(DPI const&) = default;
    constexpr bool operator!() const noexcept { return !x && !y; }
};

constexpr bool operator==(DPI a, DPI b) noexcept
{
    return a.x == b.x && a.y == b.y;
}

constexpr bool operator!=(DPI a, DPI b) noexcept
{
    return !(a == b);
}

constexpr DPI operator*(DPI dpi, double scale) noexcept
{
    auto const dpiX = static_cast<int>(static_cast<double>(dpi.x) * scale);
    auto const dpiY = static_cast<int>(static_cast<double>(dpi.y) * scale);
    return DPI { .x = dpiX, .y = dpiY };
}

constexpr double average(DPI dpi) noexcept
{
    return 0.5 * static_cast<double>(dpi.x + dpi.y);
}

enum class FontWeight : uint8_t
{
    Thin,
    ExtraLight, // aka. ultralight
    Light,
    DemiLight, // aka. semilight
    Book,
    Normal, // aka. regular
    Medium,
    DemiBold, // aka. semibold
    Bold,
    ExtraBold, // aka. ultrabold
    Black,
    ExtraBlack, // aka. ultrablack
};

constexpr std::optional<FontWeight> makeFontWeight(std::string_view text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::tryMatch(text,
                            { pair { "thin"sv, FontWeight::Thin },
                              pair { "extra light"sv, FontWeight::ExtraLight },
                              pair { "light"sv, FontWeight::Light },
                              pair { "demilight"sv, FontWeight::DemiLight },
                              pair { "book"sv, FontWeight::Book },
                              pair { "normal"sv, FontWeight::Normal },
                              pair { "medium"sv, FontWeight::Medium },
                              pair { "demibold"sv, FontWeight::DemiBold },
                              pair { "bold"sv, FontWeight::Bold },
                              pair { "extra bold"sv, FontWeight::ExtraBlack },
                              pair { "black"sv, FontWeight::Black },
                              pair { "extra black"sv, FontWeight::ExtraBlack } });
}

enum class FontSlant : uint8_t
{
    Normal,
    Italic,
    Oblique
};

constexpr std::optional<FontSlant> makeFontSlant(std::string_view text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::tryMatch(text,
                            { pair { "thin"sv, FontSlant::Normal },
                              pair { "italic"sv, FontSlant::Italic },
                              pair { "oblique"sv, FontSlant::Oblique } });
}

enum class FontSpacing : uint8_t
{
    Proportional,
    Mono
};

constexpr std::optional<FontSpacing> makeFontSpacing(std::string_view text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::tryMatch(
        text, { pair { "proportional"sv, FontSpacing::Proportional }, pair { "mono"sv, FontSpacing::Mono } });
}

struct FontFeature
{
    std::array<char, 4> name; // well defined unique four-letter font feature identifier.
    bool enabled = true;

    FontFeature(char a, char b, char c, char d, bool enabled = true): name { a, b, c, d }, enabled { enabled }
    {
    }

    FontFeature(FontFeature const&) = default;
    FontFeature(FontFeature&&) = default;
    FontFeature& operator=(FontFeature const&) = default;
    FontFeature& operator=(FontFeature&&) = default;
};

struct FontFallbackNone
{
};
struct FontFallbackList
{
    std::vector<std::string> fallbackFonts;
};

struct FontDescription
{
    std::string familyName { "regular" };
#ifdef _WIN32
    std::wstring wFamilyName { L"regular" };
#endif

    FontWeight weight = FontWeight::Normal;
    FontSlant slant = FontSlant::Normal;
    FontSpacing spacing = FontSpacing::Proportional;
    bool strictSpacing = false; // TODO Default value used in config.h while loading fonts

    std::vector<FontFeature> features {};

    // std::monostate for the case when no fallback is defined.
    std::variant<std::monostate, FontFallbackNone, FontFallbackList> fontFallback { std::monostate {} };

    // returns "familyName [weight] [slant]"
    [[nodiscard]] std::string toPattern() const;

    // Parses a font pattern of form "familyName" into a FontDescription."
    [[nodiscard]] static FontDescription parse(std::string_view pattern);
};

inline bool operator==(FontFallbackNone const&, FontFallbackNone const&) noexcept
{
    return true;
}

inline bool operator==(FontFallbackList const& a, FontFallbackList const& b) noexcept
{
    return a.fallbackFonts == b.fallbackFonts;
}

inline bool operator==(FontFeature const& a, FontFeature const& b) noexcept
{
    return a.name == b.name && a.enabled == b.enabled;
}

inline bool operator==(FontDescription const& a, FontDescription const& b)
{
    return a.familyName == b.familyName && a.weight == b.weight && a.slant == b.slant
           && a.spacing == b.spacing && a.strictSpacing == b.strictSpacing && a.features == b.features
           && a.fontFallback == b.fontFallback;
}

inline bool operator!=(FontDescription const& a, FontDescription const& b)
{
    return !(a == b);
}

struct FontMetrics
{
    int lineHeight;
    int advance;
    int ascender;
    int descender;
    int underlinePosition;
    int underlineThickness;
};

// use boxed type
struct FontSize
{
    double pt;
};

constexpr FontSize operator+(FontSize a, FontSize b) noexcept
{
    return FontSize { a.pt + b.pt };
}

constexpr FontSize operator-(FontSize a, FontSize b) noexcept
{
    return FontSize { a.pt - b.pt };
}

constexpr bool operator<(FontSize a, FontSize b) noexcept
{
    return a.pt < b.pt;
}

struct FontKey
{
    unsigned value = 0;
};

constexpr bool operator<(FontKey a, FontKey b) noexcept
{
    return a.value < b.value;
}

constexpr bool operator==(FontKey a, FontKey b) noexcept
{
    return a.value == b.value;
}

struct GlyphIndex
{
    unsigned value;
};

// NB: Ensure this struct does NOT contain padding (or adapt strong hash creation).
struct GlyphKey
{
    FontSize size {};
    FontKey font;
    GlyphIndex index {};

#ifdef GLYPH_KEY_DEBUG
    std::u32string text = {};
    static constexpr bool Debug = true;
#else
    static constexpr bool Debug = false;
#endif
};

constexpr bool operator==(GlyphKey const& a, GlyphKey const& b) noexcept
{
    return a.font.value == b.font.value && a.size.pt == b.size.pt && a.index.value == b.index.value;
}

constexpr bool operator<(GlyphKey const& a, GlyphKey const& b) noexcept
{
    return a.font.value < b.font.value || (a.font.value == b.font.value && a.size.pt < b.size.pt)
           || (a.font.value == b.font.value && a.size.pt == b.size.pt && a.index.value < b.index.value);
}

enum class RenderMode : uint8_t
{
    Bitmap, //!< bitmaps are preferred
    Gray,   //!< gray-scale anti-aliasing
    Light,  //!< gray-scale anti-aliasing for optimized for LCD screens
    LCD,    //!< LCD-optimized anti-aliasing
    Color   //!< embedded color bitmaps are preferred
};

} // namespace text

// {{{ std::hash<>
namespace std
{
template <>
struct hash<text::FontKey>
{
    std::size_t operator()(text::FontKey key) const noexcept { return key.value; } // NOLINT
};

template <>
struct hash<text::GlyphIndex>
{
    std::size_t operator()(text::GlyphIndex index) const noexcept { return index.value; } // NOLINT
};

template <>
struct hash<text::GlyphKey>
{
    std::size_t operator()(text::GlyphKey const& key) const noexcept
    {
        // Pack the three components into disjoint bit fields: font at bits 32..47,
        // glyph index at bits 16..31, and the size (in tenths of a point) at bits 0..15.
        // NB: each component must be masked BEFORE it is shifted into place, and the packing is done
        // in a fixed-width uint64_t rather than in size_t. `<< 32` on a 32-bit size_t (i386, armhf,
        // 32-bit Windows) shifts by the full width of the type, which is undefined: the compiler may
        // fold the font term away entirely and collapse every font's glyphs into the same buckets --
        // exactly the collision this packing exists to avoid. Where size_t cannot hold all 48 bits,
        // the halves are folded together so that all three components still contribute.
        auto const f = static_cast<std::uint64_t>(key.font.value);
        auto const i = static_cast<std::uint64_t>(key.index.value);
        auto const s = static_cast<std::uint64_t>(static_cast<int>(key.size.pt * 10.0));
        auto const packed = ((f & 0xFFFF) << 32) | ((i & 0xFFFF) << 16) | (s & 0xFFFF);

        if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t))
            return static_cast<std::size_t>(packed);
        else
            return static_cast<std::size_t>(packed ^ (packed >> 32));
    }
};

template <>
struct hash<text::FontDescription>
{
    std::size_t operator()(text::FontDescription const& fd) const noexcept
    {
        auto fnv = crispy::FNV<char>();
        auto h = fnv(fnv(fnv(fnv(fnv(fd.familyName), char(fd.weight)), char(fd.slant)), char(fd.spacing)),
                     char(fd.strictSpacing));
        // Include fontFallback variant index and content in the hash
        h = fnv(h, char(fd.fontFallback.index()));
        if (auto const* fallbackList = std::get_if<text::FontFallbackList>(&fd.fontFallback))
            for (auto const& name: fallbackList->fallbackFonts)
                h = fnv(h, name);
        // Include features in the hash
        for (auto const& feature: fd.features)
            h = fnv(fnv(h, std::string_view(feature.name.data(), feature.name.size())),
                    char(feature.enabled));
        return size_t(h);
    }
};
} // namespace std
// }}}

// {{{ fmt formatter
template <>
struct std::formatter<text::DPI>: std::formatter<std::string>
{
    auto format(text::DPI dpi, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}x{}", dpi.x, dpi.y), ctx);
    }
};

template <>
struct std::formatter<text::FontWeight>: formatter<string_view>
{
    auto format(text::FontWeight value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case text::FontWeight::Thin: name = "Thin"; break;
            case text::FontWeight::ExtraLight: name = "ExtraLight"; break;
            case text::FontWeight::Light: name = "Light"; break;
            case text::FontWeight::DemiLight: name = "DemiLight"; break;
            case text::FontWeight::Book: name = "Book"; break;
            case text::FontWeight::Normal: name = "Regular"; break;
            case text::FontWeight::Medium: name = "Medium"; break;
            case text::FontWeight::DemiBold: name = "DemiBold"; break;
            case text::FontWeight::Bold: name = "Bold"; break;
            case text::FontWeight::ExtraBold: name = "ExtraBold"; break;
            case text::FontWeight::Black: name = "Black"; break;
            case text::FontWeight::ExtraBlack: name = "ExtraBlack"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<text::FontSlant>: formatter<string_view>
{
    auto format(text::FontSlant value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case text::FontSlant::Normal: name = "Normal"; break;
            case text::FontSlant::Italic: name = "Italic"; break;
            case text::FontSlant::Oblique: name = "Oblique"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<text::FontSpacing>: formatter<string_view>
{
    auto format(text::FontSpacing value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case text::FontSpacing::Proportional: name = "Proportional"; break;
            case text::FontSpacing::Mono: name = "Monospace"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<text::FontDescription>: std::formatter<std::string>
{
    auto format(text::FontDescription const& desc, auto& ctx) const
    {
        return formatter<std::string>::format(
            std::format("(family={} weight={} slant={} spacing={}, strict_spacing={})",
                        desc.familyName,
                        desc.weight,
                        desc.slant,
                        desc.spacing,
                        desc.strictSpacing ? "yes" : "no"),
            ctx);
    }
};

template <>
struct std::formatter<text::FontMetrics>: std::formatter<std::string>
{
    auto format(text::FontMetrics const& metrics, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("({}, {}, {}, {}, {}, {})",
                                                          metrics.lineHeight,
                                                          metrics.advance,
                                                          metrics.ascender,
                                                          metrics.descender,
                                                          metrics.underlinePosition,
                                                          metrics.underlineThickness),
                                              ctx);
    }
};

template <>
struct std::formatter<text::FontSize>: std::formatter<std::string>
{
    auto format(text::FontSize size, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}pt", size.pt), ctx);
    }
};

template <>
struct std::formatter<text::FontKey>: std::formatter<std::string>
{
    auto format(text::FontKey key, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}", key.value), ctx);
    }
};

template <>
struct std::formatter<text::GlyphIndex>: std::formatter<std::string>
{
    auto format(text::GlyphIndex value, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}", value.value), ctx);
    }
};

template <>
struct std::formatter<text::GlyphKey>: std::formatter<std::string>
{
    auto format(text::GlyphKey const& key, auto& ctx) const
    {
#ifdef GLYPH_KEY_DEBUG
        return formatter<std::string>::format(
            std::format("({}, {}:{}, \"{}\")",
                        key.size,
                        key.font,
                        key.index,
                        unicode::convert_to<char>(std::u32string_view(key.text.data(), key.text.size()))),
            ctx);
#else
        return formatter<std::string>::format(std::format("({}, {}, {})", key.font, key.size, key.index),
                                              ctx);
#endif
    }
};

template <>
struct std::formatter<text::FontFeature>: std::formatter<std::string>
{
    auto format(text::FontFeature value, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}{}{}{}{}",
                                                          value.enabled ? '+' : '-',
                                                          value.name[0],
                                                          value.name[1],
                                                          value.name[2],
                                                          value.name[3]),
                                              ctx);
    }
};

template <>
struct std::formatter<text::RenderMode>: std::formatter<std::string_view>
{
    auto format(text::RenderMode value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case text::RenderMode::Bitmap: name = "Bitmap"; break;
            case text::RenderMode::Gray: name = "Gray"; break;
            case text::RenderMode::Light: name = "Light"; break;
            case text::RenderMode::LCD: name = "LCD"; break;
            case text::RenderMode::Color: name = "Color"; break;
        }
        return std::formatter<string_view>::format(name, ctx);
    }
};
// }}}
