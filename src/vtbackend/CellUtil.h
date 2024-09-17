// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/cell/CellConcept.h>

#include <crispy/times.h>

#include <libunicode/width.h>

#include <ranges>

namespace vtbackend::CellUtil
{

[[nodiscard]] inline RGBColorPair makeColors(ColorPalette const& colorPalette,
                                             CellFlags cellFlags,
                                             bool reverseVideo,
                                             Color foregroundColor,
                                             Color backgroundColor,
                                             bool blinkingState,
                                             bool rapidBlinkState) noexcept
{
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

    auto rgbColors = RGBColorPair { apply(colorPalette, foregroundColor, fgColorTarget, fgMode),
                                    apply(colorPalette, backgroundColor, bgColorTarget, BgMode) };

    if (cellFlags & CellFlag::Inverse)
        rgbColors = rgbColors.swapped();

    if (cellFlags & CellFlag::Hidden)
        rgbColors = rgbColors.allBackground();

    if ((cellFlags & CellFlag::Blinking) && !blinkingState)
        return rgbColors.allBackground();
    if ((cellFlags & CellFlag::RapidBlinking) && !rapidBlinkState)
        return rgbColors.allBackground();

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

template <CellConcept Cell>
[[nodiscard]] inline RGBColor makeUnderlineColor(ColorPalette const& colorPalette,
                                                 RGBColor defaultColor,
                                                 Cell const& cell) noexcept
{
    return makeUnderlineColor(colorPalette, defaultColor, cell.underlineColor(), cell.flags());
}

template <CellConcept Cell>
[[nodiscard]] inline bool compareText(Cell const& cell, char32_t character) noexcept
{
    if (cell.codepointCount() != 1)
        return character == 0 && cell.codepointCount() == 0;

    return cell.codepoint(0) == character;
}

template <CellConcept Cell>
[[nodiscard]] inline bool empty(Cell const& cell) noexcept
{
    return (cell.codepointCount() == 0) && !cell.imageFragment();
}

template <CellConcept Cell>
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
    requires(std::same_as<Cell, std::u32string_view> || CellConcept<Cell>)
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
        case GraphicsRendition::Underline: flags |= CellFlag::Underline; break;
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
        case GraphicsRendition::DoublyUnderlined: flags |= CellFlag::DoublyUnderlined; break;
        case GraphicsRendition::CurlyUnderlined: flags |= CellFlag::CurlyUnderlined; break;
        case GraphicsRendition::DottedUnderline: flags |= CellFlag::DottedUnderline; break;
        case GraphicsRendition::DashedUnderline: flags |= CellFlag::DashedUnderline; break;
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

template <CellConcept Cell>
inline void applyGraphicsRendition(GraphicsRendition sgr, Cell& cell) noexcept
{
    cell.resetFlags(makeCellFlags(sgr, cell.flags()));
}

} // namespace vtbackend::CellUtil
