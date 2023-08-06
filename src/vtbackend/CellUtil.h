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

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/cell/CellConcept.h>

#include <libunicode/width.h>

namespace terminal::CellUtil
{

[[nodiscard]] inline rgb_color_pair makeColors(ColorPalette const& colorPalette,
                                               cell_flags cellFlags,
                                               bool reverseVideo,
                                               color foregroundColor,
                                               color backgroundColor,
                                               bool blinkingState,
                                               bool rapidBlinkState) noexcept
{
    auto const fgMode = (cellFlags & cell_flags::Faint) ? ColorMode::Dimmed
                        : ((cellFlags & cell_flags::Bold) && colorPalette.useBrightColors)
                            ? ColorMode::Bright
                            : ColorMode::Normal;

    auto constexpr bgMode = ColorMode::Normal;

    auto const [fgColorTarget, bgColorTarget] =
        reverseVideo ? std::pair { ColorTarget::Background, ColorTarget::Foreground }
                     : std::pair { ColorTarget::Foreground, ColorTarget::Background };

    auto rgbColors = rgb_color_pair { apply(colorPalette, foregroundColor, fgColorTarget, fgMode),
                                      apply(colorPalette, backgroundColor, bgColorTarget, bgMode) };

    if (cellFlags & cell_flags::Inverse)
        rgbColors = rgbColors.swapped();

    if (cellFlags & cell_flags::Hidden)
        rgbColors = rgbColors.allBackground();

    if ((cellFlags & cell_flags::Blinking) && !blinkingState)
        return rgbColors.allBackground();
    if ((cellFlags & cell_flags::RapidBlinking) && !rapidBlinkState)
        return rgbColors.allBackground();

    return rgbColors;
}

[[nodiscard]] inline rgb_color makeUnderlineColor(ColorPalette const& colorPalette,
                                                  rgb_color defaultColor,
                                                  color underlineColor,
                                                  cell_flags cellFlags) noexcept
{
    if (isDefaultColor(underlineColor))
        return defaultColor;

    auto const mode = (cellFlags & cell_flags::Faint)                                    ? ColorMode::Dimmed
                      : ((cellFlags & cell_flags::Bold) && colorPalette.useBrightColors) ? ColorMode::Bright
                                                                                         : ColorMode::Normal;

    return apply(colorPalette, underlineColor, ColorTarget::Foreground, mode);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
[[nodiscard]] inline rgb_color
    makeUnderlineColor(ColorPalette const& colorPalette, rgb_color defaultColor, Cell const& cell) noexcept
{
    return makeUnderlineColor(colorPalette, defaultColor, cell.underlineColor(), cell.flags());
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
[[nodiscard]] inline bool compareText(Cell const& cell, char asciiCharacter) noexcept
{
    if (cell.codepointCount() != 1)
        return asciiCharacter == 0 && cell.codepointCount() == 0;

    return cell.codepoint(0) == static_cast<char32_t>(asciiCharacter);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
[[nodiscard]] inline bool empty(Cell const& cell) noexcept
{
    return (cell.codepointCount() == 0 || cell.codepoint(0) == 0x20) && !cell.imageFragment();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
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
            default: return unicode::width(codepoint);
        }
    }();

    return newWidth - cell.width();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
[[nodiscard]] inline bool beginsWith(std::u32string_view text, Cell const& cell) noexcept
{
    assert(!text.empty());

    if (cell.codepointCount() == 0)
        return false;

    if (text.size() < cell.codepointCount())
        return false;

    for (size_t i = 0; i < cell.codepointCount(); ++i)
        if (cell.codepoint(i) != text[i])
            return false;

    return true;
}

[[nodiscard]] inline cell_flags makeCellFlags(graphics_rendition rendition, cell_flags base) noexcept
{
    cell_flags flags = base;
    switch (rendition)
    {
        case graphics_rendition::Reset: flags = cell_flags::None; break;
        case graphics_rendition::Bold: flags |= cell_flags::Bold; break;
        case graphics_rendition::Faint: flags |= cell_flags::Faint; break;
        case graphics_rendition::Italic: flags |= cell_flags::Italic; break;
        case graphics_rendition::Underline: flags |= cell_flags::Underline; break;
        case graphics_rendition::Blinking:
            flags &= ~cell_flags::RapidBlinking;
            flags |= cell_flags::Blinking;
            break;
        case graphics_rendition::RapidBlinking:
            flags &= ~cell_flags::Blinking;
            flags |= cell_flags::RapidBlinking;
            break;
        case graphics_rendition::Inverse: flags |= cell_flags::Inverse; break;
        case graphics_rendition::Hidden: flags |= cell_flags::Hidden; break;
        case graphics_rendition::CrossedOut: flags |= cell_flags::CrossedOut; break;
        case graphics_rendition::DoublyUnderlined: flags |= cell_flags::DoublyUnderlined; break;
        case graphics_rendition::CurlyUnderlined: flags |= cell_flags::CurlyUnderlined; break;
        case graphics_rendition::DottedUnderline: flags |= cell_flags::DottedUnderline; break;
        case graphics_rendition::DashedUnderline: flags |= cell_flags::DashedUnderline; break;
        case graphics_rendition::Framed: flags |= cell_flags::Framed; break;
        case graphics_rendition::Overline: flags |= cell_flags::Overline; break;
        case graphics_rendition::Normal: flags &= ~(cell_flags::Bold | cell_flags::Faint); break;
        case graphics_rendition::NoItalic: flags &= ~cell_flags::Italic; break;
        case graphics_rendition::NoUnderline:
            flags &= ~(cell_flags::Underline | cell_flags::DoublyUnderlined | cell_flags::CurlyUnderlined
                       | cell_flags::DottedUnderline | cell_flags::DashedUnderline);
            break;
        case graphics_rendition::NoBlinking:
            flags &= ~(cell_flags::Blinking | cell_flags::RapidBlinking);
            break;
        case graphics_rendition::NoInverse: flags &= ~cell_flags::Inverse; break;
        case graphics_rendition::NoHidden: flags &= ~cell_flags::Hidden; break;
        case graphics_rendition::NoCrossedOut: flags &= ~cell_flags::CrossedOut; break;
        case graphics_rendition::NoFramed: flags &= ~cell_flags::Framed; break;
        case graphics_rendition::NoOverline: flags &= ~cell_flags::Overline; break;
    }
    return flags;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline void applyGraphicsRendition(graphics_rendition sgr, Cell& cell) noexcept
{
    cell.resetFlags(makeCellFlags(sgr, cell.flags()));
}

} // namespace terminal::CellUtil
