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

#include <crispy/text/Unicode.h>
#include <array>
#include <algorithm>
#include <utility>

#include <fmt/format.h>

namespace crispy::text {

/**
 * EmojiSegmenter API for segmenting emojis into text-emoji and emoji-emoji presentations.
 *
 * This segmenter is segmenting emojis by their presentation property (text or emoji), that is,
 * whether an emoji is to be rendered in text mode or in emoji (colored) mode.
 *
 * It must be segmenting only emojis and not any other codepoints.
 */
class EmojiSegmenter {
  private:
    char32_t const* buffer_;
    size_t size_;
    size_t lastCursor_;
    size_t cursor_;

    bool isEmoji_ = false;

  public:
    EmojiSegmenter(char32_t const* _buffer, size_t _size) noexcept
      : buffer_{ _buffer },
        size_{ _size },
        lastCursor_{ 0 },
        cursor_{ 0 },
        isEmoji_{false}
    {
        consume();
    }

    EmojiSegmenter(std::u32string_view const& _sv) noexcept
      : EmojiSegmenter(_sv.data(), _sv.size())
    {}

    /// @returns whether or not the currently segmented emoji is to be rendered in text-presentation or not.
    constexpr bool isText() const noexcept { return !isEmoji_; }

    /// @returns whether or not the currently segmented emoji is to be rendered in emoji-presentation or not.
    constexpr bool isEmoji() const noexcept { return isEmoji_; }

    /// @returns the underlying current segment that has been processed the last.
    constexpr std::u32string_view operator*() const noexcept
    {
        if (cursor_ > 0)
            return std::u32string_view(buffer_ + lastCursor_, cursor_ - lastCursor_);
        else
            return std::u32string_view{};
    }

    void consume() noexcept;

    EmojiSegmenter& operator++() noexcept { consume(); return *this; }
    EmojiSegmenter& operator++(int) noexcept { return ++*this; }

    constexpr bool operator==(EmojiSegmenter const& _rhs) const noexcept
    {
        return buffer_ == _rhs.buffer_ && size_ == _rhs.size_;
    }

    constexpr bool operator!=(EmojiSegmenter const& _rhs) const noexcept { return !(*this == _rhs); }
};

} // end namespace
