// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/Image.h>

#include <crispy/StrongHash.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <variant>

namespace vtbackend
{

enum class ColorPreference : uint8_t
{
    Dark,
    Light,
};

struct ImageData
{
    vtbackend::ImageFormat format;
    int rowAlignment = 1;
    ImageSize size;
    std::vector<uint8_t> pixels;

    crispy::strong_hash hash;

    void updateHash() noexcept;
};

using ImageDataPtr = std::shared_ptr<ImageData const>;

struct BackgroundImage
{
    using Location = std::variant<std::filesystem::path, ImageDataPtr>;

    Location location;
    crispy::strong_hash hash {};

    // image configuration
    float opacity = 0.5; // normalized value
    bool blur = false;
};

/// The color look-up table (color mode) selected by DECSTGLT (Select Color Look-Up Table). It governs
/// how a cell's foreground/background is chosen from its SGR attributes.
///
/// The manual's monochrome table (DECSTGLT 0) is deliberately absent: a lone zero parameter collapses to
/// "no parameter" under the VT parameter convention, making `CSI 0 ) {` indistinguishable from `CSI ) {`
/// and the table therefore unselectable. Contour has no gray-level map to select either.
enum class ColorLookupTable : uint8_t
{
    Alternate, //!< DECSTGLT 1/2: color chosen by text-attribute combination (DECATC); SGR colors ignored.
    AnsiSgr,   //!< DECSTGLT 3 (power-up default): SGR color parameters drive text color.
};

/// The five colors an application may assign to text carrying a particular attribute.
///
/// xterm calls these the "special" colors. They are addressed by `OSC 5 ; n ; spec`, and equivalently by
/// `OSC 4` at an index just past the last indexed color, and reset by `OSC 105`.
enum class SpecialColor : uint8_t
{
    Bold = 0,      ///< xterm's COLOR_BD
    Underline = 1, ///< COLOR_UL
    Blink = 2,     ///< COLOR_BL
    Reverse = 3,   ///< COLOR_RV
    Italic = 4,    ///< COLOR_IT
};

/// How many special colors there are. @see SpecialColor.
constexpr auto SpecialColorCount = size_t { 5 };

/// The number of indexed colors -- the ones `OSC 4` addresses directly, and the count XTGETTCAP's `Co`
/// reports. An application computes the index of a special color by adding to this.
constexpr auto IndexedColorCount = size_t { 256 };

/// Where the palette keeps Contour's own dim colors. No escape sequence reaches these.
constexpr auto DimColorOffset = IndexedColorCount;

/// Where the palette keeps the special colors.
///
/// *Not* right after the indexed ones, where xterm puts them: Contour's dim colors already sit there.
/// The two layouts cannot share an offset, so the translation from the index an application names to the
/// slot the color lives in is stated once -- in paletteSlotOfColorIndex() -- rather than being baked
/// into the array's shape and silently colliding.
constexpr auto SpecialColorOffset = IndexedColorCount + 8;

/// Maps the color index an application names in `OSC 4` to the palette slot that color lives in.
///
/// @param index The color index, 0..255 for an indexed color and 256..260 for a special one.
/// @return The palette slot, or std::nullopt if @p index names no color at all.
constexpr std::optional<size_t> paletteSlotOfColorIndex(unsigned index) noexcept
{
    if (index < IndexedColorCount)
        return index;

    if (index < IndexedColorCount + SpecialColorCount)
        return SpecialColorOffset + (index - IndexedColorCount);

    return std::nullopt;
}

/// Maps the special-color number an application names in `OSC 5` to the palette slot it lives in.
///
/// @param index The special color number, 0..4. @see SpecialColor.
/// @return The palette slot, or std::nullopt if @p index names no special color.
constexpr std::optional<size_t> paletteSlotOfSpecialColor(unsigned index) noexcept
{
    if (index < SpecialColorCount)
        return SpecialColorOffset + index;

    return std::nullopt;
}

/// Builds the built-in default colour palette: the 16 ANSI colours, the 6x6x6 colour cube, the
/// grayscale ramp, and the 8 dim colours.
///
/// Evaluated at compile time so ColorPalette::DefaultColorPalette needs no dynamic initialization.
constexpr std::array<RGBColor, SpecialColorOffset + SpecialColorCount> makeDefaultColorPalette() noexcept
{
    std::array<RGBColor, SpecialColorOffset + SpecialColorCount> colors {};

    // normal colors
    colors[0] = 0x000000_rgb; // black
    colors[1] = 0xc63939_rgb; // red
    colors[2] = 0x00a000_rgb; // green
    colors[3] = 0xa0a000_rgb; // yellow
    colors[4] = 0x4d79ff_rgb; // blue
    colors[5] = 0xff66ff_rgb; // magenta
    colors[6] = 0x00a0a0_rgb; // cyan
    colors[7] = 0xc0c0c0_rgb; // white

    // bright colors
    colors[8] = 0x707070_rgb;  // bright black (dark gray)
    colors[9] = 0xff0000_rgb;  // bright red
    colors[10] = 0x00ff00_rgb; // bright green
    colors[11] = 0xffff00_rgb; // bright yellow
    colors[12] = 0x0000ff_rgb; // bright blue
    colors[13] = 0xff00ff_rgb; // bright magenta
    colors[14] = 0x00ffff_rgb; // bright blue
    colors[15] = 0xffffff_rgb; // bright white

    // colors 16-231 are a 6x6x6 color cube
    for (unsigned red = 0; red < 6; ++red)
        for (unsigned green = 0; green < 6; ++green)
            for (unsigned blue = 0; blue < 6; ++blue)
                colors[16 + (red * 36) + (green * 6) + blue] =
                    RGBColor { static_cast<uint8_t>(red ? ((red * 40) + 55) : 0),
                               static_cast<uint8_t>(green ? ((green * 40) + 55) : 0),
                               static_cast<uint8_t>(blue ? ((blue * 40) + 55) : 0) };

    // colors 232-255 are a grayscale ramp, intentionally leaving out black and white
    for (uint8_t gray = 0, level = uint8_t((gray * 10) + 8); gray < 24;
         ++gray, level = uint8_t((gray * 10) + 8))
        colors[size_t(232 + gray)] = RGBColor { level, level, level };

    // dim colors
    colors[256 + 0] = 0x000000_rgb; // black
    colors[256 + 1] = 0xa00000_rgb; // red
    colors[256 + 2] = 0x008000_rgb; // green
    colors[256 + 3] = 0x808000_rgb; // yellow
    colors[256 + 4] = 0x000080_rgb; // blue
    colors[256 + 5] = 0x800080_rgb; // magenta
    colors[256 + 6] = 0x008080_rgb; // cyan
    colors[256 + 7] = 0x808080_rgb; // white

    return colors;
}

struct ColorPalette
{
    using Palette = std::array<RGBColor, SpecialColorOffset + SpecialColorCount>;

