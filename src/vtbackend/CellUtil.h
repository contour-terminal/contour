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

#include <unicode/width.h>

namespace terminal::CellUtil
{

[[nodiscard]] inline RGBColorPair makeColors(ColorPalette const& colorPalette,
                                             CellFlags cellFlags,
                                             bool _reverseVideo,
                                             Color foregroundColor,
                                             Color backgroundColor,
                                             bool blinkingState_,
                                             bool rapidBlinkState_) noexcept
{
    auto const fgMode = (cellFlags & CellFlags::Faint)                                    ? ColorMode::Dimmed
                        : ((cellFlags & CellFlags::Bold) && colorPalette.useBrightColors) ? ColorMode::Bright
                                                                                          : ColorMode::Normal;

    auto constexpr bgMode = ColorMode::Normal;

    auto const [fgColorTarget, bgColorTarget] =
        _reverseVideo ? std::pair { ColorTarget::Background, ColorTarget::Foreground }
                      : std::pair { ColorTarget::Foreground, ColorTarget::Background };

    auto rgbColors = RGBColorPair { apply(colorPalette, foregroundColor, fgColorTarget, fgMode),
                                    apply(colorPalette, backgroundColor, bgColorTarget, bgMode) };

    if (cellFlags & CellFlags::Inverse)
        rgbColors = rgbColors.swapped();

    if (cellFlags & CellFlags::Hidden)
        rgbColors = rgbColors.allBackground();

    if ((cellFlags & CellFlags::Blinking) && !blinkingState_)
        return rgbColors.allBackground();
    if ((cellFlags & CellFlags::RapidBlinking) && !rapidBlinkState_)
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

    auto const mode = (cellFlags & CellFlags::Faint)                                    ? ColorMode::Dimmed
                      : ((cellFlags & CellFlags::Bold) && colorPalette.useBrightColors) ? ColorMode::Bright
                                                                                        : ColorMode::Normal;

    return apply(colorPalette, underlineColor, ColorTarget::Foreground, mode);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
[[nodiscard]] inline RGBColor
    makeUnderlineColor(ColorPalette const& colorPalette, RGBColor defaultColor, Cell const& cell) noexcept
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
    assert(text.size() != 0);

    if (cell.codepointCount() == 0)
        return false;

    if (text.size() < cell.codepointCount())
        return false;

    for (size_t i = 0; i < cell.codepointCount(); ++i)
        if (cell.codepoint(i) != text[i])
            return false;

    return true;
}

[[nodiscard]] inline CellFlags makeCellFlags(GraphicsRendition rendition, CellFlags base) noexcept
{
    CellFlags flags = base;
    switch (rendition)
    {
        case GraphicsRendition::Reset: flags = CellFlags::None; break;
        case GraphicsRendition::Bold: flags |= CellFlags::Bold; break;
        case GraphicsRendition::Faint: flags |= CellFlags::Faint; break;
        case GraphicsRendition::Italic: flags |= CellFlags::Italic; break;
        case GraphicsRendition::Underline: flags |= CellFlags::Underline; break;
        case GraphicsRendition::Blinking:
            flags &= ~CellFlags::RapidBlinking;
            flags |= CellFlags::Blinking;
            break;
        case GraphicsRendition::RapidBlinking:
            flags &= ~CellFlags::Blinking;
            flags |= CellFlags::RapidBlinking;
            break;
        case GraphicsRendition::Inverse: flags |= CellFlags::Inverse; break;
        case GraphicsRendition::Hidden: flags |= CellFlags::Hidden; break;
        case GraphicsRendition::CrossedOut: flags |= CellFlags::CrossedOut; break;
        case GraphicsRendition::DoublyUnderlined: flags |= CellFlags::DoublyUnderlined; break;
        case GraphicsRendition::CurlyUnderlined: flags |= CellFlags::CurlyUnderlined; break;
        case GraphicsRendition::DottedUnderline: flags |= CellFlags::DottedUnderline; break;
        case GraphicsRendition::DashedUnderline: flags |= CellFlags::DashedUnderline; break;
        case GraphicsRendition::Framed: flags |= CellFlags::Framed; break;
        case GraphicsRendition::Overline: flags |= CellFlags::Overline; break;
        case GraphicsRendition::Normal: flags &= ~(CellFlags::Bold | CellFlags::Faint); break;
        case GraphicsRendition::NoItalic: flags &= ~CellFlags::Italic; break;
        case GraphicsRendition::NoUnderline:
            flags &= ~(CellFlags::Underline | CellFlags::DoublyUnderlined | CellFlags::CurlyUnderlined
                       | CellFlags::DottedUnderline | CellFlags::DashedUnderline);
            break;
        case GraphicsRendition::NoBlinking: flags &= ~(CellFlags::Blinking | CellFlags::RapidBlinking); break;
        case GraphicsRendition::NoInverse: flags &= ~CellFlags::Inverse; break;
        case GraphicsRendition::NoHidden: flags &= ~CellFlags::Hidden; break;
        case GraphicsRendition::NoCrossedOut: flags &= ~CellFlags::CrossedOut; break;
        case GraphicsRendition::NoFramed: flags &= ~CellFlags::Framed; break;
        case GraphicsRendition::NoOverline: flags &= ~CellFlags::Overline; break;
    }
    return flags;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline void applyGraphicsRendition(GraphicsRendition sgr, Cell& cell) noexcept
{
    cell.resetFlags(makeCellFlags(sgr, cell.flags()));
}

} // namespace terminal::CellUtil
