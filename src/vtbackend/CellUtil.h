// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>

#include <crispy/times.h>

#include <libunicode/width.h>

#include <ranges>
#include <utility>

namespace vtbackend::CellUtil
{

/// Derives the DECATC attribute-combination index for a cell. This is the single definition of the
/// flags-to-index mapping: "underline" matches all underline variants, "blink" matches slow and
/// rapid, and "reverse" is the cell's *visual* reverse state — per-cell SGR 7 (CellFlag::Inverse) XOR
/// the screen-wide reverse-video mode (DECSCNM), so the override matches how the cell actually looks.
/// The active-attribute bit-set is mapped to the DEC-enumerated Ps1 index via
/// alternateTextColorIndexFromMask (the manual's table is not a plain bitmask).
/// @param cellFlags The cell's attribute flags.
/// @param reverseVideo Whether screen-wide reverse video (DECSCNM) is active.
/// @return An index into ColorPalette::alternateTextColors, in [0, AlternateTextColorCount).
[[nodiscard]] inline size_t cellFlagsToAlternateTextColorIndex(CellFlags cellFlags,
                                                               bool reverseVideo) noexcept
{
    auto constexpr AnyBlink = CellFlags { CellFlag::Blinking } | CellFlag::RapidBlinking;

    using enum AlternateTextColorMask;
    auto bitset = uint8_t { 0 };
    if (cellFlags.contains(CellFlag::Bold))
        bitset |= std::to_underlying(Bold);
    if (cellFlags.contains(CellFlag::Inverse) != reverseVideo) // net visual reverse (SGR 7 XOR DECSCNM)
        bitset |= std::to_underlying(Reverse);
    if (cellFlags.any(UnderlineMask))
        bitset |= std::to_underlying(Underline);
    if (cellFlags.any(AnyBlink))
        bitset |= std::to_underlying(Blink);
    return alternateTextColorIndexFromMask(bitset);
}

/// Resolves a cell's color from its text-attribute combination, as DECSTGLT "Alternate color" mode
/// prescribes. A combination the application never assigned via DECATC renders in the default
/// foreground/background — SGR color parameters play no part in this mode at all. (xterm models the same
/// rule by seeding all sixteen entries from the default pair on reset.)
/// @param colorPalette The palette holding the DECATC assignments and the defaults to fall back to.
/// @param cellFlags The cell's attribute flags.
/// @param reverseVideo Whether screen-wide reverse video (DECSCNM) is active.
/// @return The foreground/background to paint the cell with.
[[nodiscard]] inline RGBColorPair alternateTextColors(ColorPalette const& colorPalette,
                                                      CellFlags cellFlags,
                                                      bool reverseVideo) noexcept
{
    auto const index = cellFlagsToAlternateTextColorIndex(cellFlags, reverseVideo);
    return colorPalette.alternateTextColors[index].value_or(RGBColorPair {
        .foreground = colorPalette.defaultForeground, .background = colorPalette.defaultBackground });
}

/// Resolves the foreground/background a cell is painted with, from its SGR attributes or — in DECSTGLT
/// Alternate mode — from its attribute combination.
/// @param colorPalette The active color palette.
/// @param colorLookupTable The color mode that applies to the screen being rendered. Callers rendering
///                         host-owned chrome (the status lines) pass AnsiSgr, because an application's
///                         DECATC assignments must not repaint the terminal's own furniture.
/// @param cellFlags The cell's attribute flags.
/// @param reverseVideo Whether screen-wide reverse video (DECSCNM) is active.
/// @param foregroundColor The cell's SGR foreground color.
/// @param backgroundColor The cell's SGR background color.
/// @param blinkingState Blink animation phase, in [0, 1].
/// @param rapidBlinkState Rapid-blink animation phase, in [0, 1].
/// @return The foreground/background to paint the cell with.
[[nodiscard]] inline RGBColorPair makeColors(ColorPalette const& colorPalette,
                                             ColorLookupTable colorLookupTable,
                                             CellFlags cellFlags,
                                             bool reverseVideo,
                                             Color foregroundColor,
                                             Color backgroundColor,
                                             float blinkingState,
                                             float rapidBlinkState) noexcept
{
    auto rgbColors = [&]() -> RGBColorPair {
        // DECATC (Alternate Text Color): in DECSTGLT "Alternate color" mode a cell's color comes from its
        // text-attribute combination instead of its SGR color parameters — the VT240-compat model for
        // terminals without ANSI color. The reverse attribute is part of that combination index, so this
        // supersedes the reverse-video handling below rather than composing with it. In the default
        // AnsiSgr mode the whole feature is inert, so the overwhelmingly common path pays a single
        // predictable branch and never touches the otherwise-cold override array.
        if (colorLookupTable == ColorLookupTable::Alternate)
            return alternateTextColors(colorPalette, cellFlags, reverseVideo);

        auto const fgMode = [](CellFlags flags, ColorPalette const& colorPalette) {
            if (flags & CellFlag::Faint)
                return ColorMode::Dimmed;
            if ((flags & CellFlag::Bold) && colorPalette.useBrightColors)
                return ColorMode::Bright;
            return ColorMode::Normal;
        }(cellFlags, colorPalette);

        auto constexpr BgMode = ColorMode::Normal;

        auto const [fgColorTarget, bgColorTarget] =
            reverseVideo ? std::pair { ColorTarget::Background, ColorTarget::Foreground }
                         : std::pair { ColorTarget::Foreground, ColorTarget::Background };

        auto sgrColors =
            RGBColorPair { .foreground = apply(colorPalette, foregroundColor, fgColorTarget, fgMode),
                           .background = apply(colorPalette, backgroundColor, bgColorTarget, BgMode) };

        if (cellFlags & CellFlag::Inverse)
            sgrColors = sgrColors.swapped();

        return sgrColors;
    }();

    if (cellFlags & CellFlag::Hidden)
        rgbColors = rgbColors.allBackground();

    if (cellFlags & CellFlag::Blinking)
        return mixColor(rgbColors.allBackground(), rgbColors, blinkingState);
    if (cellFlags & CellFlag::RapidBlinking)
        return mixColor(rgbColors.allBackground(), rgbColors, rapidBlinkState);

    return rgbColors;
}

[[nodiscard]] inline RGBColor makeUnderlineColor(ColorPalette const& colorPalette,
                                                 RGBColor defaultColor,
                                                 Color underlineColor,
                                                 CellFlags cellFlags) noexcept
{
    if (isDefaultColor(underlineColor))
        return defaultColor;

    auto const mode = [](CellFlags flags, ColorPalette const& colorPalette) {
        if (flags & CellFlag::Faint)
            return ColorMode::Dimmed;
        if ((flags & CellFlag::Bold) && colorPalette.useBrightColors)
            return ColorMode::Bright;
        return ColorMode::Normal;
    }(cellFlags, colorPalette);

    return apply(colorPalette, underlineColor, ColorTarget::Foreground, mode);
}

template <typename Cell>
[[nodiscard]] inline RGBColor makeUnderlineColor(ColorPalette const& colorPalette,
                                                 RGBColor defaultColor,
                                                 Cell const& cell) noexcept
{
    return makeUnderlineColor(colorPalette, defaultColor, cell.underlineColor(), cell.flags());
}

template <typename Cell>
[[nodiscard]] inline bool compareText(Cell const& cell, char32_t character) noexcept
{
    if (cell.codepointCount() != 1)
        return character == 0 && cell.codepointCount() == 0;

    return cell.codepoint(0) == character;
}

template <typename Cell>
[[nodiscard]] inline bool empty(Cell const& cell) noexcept
{
    return (cell.codepointCount() == 0) && !cell.imageFragment();
}

template <typename Cell>
[[nodiscard]] inline int computeWidthChange(Cell const& cell, char32_t codepoint) noexcept
{
    constexpr bool AllowWidthChange = false; // TODO: make configurable
    if (!AllowWidthChange)
        return 0;

    auto const newWidth = [codepoint]() {
        switch (codepoint)
        {
            case 0xFE0E: return 1;
            case 0xFE0F: return 2;
            default: return static_cast<int>(unicode::width(codepoint));
        }
    }();

    return newWidth - cell.width();
}

template <typename Cell>
[[nodiscard]] inline bool beginsWith(std::u32string_view text,
                                     Cell const& cell,
                                     bool isCaseSensitive) noexcept
{
    assert(!text.empty());

    auto const cellCodepointCount = cell.size();

    if (cellCodepointCount == 0)
        return false;

    if (text.size() < cellCodepointCount)
        return false;

    auto const testMatchAt = [&](size_t i) {
        if (isCaseSensitive)
            return cell[i] == text[i];
        return static_cast<char32_t>(std::tolower(cell[i])) == text[i];
    };

    // TODO: Should use this line instead - but that breaks on Ubuntu 22.04 with Clang 15
    // return std::ranges::all_of(crispy::times(cellCodepointCount), testMatchAt);
    for (auto const i: crispy::times(cellCodepointCount))
        if (!testMatchAt(i))
            return false;
    return true;
}

[[nodiscard]] inline CellFlags makeCellFlags(GraphicsRendition rendition, CellFlags base) noexcept
{
    CellFlags flags = base;
    switch (rendition)
    {
        case GraphicsRendition::Reset: flags = CellFlag::None; break;
        case GraphicsRendition::Bold: flags |= CellFlag::Bold; break;
        case GraphicsRendition::Faint: flags |= CellFlag::Faint; break;
        case GraphicsRendition::Italic: flags |= CellFlag::Italic; break;
        case GraphicsRendition::Underline:
            flags = flags.without({ CellFlag::DoublyUnderlined,
                                    CellFlag::CurlyUnderlined,
                                    CellFlag::DottedUnderline,
                                    CellFlag::DashedUnderline });
            flags |= CellFlag::Underline;
            break;
        case GraphicsRendition::Blinking:
            flags.disable(CellFlag::RapidBlinking);
            flags.enable(CellFlag::Blinking);
            break;
        case GraphicsRendition::RapidBlinking:
            flags.disable(CellFlag::Blinking);
            flags.enable(CellFlag::RapidBlinking);
            break;
        case GraphicsRendition::Inverse: flags |= CellFlag::Inverse; break;
        case GraphicsRendition::Hidden: flags |= CellFlag::Hidden; break;
        case GraphicsRendition::CrossedOut: flags |= CellFlag::CrossedOut; break;
        case GraphicsRendition::DoublyUnderlined:
            flags = flags.without({ CellFlag::Underline,
                                    CellFlag::CurlyUnderlined,
                                    CellFlag::DottedUnderline,
                                    CellFlag::DashedUnderline });
            flags |= CellFlag::DoublyUnderlined;
            break;
        case GraphicsRendition::CurlyUnderlined:
            flags = flags.without({ CellFlag::Underline,
                                    CellFlag::DoublyUnderlined,
                                    CellFlag::DottedUnderline,
                                    CellFlag::DashedUnderline });
            flags |= CellFlag::CurlyUnderlined;
            break;
        case GraphicsRendition::DottedUnderline:
            flags = flags.without({ CellFlag::Underline,
                                    CellFlag::DoublyUnderlined,
                                    CellFlag::CurlyUnderlined,
                                    CellFlag::DashedUnderline });
            flags |= CellFlag::DottedUnderline;
            break;
        case GraphicsRendition::DashedUnderline:
            flags = flags.without({ CellFlag::Underline,
                                    CellFlag::DoublyUnderlined,
                                    CellFlag::CurlyUnderlined,
                                    CellFlag::DottedUnderline });
            flags |= CellFlag::DashedUnderline;
            break;
        case GraphicsRendition::Framed: flags |= CellFlag::Framed; break;
        case GraphicsRendition::Overline: flags |= CellFlag::Overline; break;
        case GraphicsRendition::Normal: flags = flags.without({ CellFlag::Bold, CellFlag::Faint }); break;
        case GraphicsRendition::NoItalic: flags.disable(CellFlag::Italic); break;
        case GraphicsRendition::NoUnderline:
            flags = flags.without({ CellFlag::Underline,
                                    CellFlag::DoublyUnderlined,
                                    CellFlag::CurlyUnderlined,
                                    CellFlag::DottedUnderline,
                                    CellFlag::DashedUnderline });
            break;
        case GraphicsRendition::NoBlinking:
            flags = flags.without({ CellFlag::Blinking, CellFlag::RapidBlinking });
            break;
        case GraphicsRendition::NoInverse: flags.disable(CellFlag::Inverse); break;
        case GraphicsRendition::NoHidden: flags.disable(CellFlag::Hidden); break;
        case GraphicsRendition::NoCrossedOut: flags.disable(CellFlag::CrossedOut); break;
        case GraphicsRendition::NoFramed: flags.disable(CellFlag::Framed); break;
        case GraphicsRendition::NoOverline: flags.disable(CellFlag::Overline); break;
    }
    return flags;
}

template <typename Cell>
inline void applyGraphicsRendition(GraphicsRendition sgr, Cell& cell) noexcept
{
    cell.resetFlags(makeCellFlags(sgr, cell.flags()));
}

} // namespace vtbackend::CellUtil
