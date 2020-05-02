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

#include <fmt/format.h>
#include <iterator>

namespace crispy::text {

enum class FontOrientation {
    LTR,
    RTL
};

enum class FontFallbackPriority {
    Text,
    EmojiInText,
    EmojiInEmoji,
    Invalid,
};

enum class RenderOrientation {
    Preserve,
    RotateSideways
};

enum class ScriptCode {
    Invalid // TODO
};

struct Segment
{
    unsigned start;
    unsigned end;
    ScriptCode script;
    RenderOrientation renderOrientation;
    FontFallbackPriority fontFallbackPriority;
};

template <typename Iterator>
class Segmenter {
  public:
    Segmenter(Iterator _begin, Iterator _end,
              FontOrientation _orientation,
              unsigned _startOffset = 0);

    bool consume();

    operator Segment const& () const noexcept { return segment_; }

  private:
    Segment segment_;
};

} // end namespace