    /// Indicates whether or not bright colors are being allowed
    /// for indexed colors between 0..7 and mode set to ColorMode::Bright.
    ///
    /// This value is used by draw_bold_text_with_bright_colors in profile configuration.
    ///
    /// If disabled, normal color will be used instead.
    ///
    /// TODO: This should be part of Config's Profile instead of being here. That sounds just wrong.
    /// TODO: And even the naming sounds wrong. Better would be makeIndexedColorsBrightForBoldText or similar.
    bool useBrightColors = false;

    static constexpr Palette DefaultColorPalette = makeDefaultColorPalette();

    Palette palette = DefaultColorPalette;

    [[nodiscard]] RGBColor normalColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette.at(index);
    }

    [[nodiscard]] RGBColor brightColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette.at(index + 8);
    }

    [[nodiscard]] RGBColor dimColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette[DimColorOffset + index];
    }

    /// @return The color assigned to text carrying the attribute @p which. @see SpecialColor.
    [[nodiscard]] RGBColor specialColor(SpecialColor which) const noexcept
    {
        return palette[SpecialColorOffset + static_cast<size_t>(which)];
    }

    [[nodiscard]] RGBColor indexedColor(size_t index) const noexcept
    {
        assert(index < 256);
        return palette.at(index);
    }

    RGBColor defaultForeground = 0xD0D0D0_rgb;
    RGBColor defaultBackground = 0x1a1716_rgb;
    RGBColor defaultForegroundBright = 0xFFFFFF_rgb;
    RGBColor defaultForegroundDimmed = 0x808080_rgb;

    CursorColor cursor;

    RGBColor mouseForeground = 0x800000_rgb;
    RGBColor mouseBackground = 0x808000_rgb;

    struct
    {
        RGBColor normal = 0xF0F000_rgb;
        RGBColor hover = 0xFF0000_rgb;
    } hyperlinkDecoration;

    RGBColorPair inputMethodEditor = { .foreground = 0xFFFFFF_rgb, .background = 0xFF0000_rgb };

    /// Number of text-attribute combinations addressable by DECATC (Alternate Text Color): the four
    /// relevant attributes (bold, reverse, underline, blink) are either present or absent, so there are
    /// 2^4 = 16 combinations, which the manual enumerates as Ps1 values 0..15.
    static constexpr size_t AlternateTextColorCount = 16;

    /// Per-attribute-combination color overrides assigned via DECATC. Indexed by the DECATC Ps1
    /// attribute-combination index (see alternateTextColorIndexFromMask): a set entry replaces the
    /// SGR-derived foreground/background for a matching cell, but only while colorLookupTable is
    /// Alternate. Empty = no override.
    std::array<std::optional<RGBColorPair>, AlternateTextColorCount> alternateTextColors {};

    /// Assigns a DECATC color override for attribute combination @p index.
    /// @param index The DECATC Ps1 index, in [0, AlternateTextColorCount).
    /// @param colors The foreground/background to use for that combination.
    void setAlternateTextColor(size_t index, RGBColorPair colors) noexcept
    {
        alternateTextColors.at(index) = colors;
    }

    /// Clears the DECATC color override for attribute combination @p index.
    /// @param index The DECATC Ps1 index, in [0, AlternateTextColorCount).
    void resetAlternateTextColor(size_t index) noexcept { alternateTextColors.at(index).reset(); }

    /// The active color look-up table selected by DECSTGLT. In AnsiSgr mode (the power-up default) SGR
    /// color parameters drive text color; in Alternate mode the DECATC attribute-combination colors do
    /// instead and SGR colors are ignored. Living on the palette means it (like alternateTextColors)
    /// rides XTPUSHCOLORS/XTPOPCOLORS save/restore for free and resets to the default on RIS — and, as a
    /// side effect of the whole-palette reset, also on DECSTR (a knowingly-accepted minor deviation
    /// from the spec's DECSTR table; DECATC and DECSTGLT reset together as one unit).
    /// @see ColorLookupTable, alternateTextColors.
    ColorLookupTable colorLookupTable = ColorLookupTable::AnsiSgr;

    std::shared_ptr<BackgroundImage> backgroundImage;

    // clang-format off
    CellRGBColorAndAlphaPair yankHighlight { .foreground=CellForegroundColor {}, .foregroundAlpha=1.0f, .background=0xffA500_rgb, .backgroundAlpha=0.5f };

    CellRGBColorAndAlphaPair searchHighlight { .foreground=CellBackgroundColor {}, .foregroundAlpha=1.0f, .background=CellForegroundColor {}, .backgroundAlpha=1.0f };
    CellRGBColorAndAlphaPair searchHighlightFocused {  .foreground=CellBackgroundColor {}, .foregroundAlpha=1.0f,.background=CellForegroundColor {}, .backgroundAlpha=1.0f };

    CellRGBColorAndAlphaPair wordHighlight { .foreground=CellForegroundColor {}, .foregroundAlpha=1.0f, .background=0x909090_rgb, .backgroundAlpha=0.5f };
    CellRGBColorAndAlphaPair wordHighlightCurrent { .foreground=CellForegroundColor {}, .foregroundAlpha=1.0f, .background=RGBColor{0x90, 0x90, 0x90}, .backgroundAlpha=0.6f };

    CellRGBColorAndAlphaPair selection { .foreground=CellForegroundColor {}, .foregroundAlpha=1.0f, .background=0x4040f0_rgb , .backgroundAlpha=0.5f };

    CellRGBColorAndAlphaPair normalModeCursorline = { .foreground=0xFFFFFF_rgb, .foregroundAlpha=0.2f, .background=0x808080_rgb, .backgroundAlpha=0.4f };

    CellRGBColorAndAlphaPair hintLabel = { .foreground=0x1a1716_rgb, .foregroundAlpha=1.0f, .background=0xFFCC00_rgb, .backgroundAlpha=1.0f };
    CellRGBColorAndAlphaPair hintMatch = { .foreground=CellForegroundColor {}, .foregroundAlpha=1.0f, .background=0x4488CC_rgb, .backgroundAlpha=0.35f };
    // clang-format on

    RGBColorPair indicatorStatusLineInactive = { .foreground = 0xFFFFFF_rgb, .background = 0x0270c0_rgb };
    RGBColorPair indicatorStatusLineInsertMode = { .foreground = 0xFFFFFF_rgb, .background = 0x0270c0_rgb };
    RGBColorPair indicatorStatusLineNormalMode = { .foreground = 0xFFFFFF_rgb, .background = 0x0270c0_rgb };
    RGBColorPair indicatorStatusLineVisualMode = { .foreground = 0xFFFFFF_rgb, .background = 0x0270c0_rgb };
};

