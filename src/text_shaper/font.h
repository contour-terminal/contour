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

#define GLYPH_KEY_DEBUG 1

#if defined(NDEBUG) && defined(GLYPH_KEY_DEBUG)
    #undef GLYPH_KEY_DEBUG
#endif

#include <crispy/FNV.h>
#include <crispy/logstore.h>
#include <crispy/point.h>

#if defined(GLYPH_KEY_DEBUG)
    #include <unicode/convert.h>
    #include <unicode/width.h>
#endif

#include <fmt/format.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace text
{

auto const inline LocatorLog = logstore::Category("font.locator", "Logs about font loads.");

namespace detail
{
    template <typename T>
    constexpr std::optional<T> try_match(std::string_view _text,
                                         std::initializer_list<std::pair<std::string_view, T>> _mappings)
    {
        for (auto const& mapping: _mappings)
            if (mapping.first == _text) // TODO: improvable (ignore case, '_' can be one or many ' ')
                return mapping.second;

        return std::nullopt;
    }
} // namespace detail

struct [[nodiscard]] DPI
{
    int x;
    int y;

    // constexpr DPI(DPI const&) = default;
    // DPI& operator=(DPI const&) = default;
    constexpr bool operator!() noexcept { return !x && !y; }
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
    return DPI { dpiX, dpiY };
}

constexpr double average(DPI dpi) noexcept
{
    return 0.5 * static_cast<double>(dpi.x + dpi.y);
}

enum class font_weight
{
    thin,
    extra_light, // aka. ultralight
    light,
    demilight, // aka. semilight
    book,
    normal, // aka. regular
    medium,
    demibold, // aka. semibold
    bold,
    extra_bold, // aka. ultrabold
    black,
    extra_black, // aka. ultrablack
};

constexpr std::optional<font_weight> make_font_weight(std::string_view _text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::try_match(_text,
                             { pair { "thin"sv, font_weight::thin },
                               pair { "extra light"sv, font_weight::extra_light },
                               pair { "light"sv, font_weight::light },
                               pair { "demilight"sv, font_weight::demilight },
                               pair { "book"sv, font_weight::book },
                               pair { "normal"sv, font_weight::normal },
                               pair { "medium"sv, font_weight::medium },
                               pair { "demibold"sv, font_weight::demibold },
                               pair { "bold"sv, font_weight::bold },
                               pair { "extra bold"sv, font_weight::extra_black },
                               pair { "black"sv, font_weight::black },
                               pair { "extra black"sv, font_weight::extra_black } });
}

enum class font_slant
{
    normal,
    italic,
    oblique
};

constexpr std::optional<font_slant> make_font_slant(std::string_view _text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::try_match(_text,
                             { pair { "thin"sv, font_slant::normal },
                               pair { "italic"sv, font_slant::italic },
                               pair { "oblique"sv, font_slant::oblique } });
}

enum class font_spacing
{
    proportional,
    mono
};

constexpr std::optional<font_spacing> make_font_spacing(std::string_view _text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::try_match(
        _text,
        { pair { "proportional"sv, font_spacing::proportional }, pair { "mono"sv, font_spacing::mono } });
}

struct font_feature
{
    std::array<char, 4> name; // well defined unique four-letter font feature identifier.

    font_feature(char a, char b, char c, char d): name { a, b, c, d } {}
    font_feature(font_feature const&) = default;
    font_feature(font_feature&&) = default;
    font_feature& operator=(font_feature const&) = default;
    font_feature& operator=(font_feature&&) = default;
};

struct font_description
{
    std::string familyName;
#if defined(_WIN32)
    std::wstring wFamilyName;
#endif

    font_weight weight = font_weight::normal;
    font_slant slant = font_slant::normal;
    font_spacing spacing = font_spacing::proportional;
    bool strict_spacing = false;

    std::vector<font_feature> features;

    // returns "familyName [weight] [slant]"
    std::string toPattern() const;

    // Parses a font pattern of form "familyName" into a font_description."
    static font_description parse(std::string_view _pattern);
};

inline bool operator==(font_description const& a, font_description const& b)
{
    return a.familyName == b.familyName && a.weight == b.weight && a.slant == b.slant
           && a.spacing == b.spacing && a.strict_spacing == b.strict_spacing;
}

inline bool operator!=(font_description const& a, font_description const& b)
{
    return !(a == b);
}

struct font_metrics
{
    int line_height;
    int advance;
    int ascender;
    int descender;
    int underline_position;
    int underline_thickness;
};

struct font_size
{
    double pt;
};

constexpr font_size operator+(font_size a, font_size b) noexcept
{
    return font_size { a.pt + b.pt };
}

constexpr font_size operator-(font_size a, font_size b) noexcept
{
    return font_size { a.pt - b.pt };
}

constexpr bool operator<(font_size a, font_size b) noexcept
{
    return a.pt < b.pt;
}

struct font_key
{
    unsigned value;
};

constexpr bool operator<(font_key a, font_key b) noexcept
{
    return a.value < b.value;
}

constexpr bool operator==(font_key a, font_key b) noexcept
{
    return a.value == b.value;
}

struct glyph_index
{
    unsigned value;
};

// NB: Ensure this struct does NOT contain padding (or adapt strong hash creation).
struct glyph_key
{
    font_size size;
    font_key font;
    glyph_index index;

#if defined(GLYPH_KEY_DEBUG)
    std::u32string text;
    static constexpr inline bool Debug = true;
#else
    static constexpr inline bool Debug = false;
#endif
};

constexpr bool operator==(glyph_key const& a, glyph_key const& b) noexcept
{
    return a.font.value == b.font.value && a.size.pt == b.size.pt && a.index.value == b.index.value;
}

constexpr bool operator<(glyph_key const& a, glyph_key const& b) noexcept
{
    return a.font.value < b.font.value || (a.font.value == b.font.value && a.size.pt < b.size.pt)
           || (a.font.value == b.font.value && a.size.pt == b.size.pt && a.index.value < b.index.value);
}

enum class render_mode
{
    bitmap, //!< bitmaps are preferred
    gray,   //!< gray-scale anti-aliasing
    light,  //!< gray-scale anti-aliasing for optimized for LCD screens
    lcd,    //!< LCD-optimized anti-aliasing
    color   //!< embedded color bitmaps are preferred
};

} // namespace text

