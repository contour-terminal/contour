// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

// What an action on a command block names: which block, and which part of it.
//
// Its own header, apart from the block store (CommandBlock.hpp) and the block rows (CommandBlockRows.hpp),
// because the configuration layer names both enums in every action that carries them: an action header
// included all over the frontend must not acquire the store, the clocks and <deque> with them.

/// Which block an action addresses.
enum class CommandBlockTarget : uint8_t
{
    Pointer = 0, ///< The block under the mouse pointer.
    Cursor,      ///< The block the cursor (the vi cursor in vi mode) is in.
    Last,        ///< The most recently finished block.
};

/// Which part of a command block a caller wants.
enum class CommandBlockPart : std::uint8_t
{
    Prompt,
    Output,
    PromptAndOutput,
};

} // namespace vtbackend
