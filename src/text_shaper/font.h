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
    #include <libunicode/convert.h>
    #include <libunicode/width.h>
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
    constexpr std::optional<T> try_match(std::string_view text,
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
    return DPI { dpiX, dpiY };
}

constexpr double average(DPI dpi) noexcept
{
    return 0.5 * static_cast<double>(dpi.x + dpi.y);
}

// NOLINTBEGIN(readability-identifier-naming)
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
// NOLINTEND(readability-identifier-naming)

constexpr std::optional<font_weight> make_font_weight(std::string_view text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::try_match(text,
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

// NOLINTBEGIN(readability-identifier-naming)
enum class font_slant
{
    normal,
    italic,
    oblique
};
// NOLINTEND(readability-identifier-naming)

constexpr std::optional<font_slant> make_font_slant(std::string_view text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::try_match(text,
                             { pair { "thin"sv, font_slant::normal },
                               pair { "italic"sv, font_slant::italic },
                               pair { "oblique"sv, font_slant::oblique } });
}

// NOLINTBEGIN(readability-identifier-naming)
enum class font_spacing
{
    proportional,
    mono
};
// NOLINTEND(readability-identifier-naming)

constexpr std::optional<font_spacing> make_font_spacing(std::string_view text)
{
    using namespace std::string_view_literals;
    using std::pair;
    return detail::try_match(
        text,
        { pair { "proportional"sv, font_spacing::proportional }, pair { "mono"sv, font_spacing::mono } });
}

struct font_feature
{
    std::array<char, 4> name; // well defined unique four-letter font feature identifier.
    bool enabled = true;

    font_feature(char a, char b, char c, char d, bool enabled = true):
        name { a, b, c, d }, enabled { enabled }
    {
    }

    font_feature(font_feature const&) = default;
    font_feature(font_feature&&) = default;
    font_feature& operator=(font_feature const&) = default;
    font_feature& operator=(font_feature&&) = default;
};

struct font_description
{
    std::string familyName { "regular" };
#if defined(_WIN32)
    std::wstring wFamilyName { L"regular" };
#endif

    font_weight weight = font_weight::normal;
    font_slant slant = font_slant::normal;
    font_spacing spacing = font_spacing::proportional;
    bool strict_spacing = false;

    std::vector<font_feature> features;

    // returns "familyName [weight] [slant]"
    [[nodiscard]] std::string toPattern() const;

    // Parses a font pattern of form "familyName" into a font_description."
    [[nodiscard]] static font_description parse(std::string_view pattern);
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
    unsigned value = 0;
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
    std::u32string text = {};
    static constexpr inline bool debug = true;
#else
    static constexpr inline bool debug = false;
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

// NOLINTBEGIN(readability-identifier-naming)
enum class render_mode
{
    bitmap, //!< bitmaps are preferred
    gray,   //!< gray-scale anti-aliasing
    light,  //!< gray-scale anti-aliasing for optimized for LCD screens
    lcd,    //!< LCD-optimized anti-aliasing
    color   //!< embedded color bitmaps are preferred
};
// NOLINTEND(readability-identifier-naming)

} // namespace text

// {{{ std::hash<>
namespace std
{
template <>
struct hash<text::font_key>
{
    std::size_t operator()(text::font_key key) const noexcept { return key.value; }
};

template <>
struct hash<text::glyph_index>
{
    std::size_t operator()(text::glyph_index index) const noexcept { return index.value; }
};

template <>
struct hash<text::glyph_key>
{
    std::size_t operator()(text::glyph_key const& key) const noexcept
    {
        auto const f = key.font.value;
        auto const i = key.index.value;
        auto const s = int(key.size.pt * 10.0);
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
template <>
struct fmt::formatter<text::DPI>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(text::DPI dpi, format_context& ctx) -> format_context::iterator
    {
        return fmt::format_to(ctx.out(), "{}x{}", dpi.x, dpi.y);
    }
};

template <>
struct fmt::formatter<text::font_weight>: formatter<string_view>
{
    static auto format(text::font_weight value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case text::font_weight::thin: name = "Thin"; break;
            case text::font_weight::extra_light: name = "ExtraLight"; break;
            case text::font_weight::light: name = "Light"; break;
            case text::font_weight::demilight: name = "DemiLight"; break;
            case text::font_weight::book: name = "Book"; break;
            case text::font_weight::normal: name = "Regular"; break;
            case text::font_weight::medium: name = "Medium"; break;
            case text::font_weight::demibold: name = "DemiBold"; break;
            case text::font_weight::bold: name = "Bold"; break;
            case text::font_weight::extra_bold: name = "ExtraBold"; break;
            case text::font_weight::black: name = "Black"; break;
            case text::font_weight::extra_black: name = "ExtraBlack"; break;
        }
        return fmt::formatter<string_view> {}.format(name, ctx);
    }
};

template <>
struct fmt::formatter<text::font_slant>: formatter<string_view>
{
    static auto format(text::font_slant value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case text::font_slant::normal: name = "Roman"; break;
            case text::font_slant::italic: name = "Italic"; break;
            case text::font_slant::oblique: name = "Oblique"; break;
        }
        return fmt::formatter<string_view> {}.format(name, ctx);
    }
};

template <>
struct fmt::formatter<text::font_spacing>: formatter<string_view>
{
    static auto format(text::font_spacing value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case text::font_spacing::proportional: name = "Proportional"; break;
            case text::font_spacing::mono: name = "Monospace"; break;
        }
        return fmt::formatter<string_view> {}.format(name, ctx);
    }
};

template <>
struct fmt::formatter<text::font_description>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(text::font_description const& desc, format_context& ctx) -> format_context::iterator
    {
        return fmt::format_to(ctx.out(),
                              "(family={} weight={} slant={} spacing={}, strict_spacing={})",
                              desc.familyName,
                              desc.weight,
                              desc.slant,
                              desc.spacing,
                              desc.strict_spacing ? "yes" : "no");
    }
};

