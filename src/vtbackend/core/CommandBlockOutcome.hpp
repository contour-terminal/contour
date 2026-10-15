// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace vtbackend
{

/// How a command block reads at a glance -- what the gutter and the scrollbar colour it by.
///
/// In core/ rather than shell/CommandBlock.hpp because ColorPalette (core/) resolves a colour per
/// outcome, and core/ may include nothing but core/. shell/CommandBlock.hpp includes this header, so
/// every consumer of the block vocabulary sees it under the same name.
enum class CommandBlockOutcome : uint8_t
{
    Success = 0, ///< Exit code zero, no command run yet, or nothing known.
    Failure,     ///< A non-zero exit code, a signal, a crash or an interrupt.
    Running,     ///< The command is still running.
};

/// How many outcomes there are: the size of a table indexed by CommandBlockOutcome. A constant rather than a
/// Count enumerator, which every switch over the outcome would then have to handle.
inline constexpr std::size_t CommandBlockOutcomeCount =
    static_cast<std::size_t>(CommandBlockOutcome::Running) + 1;

} // namespace vtbackend
