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
#include <crispy/text/UnicodeTables.h>

#include <string_view>

namespace crispy::text {

/// Implements http://www.unicode.org/reports/tr29/tr29-27.html#Grapheme_Cluster_Boundary_Rules
class GraphemeSegmenter {
  public:
    constexpr GraphemeSegmenter(char32_t const* _begin, char32_t const* _end) noexcept
      : left_{ _begin },
        right_{ _begin },
        end_{ _end }
    {
        ++*this;
    }

    constexpr GraphemeSegmenter(std::u32string_view const& _sv) noexcept
      : GraphemeSegmenter(_sv.begin(), _sv.end())
    {}

    constexpr GraphemeSegmenter() noexcept
      : GraphemeSegmenter({}, {})
    {}

    constexpr GraphemeSegmenter& operator++()
    {
        right_ = left_;
        // TODO
        return *this;
    }

    constexpr std::u32string_view operator*() const noexcept
    {
        return std::u32string_view(left_, right_ - left_);
    }

    constexpr bool empty() const noexcept
    {
        return right_ == end_;
    }

    constexpr bool operator==(GraphemeSegmenter const& _rhs) const noexcept
    {
        return (empty() && _rhs.empty())
            || (left_ == _rhs.left_ && right_ == _rhs.right_);
    }

    /// Tests if codepoint @p a and @p b are breakable, and thus, two different grapheme clusters.
    ///
    /// @retval true both codepoints to not belong to the same grapheme cluster
    /// @retval false both codepoints belong to the same grapheme cluster
    static constexpr bool breakable(char32_t a, char32_t b)
    {
        constexpr char32_t CR = 0x000D;
        constexpr char32_t LF = 0x000A;

        // Do not break between a CR and LF. Otherwise, break before and after controls.
        if (a == CR && b == LF) // GB3
            return false;

        if (a == CR || a == LF || contains(General_Category::Control, a)) // GB4
            return false;

        if (b == CR || b == LF || contains(General_Category::Control, b)) // GB5
            return false;

        // Do not break Hangul syllable sequences.
        // GB6: TODO
        // GB7: TODO
        // GB8: TODO

        // Do not break between regional indicator symbols.
        if (isRegionalIndicator(a) && isRegionalIndicator(b)) // GB8a
            return false;

        // Do not break before extending characters.
        if (isExtend(b)) // GB9
            return false;

        // EXT: Do not break before SpacingMarks, or after Prepend characters.
        if (contains(General_Category::Spacing_Mark, b)) // GB9a
            return false;

        // NB: wrt "Prepend": Currently there are no characters with this value.
        if (false/*contains(General_Category::Pepend, a)*/) // GB9b
            return false;

        // Otherwise, break everywhere.
        return true; // GB10
    }

    static constexpr bool nonbreakable(char32_t a, char32_t b)
    {
        return !breakable(a, b);
    }

  private:
    char32_t const* left_;
    char32_t const* right_;
    char32_t const* end_;
};

} // end namespace
