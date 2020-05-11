/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <crispy/text/UnicodeTables.h>
#include <crispy/text/Unicode.h>

namespace crispy::text {

#if 0
constexpr bool isEmojiPresentation(char32_t _codepoint) noexcept
{
    return contains(tables::emojiPresentation, _codepoint);
}

constexpr bool isEmojiExtendedPictographic(char32_t _codepoint) noexcept
{
    return contains(tables::emojiExtendedPictographic, _codepoint);
}

constexpr bool isEmojiModifierBase(char32_t _codepoint) noexcept
{
    return contains(tables::emojiModifierBase, _codepoint);
}

constexpr bool isEmojiModifier(char32_t _codepoint) noexcept
{
    return contains(tables::emojiModifier, _codepoint);
}

constexpr bool isEmojiKeycapBase(char32_t _codepoint) noexcept
{
    return contains(tables::emojiKeycapBase, _codepoint);
}

constexpr bool isEmojiEmojiDefault(char32_t _codepoint) noexcept
{
    return isEmojiPresentation(_codepoint);
}

constexpr bool isEmoji(char32_t _codepoint) noexcept
{
    return contains(tables::emoji, _codepoint);
}

constexpr bool isEmojiTextDefault(char32_t _codepoint) noexcept
{
    return isEmoji(_codepoint) && !isEmojiPresentation(_codepoint);
}
#endif

constexpr bool isRegionalIndicator(char32_t _codepoint) noexcept
{
    return 0x1F1E6 <= _codepoint && _codepoint <= 0x1F1FF;
}

constexpr bool isNewline(char32_t _codepoint) noexcept
{
    switch (_codepoint)
    {
        case 0x000B: // LINE TABULATION
        case 0x000C: // FORM FEED (FF)
        case 0x0085: // NEXT LINE (NEL)
        case 0x2028: // LINE SEPARATOR
        case 0x2029: // PARAGRAPH SEPARATOR
            return true;
        default:
            return false;
    }
}

bool isExtend(char32_t _codepoint) noexcept
{
    return contains(Core_Property::Grapheme_Extend, _codepoint)
        || contains(General_Category::Spacing_Mark, _codepoint);
}

} // end namespace
