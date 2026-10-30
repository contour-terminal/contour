// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Which part of a command block's rail a row draws.
///
/// In core/ rather than shell/BlockRail.hpp because the render buffer that carries the rail and the
/// renderer that draws it name it too, and render/ may include nothing from shell/. shell/BlockRail.hpp
/// includes this header, so every caller of the decision sees it under the same name.
enum class RailSegment : uint8_t
{
    None = 0, ///< No rail: a row of no block, or of the live prompt.
    Head,     ///< The block's first row: the bar starts here, rounded at its top.
    Body,     ///< A row between the first and the last: the bar runs through.
    Tail,     ///< The block's last row: the bar ends here, rounded at its bottom.
    Single,   ///< The block's only row (a collapsed fold, or one row): rounded at both ends.
};

/// The mark a rail row carries, centred on the bar.
enum class RailMark : uint8_t
{
    None = 0, ///< No mark.
    Check,    ///< ✓: a successful block, while hovered or focused.
    Cross,    ///< ✗ on a danger disc: a failed block.
    Running,  ///< ▸: a block still running.
    UserMark, ///< ◆: a row the user marked (vi `mm`) that heads nothing.
};

} // namespace vtbackend
