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
#pragma once

#include <crispy/text/UnicodeTraits.h>
#include <array>
#include <algorithm>
#include <utility>

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
#if 0 // TODO
    if (isEmojiModifierBase(_codepoint))
        return EmojiSegmentationCategory::EmojiModifierBase;
    if (isEmojiModifier(_codepoint))
        return EmojiSegmentationCategory::EmojiModifier;
    if (isRegionalIndicator(_codepoint))
        return EmojiSegmentationCategory::RegionalIndicator;
    if (isEmojiKeycapBase(_codepoint))
        return EmojiSegmentationCategory::KeyCapBase;
    if (isEmojiEmojiDefault(_codepoint))
        return EmojiSegmentationCategory::EmojiEmojiPresentation;
    if (isEmojiTextDefault(_codepoint))
        return EmojiSegmentationCategory::EmojiTextPresentation;
    if (isEmoji(_codepoint))
        return EmojiSegmentationCategory::Emoji;
#endif

    return EmojiSegmentationCategory::Invalid;
}

class EmojiSegmenter {
  private:
    char32_t const* begin_;
    char32_t const* end_;
    bool isEmoji_ = false;
    EmojiSegmentationCategory category_;

  public:
    constexpr EmojiSegmenter(char32_t const* _begin, char32_t const* _end) noexcept
      : begin_{ _begin },
        end_{ _end },
        isEmoji_{false},
        category_{ EmojiSegmentationCategory::Invalid }
    {}

    constexpr EmojiSegmenter(std::u32string_view const& _sv) noexcept
      : EmojiSegmenter(_sv.begin(), _sv.end())
    {}

    constexpr bool isText() const noexcept
    {
        return category_ == EmojiSegmentationCategory::EmojiTextPresentation;
    }

    constexpr std::u32string_view operator*() const noexcept
    {
        return std::u32string_view(begin_, end_ - begin_);
    }

    constexpr EmojiSegmenter& operator++()
    {
        begin_ = end_;
        isEmoji_ = false;
        // ...

        // TODO: after increment, [begin, end] point to the next emoji - ?? or [end, end] if none found ??
        // use (ragel) FSM here?
        return *this;
    }

    constexpr EmojiSegmenter& operator++(int) { return ++*this; }

    constexpr bool operator==(EmojiSegmenter const& _rhs) const noexcept
    {
        return begin_ == _rhs.begin_ && end_ == _rhs.end_;
    }

    constexpr bool operator!=(EmojiSegmenter const& _rhs) const noexcept { return !(*this == _rhs); }
};

/// Tests whether given codepoint range is one single emoji grapheme cluster.
// bool isSingleEmojiSegment(char32_t const* _begin, char32_t const* _end)
// {
//     EmojiSegmenter segmenter(_begin, _end);
//     auto i = segmenter.begin();
//     while (i != segmenter.end())
//         ++i;
//     return false; // TODO
// }
//

} // end namespace