// {{{ std::hash<>
namespace std
{
template <>
struct hash<text::font_key>
{
    std::size_t operator()(text::font_key _key) const noexcept { return _key.value; }
};

template <>
struct hash<text::glyph_index>
{
    std::size_t operator()(text::glyph_index _index) const noexcept { return _index.value; }
};

template <>
struct hash<text::glyph_key>
{
    std::size_t operator()(text::glyph_key const& _key) const noexcept
    {
        auto const f = _key.font.value;
        auto const i = _key.index.value;
        auto const s = int(_key.size.pt * 10.0);
        return std::size_t(((size_t(f) << 32) & 0xFFFF) | ((i << 16) & 0xFFFF) | (s & 0xFF));
    }
};

template <>
struct hash<text::font_description>
{
    std::size_t operator()(text::font_description const& fd) const noexcept
    {
        auto fnv = crispy::FNV<char>();
        return size_t(
            fnv(fnv(fnv(fnv(fnv(fd.familyName), char(fd.weight)), char(fd.slant)), char(fd.spacing)),
                char(fd.strict_spacing)));
    }
};
} // namespace std
// }}}

// {{{ fmt formatter
namespace fmt
{

template <>
struct formatter<text::DPI>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::DPI dpi, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}x{}", dpi.x, dpi.y);
    }
};

