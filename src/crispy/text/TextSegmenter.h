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

#include <crispy/reference.h>
#include <fmt/format.h>
#include <iterator>

namespace crispy::text {

enum class FontFallbackPriority {
    Text,
    EmojiInText,
    EmojiInEmoji,
    Invalid,
};

using ScriptCode = unsigned;

struct Segment
{
    unsigned start;
    unsigned end;
    ScriptCode script;
    FontFallbackPriority fontFallbackPriority;
};

template <typename Iterator>
class Segmenter {
  public:
    Segmenter(Iterator _begin,
              size_t _size,
              unsigned _startOffset = 0);

    /// Splits input text into segments, such as pure text by script, emoji-emoji, or emoji-text.
    ///
    /// @retval true more data can be processed
    /// @retval false end of input data has been reached.
    bool consume(reference<Segment> _result);

  private:
    Segment segment_;
};

} // end namespace
