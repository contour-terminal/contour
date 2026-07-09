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

struct ColorPalette
{
    using Palette = std::array<RGBColor, 256 + 8>;

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

    static Palette const DefaultColorPalette;

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
        return palette[256 + index];
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
