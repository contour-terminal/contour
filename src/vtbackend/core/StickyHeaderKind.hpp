// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// What the sticky header shows.
///
/// In core/ rather than shell/StickyHeader.hpp because the render buffer that carries the header and the
/// renderer that draws it name it too, and render/ may include nothing from shell/. shell/StickyHeader.hpp
/// includes this header, so every caller of the decision sees it under the same name.
enum class StickyHeaderKind : uint8_t
{
    Command = 0, ///< The block's input row, copied from the grid with its own colours.
    Evicted,     ///< `⋯ <command> — earlier output evicted`: the head has left the scrollback.
};

} // namespace vtbackend
