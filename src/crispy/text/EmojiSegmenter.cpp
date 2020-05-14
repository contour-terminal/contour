/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <crispy/text/EmojiSegmenter.h>
#include <crispy/text/Unicode.h>
#include <crispy/escape.h>
#include <iostream>

namespace crispy::text {

enum class EmojiSegmentationCategory
{
    Invalid = -1,

    Emoji = 0,
    EmojiTextPresentation = 1,
    EmojiEmojiPresentation = 2,
    EmojiModifierBase = 3,
    EmojiModifier = 4,
    EmojiVSBase = 5,
    RegionalIndicator = 6,
    KeyCapBase = 7,
    CombiningEnclosingKeyCap = 8,
    CombiningEnclosingCircleBackslash = 9,
    ZWJ = 10,
    VS15 = 11,
    VS16 = 12,
    TagBase = 13,
    TagSequence = 14,
    TagTerm = 15,
};

constexpr inline EmojiSegmentationCategory toCategory(char32_t _codepoint)
{
    auto isEmojiKeycapBase = [](char32_t _codepoint) -> bool {
        return ('0' <= _codepoint && _codepoint <= '9')
            || _codepoint == '#' || _codepoint == '*';
    };

    if (_codepoint == 0x20e3)
        return EmojiSegmentationCategory::CombiningEnclosingKeyCap;
    if (_codepoint == 0x20e0)
        return EmojiSegmentationCategory::CombiningEnclosingCircleBackslash;
    if (_codepoint == 0x200d)
        return EmojiSegmentationCategory::ZWJ;
    if (_codepoint == 0xfe0e)
        return EmojiSegmentationCategory::VS15;
    if (_codepoint == 0xfe0f)
        return EmojiSegmentationCategory::VS16;
    if (_codepoint == 0x1f3f4)
        return EmojiSegmentationCategory::TagBase;
    if ((_codepoint >= 0xE0030 && _codepoint <= 0xE0039) || (_codepoint >= 0xE0061 && _codepoint <= 0xE007A))
        return EmojiSegmentationCategory::TagSequence;
    if (_codepoint == 0xE007F)
        return EmojiSegmentationCategory::TagTerm;
    if (emoji_modifier_base(_codepoint))
        return EmojiSegmentationCategory::EmojiModifierBase;
    if (emoji_modifier(_codepoint))
        return EmojiSegmentationCategory::EmojiModifier;
    if (grapheme_cluster_break::regional_indicator(_codepoint))
        return EmojiSegmentationCategory::RegionalIndicator;
    if (isEmojiKeycapBase(_codepoint))
        return EmojiSegmentationCategory::KeyCapBase;
    if (emoji_presentation(_codepoint))
        return EmojiSegmentationCategory::EmojiEmojiPresentation;
    if (emoji(_codepoint) && !emoji_presentation(_codepoint))
        return EmojiSegmentationCategory::EmojiTextPresentation;
    if (emoji(_codepoint))
        return EmojiSegmentationCategory::Emoji;

    return EmojiSegmentationCategory::Invalid;
}

class RagelIterator {
    EmojiSegmentationCategory category_;
    char32_t const* buffer_;
    size_t size_;
    size_t cursor_;

  public:
    constexpr RagelIterator(char32_t const* _buffer, size_t _size, size_t _cursor) noexcept
      : category_{ EmojiSegmentationCategory::Invalid },
        buffer_{ _buffer },
        size_{ _size },
        cursor_{ _cursor }
    {
        updateCategory();
    }

    constexpr RagelIterator() noexcept : RagelIterator(U"", 0, 0) {}

    constexpr char32_t codepoint() const noexcept { return buffer_[cursor_]; }
    constexpr EmojiSegmentationCategory category() const noexcept { return category_; }
    constexpr size_t cursor() const noexcept { return cursor_; }

    constexpr void updateCategory()
    {
        category_ = toCategory(codepoint());
    }

    constexpr int operator*() const noexcept { return static_cast<int>(category_); }

    constexpr RagelIterator& operator++() noexcept { cursor_++; updateCategory(); return *this; }
    constexpr RagelIterator& operator--(int) noexcept { cursor_--; updateCategory(); return *this; }

    constexpr RagelIterator operator+(int v) const noexcept { return {buffer_, size_, cursor_ + v}; }
    constexpr RagelIterator operator-(int v) const noexcept { return {buffer_, size_, cursor_ - v}; }

    constexpr RagelIterator& operator=(int v) noexcept { cursor_ = v; updateCategory(); return *this; }

    constexpr bool operator==(RagelIterator const& _rhs) const noexcept
    {
        return buffer_ == _rhs.buffer_ && size_ == _rhs.size_ && cursor_ == _rhs.cursor_;
    }

    constexpr bool operator!=(RagelIterator const& _rhs) const noexcept { return !(*this == _rhs); }
};

namespace {
using emoji_text_iter_t = RagelIterator;
#include "emoji_presentation_scanner.c"
}

void EmojiSegmenter::consume() noexcept
{
    if (size_ != 0)
    {
        lastCursor_ = cursor_;
        auto const i = RagelIterator(buffer_, size_, cursor_);
        auto const e = RagelIterator(buffer_, size_, size_);
        auto const o = scan_emoji_presentation(i, e, &isEmoji_);
        cursor_ = o.cursor();
    }
}

} // end namespace