template <>
struct fmt::formatter<text::font_metrics>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_metrics const& metrics, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(),
                              "({}, {}, {}, {}, {}, {})",
                              metrics.line_height,
                              metrics.advance,
                              metrics.ascender,
                              metrics.descender,
                              metrics.underline_position,
                              metrics.underline_thickness);
    }
};

template <>
struct fmt::formatter<text::font_size>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_size size, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}pt", size.pt);
    }
};

template <>
struct fmt::formatter<text::font_key>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_key key, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", key.value);
    }
};

template <>
struct fmt::formatter<text::glyph_index>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::glyph_index value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", value.value);
    }
};

template <>
struct fmt::formatter<text::glyph_key>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::glyph_key const& key, FormatContext& ctx)
    {
#if defined(GLYPH_KEY_DEBUG)
        return fmt::format_to(
            ctx.out(),
            "({}, {}:{}, \"{}\")",
            key.size,
            key.font,
            key.index,
            unicode::convert_to<char>(std::u32string_view(key.text.data(), key.text.size())));
#else
        return fmt::format_to(ctx.out(), "({}, {}, {})", key.font, key.size, key.index);
#endif
    }
};

template <>
struct fmt::formatter<text::font_feature>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::font_feature value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(),
                              "{}{}{}{}{}",
                              value.enabled ? '+' : '-',
                              value.name[0],
                              value.name[1],
                              value.name[2],
                              value.name[3]);
    }
};

template <>
struct fmt::formatter<text::render_mode>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::render_mode value, FormatContext& ctx)
    {
        string_view name;
        switch (value)
        {
            case text::render_mode::bitmap: name = "Bitmap"; break;
            case text::render_mode::gray: name = "Gray"; break;
            case text::render_mode::light: name = "Light"; break;
            case text::render_mode::lcd: name = "LCD"; break;
            case text::render_mode::color: name = "Color"; break;
        }
        return fmt::formatter<string_view> {}.format(name, ctx);
    }
};
// }}}