bool defaultColorPalettes(std::string const& colorPaletteName, ColorPalette& palette) noexcept;

enum class ColorTarget : uint8_t
{
    Foreground,
    Background,
};

enum class ColorMode : uint8_t
{
    Dimmed,
    Normal,
    Bright
};

/// Bit values for an internal bit-set describing which of the four DECATC-relevant attributes are
/// active on a cell. NOTE: this is NOT the DECATC wire selector Ps1 — the manual's Ps1 is an
/// *enumerated* combination index (0=Normal, 1=Bold, 2=Reverse, 3=Underline, 4=Blink, 5=Bold reverse,
/// ...), not a bitwise OR. The load-bearing convention is that ColorPalette::alternateTextColors is
/// indexed by Ps1: the DECATC handler stores at the raw Ps1 wire value, while the cell color resolver
/// derives Ps1 from a cell's flags via alternateTextColorIndexFromMask (which owns the DEC ordering).
/// @see alternateTextColorIndexFromMask, ColorPalette::alternateTextColors.
enum class AlternateTextColorMask : uint8_t
{
    Bold = 1,
    Reverse = 2,
    Underline = 4,
    Blink = 8,
};

namespace detail
{
    // DECATC Ps1 lookup, indexed directly by the active-attribute bit-set (OR of AlternateTextColorMask
    // values, 0..15) → the Ps1 combination index. The VT525 manual (§5-22) enumerates combinations by
    // attribute count then Bold>Reverse>Underline>Blink priority, so this is a table, not a bitmask. Bit
    // values: Bold=1, Reverse=2, Underline=4, Blink=8. Written out literally (no compile-time inversion)
    // for maximum portability; the static_assert below verifies it against the manual's ordering.
    // Bit-set value (index) = Bold*1 + Reverse*2 + Underline*4 + Blink*8; the entry is the Ps1 index.
    inline constexpr std::array<uint8_t, 16> AlternateTextColorIndexTable {
        0,  //  0  -> Ps1 0  Normal text
        1,  //  1  -> Ps1 1  Bold
        2,  //  2  -> Ps1 2  Reverse
        5,  //  3  -> Ps1 5  Bold reverse
        3,  //  4  -> Ps1 3  Underline
        6,  //  5  -> Ps1 6  Bold underline
        8,  //  6  -> Ps1 8  Reverse underline
        11, //  7  -> Ps1 11 Bold reverse underline
        4,  //  8  -> Ps1 4  Blink
        7,  //  9  -> Ps1 7  Bold blink
        9,  //  10 -> Ps1 9  Reverse blink
        12, //  11 -> Ps1 12 Bold reverse blink
        10, //  12 -> Ps1 10 Underline blink
        13, //  13 -> Ps1 13 Bold underline blink
        14, //  14 -> Ps1 14 Reverse underline blink
        15, //  15 -> Ps1 15 Bold reverse underline blink
    };

