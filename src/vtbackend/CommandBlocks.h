// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/LineFlags.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vtbackend
{

/// The text of one shell-integration command block (OSC 133).
///
/// Line-granular, because the OSC 133 markers are: @ref prompt is the prompt LINE(S) — which visually
/// include the command the user typed — and @ref output is what the command then printed. The command
/// string on its own exists only when the shell sent it explicitly (OSC 133;C cmdline_url), which is a
/// different, opt-in channel.
struct CommandBlockText
{
    std::string prompt;      ///< The prompt line(s) the command was entered at.
    std::string output;      ///< What the command printed.
    int outputLineCount = 0; ///< The number of lines @ref output spans.
};

/// Which part of a command block a caller wants.
enum class CommandBlockPart : std::uint8_t
{
    Prompt,
    Output,
    PromptAndOutput,
};

/// The requested part of @p block, ready for the clipboard.
/// @param block The block to render.
/// @param part Which part of it.
/// @return The text, with prompt and output separated by a newline when both are asked for and present.
[[nodiscard]] std::string textOf(CommandBlockText const& block, CommandBlockPart part);

/// Supplies the LOGICAL lines a command-block scan walks, indexed backwards from the line it starts at.
///
/// This is the dependency-injection seam of the scanner: the state machine below is a pure function of
/// the flags and the text it is handed, so it can be exercised against a plain vector of lines — with no
/// Grid, no Screen and no terminal behind it.
///
/// Logical, not physical: a long line is stored as a head plus the continuations a wrap chopped it into,
/// but the shell's marks name the line it wrote, and so does this scan. A window resize re-chops the
/// physical pieces and must not change a single block.
class CommandBlockLineSource
{
  public:
    CommandBlockLineSource() = default;
    CommandBlockLineSource(CommandBlockLineSource&&) = default;
    CommandBlockLineSource(CommandBlockLineSource const&) = default;
    CommandBlockLineSource& operator=(CommandBlockLineSource&&) = default;
    CommandBlockLineSource& operator=(CommandBlockLineSource const&) = default;
    virtual ~CommandBlockLineSource() = default;

    /// Whether there is a logical line at @p index — 0 being the one the scan starts at, counting upwards.
    ///
    /// A predicate rather than a count, and deliberately so: counting the logical lines would mean walking
    /// the whole scrollback before the scan has looked at a single flag, when the scan is precisely what
    /// knows where to stop. "Copy the last command" reads a handful of lines above the cursor, not 100'000.
    ///
    /// The scan is a single forward pass — it asks about each index once, in increasing order — which is
    /// what lets an implementation walk lazily and keep no more state than the line it is standing on.
    [[nodiscard]] virtual bool hasLineAt(size_t index) const = 0;

    /// The flags of the logical line @p index lines ABOVE the starting one (0 being the starting one).
    [[nodiscard]] virtual LineFlags flagsAt(size_t index) const = 0;

    /// The text of that line.
    ///
    /// Only asked for the lines the scan has decided belong to a block, so a scrollback with no shell
    /// integration anywhere in it costs a flags-only walk and not one string per line.
    [[nodiscard]] virtual std::string textAt(size_t index) const = 0;

    /// The part of that line a finished command PRINTED — everything up to the point where the shell
    /// closed the command with OSC 133;D.
    ///
    /// A shell emits ;D from its precmd hook, so the cursor is still standing wherever the command's
    /// output left it. When that output did not end in a newline the prompt printed next lands on the very
    /// same line, and this is the half of it that belongs to the command. Empty in the ordinary case,
    /// where the output ended in a newline and the whole line belongs to the prompt alone.
    [[nodiscard]] virtual std::string textBeforeCommandEndAt(size_t index) const = 0;

    /// The other half: what the shell painted from the command-end onwards — its next prompt.
    /// The whole line when it carries no CommandEnd.
    [[nodiscard]] virtual std::string textFromCommandEndAt(size_t index) const = 0;
};

/// The @p maxBlocks most recently FINISHED command blocks in @p lines, most recent first.
///
/// A backward walk over the line flags a shell's OSC 133 marks leave behind: CommandEnd (;D) closes a
/// block, OutputStart (;C) separates its output from its prompt, and Marked (;A) begins the prompt.
///
/// @param lines The lines to walk, newest first.
/// @param maxBlocks How many blocks to reconstruct at most.
/// @return The blocks, most recent first; empty when the lines hold no finished command.
[[nodiscard]] std::vector<CommandBlockText> scanCommandBlocksBackward(CommandBlockLineSource const& lines,
                                                                      size_t maxBlocks);

} // namespace vtbackend
