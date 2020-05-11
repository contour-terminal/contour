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

#include <string_view>

namespace crispy::text {

class WordSegmenter
{
  public:
    using char_type = char32_t;
    using iterator = char_type const*;
    using view_type = std::basic_string_view<char_type>;

    constexpr WordSegmenter(iterator _begin, iterator _end)
      : left_{ _begin },
        right_{ _begin },
        state_{ _begin != _end ? (isDelimiter(*right_) ? State::NoWord : State::Word) : State::NoWord },
        end_{ _end }
    {
        ++*this;
    }

    constexpr WordSegmenter(std::basic_string_view<char_type> const& _str)
      : WordSegmenter(_str.begin(), _str.end())
    {}

    constexpr WordSegmenter()
      : WordSegmenter({}, {})
    {}

    constexpr bool empty() const noexcept { return size() == 0; }
    constexpr std::size_t size() const noexcept { return right_ - left_; }
    constexpr view_type operator*() const noexcept { return view_type(left_, right_ - left_); }

    constexpr WordSegmenter& operator++() noexcept
    {
        left_ = right_;
        while (right_ != end_)
        {
            switch (state_)
            {
                case State::NoWord:
                    if (!isDelimiter(*right_))
                    {
                        state_ = State::Word;
                        return *this;
                    }
                    break;
                case State::Word:
                    if (isDelimiter(*right_))
                    {
                        state_ = State::NoWord;
                        return *this;
                    }
                    break;
            }
            ++right_;
        }
        return *this;
    }

    constexpr bool operator==(WordSegmenter const& _rhs) const noexcept
    {
        return left_ == _rhs.left_ && right_ == _rhs.right_;
    }

    constexpr bool operator!=(WordSegmenter const& _rhs) const noexcept
    {
        return !(*this == _rhs);
    }

  private:
    constexpr bool isDelimiter(char_type _char) const noexcept
    {
        switch (_char)
        {
            case ' ':
            case '\r':
            case '\n':
            case '\t':
                return true;
            default:
                return false;
        }
    }

  private:
    enum class State { Word, NoWord };

    iterator left_;
    iterator right_;
    State state_;
    iterator end_;
};

} // end namespace
