// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace contour::command
{

struct ContextMenuEntry;

/// Whether a block has an output part yet: its command started (OSC 133;C).
enum class OutputPresence : std::uint8_t
{
    Absent = 0, ///< Still at its prompt.
    Present,    ///< Running or finished.
};

/// Whether a block's command line is known, which is what "Copy Command" copies.
enum class CommandLinePresence : std::uint8_t
{
    Absent = 0, ///< Neither reported nor recovered.
    Present,    ///< Reported by the shell (OSC 133;C cmdline_url), or recovered from the grid.
};

/// Whether the shell shows a prompt that a command typed now would run at.
enum class PromptPresence : std::uint8_t
{
    Absent = 0, ///< A command is running, or the shell marks no prompts at all.
    Present,    ///< The shell waits at its prompt.
};

/// Whether a block's output can be folded, and how it stands.
enum class FoldPresence : std::uint8_t
{
    None = 0,  ///< No fold hangs on the block (folding is off, or the block has no output rows).
    Expanded,  ///< Its fold is open.
    Collapsed, ///< Its fold is closed.
};

/// The command block a menu was opened for, with what its rows may act on.
///
/// A plain snapshot, taken once under the terminal lock when the menu opens. buildBlockMenu() is a pure
/// function of it, so each row and each rule about when it is enabled is decided and tested with no
/// terminal, no window and no clipboard behind it.
struct BlockMenuState
{
    vtbackend::CommandBlockId id {}; ///< The block; never zero.

    /// The block's facts, one line each, that head the menu: its command, how it ended, where it ran and
    /// when it started, each only when known. Empty for a block with nothing to say, such as a prompt.
    std::vector<std::string> header {};

    OutputPresence output = OutputPresence::Absent;                ///< Whether it has output to act on.
    CommandLinePresence commandLine = CommandLinePresence::Absent; ///< Whether its command line is known.
    PromptPresence livePrompt = PromptPresence::Absent;            ///< Whether a command typed now would run.
    FoldPresence fold = FoldPresence::None; ///< Whether its output can be folded, and how it stands.
};

/// How many command rows the block menu shows below its header, whatever the state.
inline constexpr std::size_t BlockMenuRowCount = 5;

/// The block menu for @p state: the header lines as disabled rows, a separator, then the rows that act on
/// the block, each pinned to it by id.
///
/// The rows are Copy Output, Copy Command, Rerun…, Open in Pager, and Collapse Output (Expand Output while
/// its fold is closed). Rows whose precondition is unmet are disabled rather than hidden, so the menu keeps
/// its shape from block to block; only the header, and its separator, are left out when it has no lines.
/// @param state The block the menu was opened for.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildBlockMenu(BlockMenuState const& state);

} // namespace contour::command