template <>
struct formatter<text::font_weight>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_weight _value, FormatContext& ctx)
    {
        switch (_value)
        {
        case text::font_weight::thin: return format_to(ctx.out(), "Thin");
        case text::font_weight::extra_light: return format_to(ctx.out(), "ExtraLight");
        case text::font_weight::light: return format_to(ctx.out(), "Light");
        case text::font_weight::demilight: return format_to(ctx.out(), "DemiLight");
        case text::font_weight::book: return format_to(ctx.out(), "Book");
        case text::font_weight::normal: return format_to(ctx.out(), "Regular");
        case text::font_weight::medium: return format_to(ctx.out(), "Medium");
        case text::font_weight::demibold: return format_to(ctx.out(), "DemiBold");
        case text::font_weight::bold: return format_to(ctx.out(), "Bold");
        case text::font_weight::extra_bold: return format_to(ctx.out(), "ExtraBold");
        case text::font_weight::black: return format_to(ctx.out(), "Black");
        case text::font_weight::extra_black: return format_to(ctx.out(), "ExtraBlack");
        }
        return format_to(ctx.out(), "({})", unsigned(_value));
    }
};

template <>
struct formatter<text::font_slant>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_slant _value, FormatContext& ctx)
    {
        switch (_value)
        {
        case text::font_slant::normal: return format_to(ctx.out(), "Roman");
        case text::font_slant::italic: return format_to(ctx.out(), "Italic");
        case text::font_slant::oblique: return format_to(ctx.out(), "Oblique");
        }
        return format_to(ctx.out(), "({})", unsigned(_value));
    }
};

template <>
struct formatter<text::font_spacing>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_spacing _value, FormatContext& ctx)
    {
        switch (_value)
        {
        case text::font_spacing::proportional: return format_to(ctx.out(), "Proportional");
        case text::font_spacing::mono: return format_to(ctx.out(), "Monospace");
        }
        return format_to(ctx.out(), "({})", unsigned(_value));
    }
};

template <>
struct formatter<text::font_description>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_description const& _desc, FormatContext& ctx)
    {
        return format_to(ctx.out(),
                         "(family={} weight={} slant={} spacing={}, strict_spacing={})",
                         _desc.familyName,
                         _desc.weight,
                         _desc.slant,
                         _desc.spacing,
                         _desc.strict_spacing ? "yes" : "no");
    }
};

template <>
struct formatter<text::font_metrics>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_metrics const& _metrics, FormatContext& ctx)
    {
        return format_to(ctx.out(),
                         "({}, {}, {}, {}, {}, {})",
                         _metrics.line_height,
                         _metrics.advance,
                         _metrics.ascender,
                         _metrics.descender,
                         _metrics.underline_position,
                         _metrics.underline_thickness);
    }
};

template <>
struct formatter<text::font_size>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_size _size, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}pt", _size.pt);
    }
};

template <>
struct formatter<text::font_key>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_key _key, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", _key.value);
    }
};

template <>
struct formatter<text::glyph_index>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::glyph_index _value, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", _value.value);
    }
};

template <>
struct formatter<text::glyph_key>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::glyph_key const& _key, FormatContext& ctx)
    {
#if defined(GLYPH_KEY_DEBUG)
        return format_to(ctx.out(),
                         "({}, {}:{}, \"{}\")",
                         _key.size,
                         _key.font,
                         _key.index,
                         unicode::convert_to<char>(std::u32string_view(_key.text.data(), _key.text.size())));
#else
        return format_to(ctx.out(), "({}, {}, {})", _key.font, _key.size, _key.index);
#endif
    }
};

template <>
struct formatter<text::font_feature>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_feature _value, FormatContext& ctx)
    {
        return format_to(
            ctx.out(), "{}{}{}{}", _value.name[0], _value.name[1], _value.name[2], _value.name[3]);
    }
};

template <>
struct formatter<text::render_mode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::render_mode _value, FormatContext& ctx)
    {
        switch (_value)
        {
        case text::render_mode::bitmap: return format_to(ctx.out(), "Bitmap");
        case text::render_mode::gray: return format_to(ctx.out(), "Gray");
        case text::render_mode::light: return format_to(ctx.out(), "Light");
        case text::render_mode::lcd: return format_to(ctx.out(), "LCD");
        case text::render_mode::color: return format_to(ctx.out(), "Color");
        }
        return format_to(ctx.out(), "({})", unsigned(_value));
    }
};
} // namespace fmt
// }}}