    // Guard: the table must be the exact inverse of the manual's forward ordering (Ps1 index i holds
    // this attribute bit-set), so a typo above is a compile error rather than a wrong color.
    static_assert([] {
        constexpr std::array<uint8_t, 16> Forward { 0, 1, 2, 4, 8, 3, 5, 9, 6, 10, 12, 7, 11, 13, 14, 15 };
        for (size_t ps1 = 0; ps1 < Forward.size(); ++ps1)
            if (AlternateTextColorIndexTable[Forward[ps1]] != ps1)
                return false;
        return true;
    }());
} // namespace detail

/// Maps a bit-set of active attributes (OR of AlternateTextColorMask values, 0..15) to the DECATC Ps1
/// attribute-combination index as enumerated in the VT525 manual (§5-22).
/// @param bitset The active-attribute bit-set. @return The Ps1 index, in [0, 16).
[[nodiscard]] constexpr uint8_t alternateTextColorIndexFromMask(uint8_t bitset) noexcept
{
    return detail::AlternateTextColorIndexTable[bitset & 0x0F];
}

RGBColor apply(ColorPalette const& colorPalette, Color color, ColorTarget target, ColorMode mode) noexcept;

} // namespace vtbackend

// {{{ fmtlib custom formatter support
template <>
struct std::formatter<vtbackend::ColorPreference>: std::formatter<std::string_view>
{
    auto format(vtbackend::ColorPreference value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ColorPreference::Dark: name = "Dark"; break;
            case vtbackend::ColorPreference::Light: name = "Light"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ColorMode>: std::formatter<std::string_view>
{
    auto format(vtbackend::ColorMode value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ColorMode::Normal: name = "Normal"; break;
            case vtbackend::ColorMode::Dimmed: name = "Dimmed"; break;
            case vtbackend::ColorMode::Bright: name = "Bright"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ColorTarget>: std::formatter<std::string_view>
{
    auto format(vtbackend::ColorTarget value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ColorTarget::Foreground: name = "Foreground"; break;
            case vtbackend::ColorTarget::Background: name = "Background"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
