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

/// Supplies the lines a command-block scan walks, indexed backwards from the line it starts at.
///
/// This is the dependency-injection seam of the scanner: the state machine below is a pure function of
/// the flags and the text it is handed, so it can be exercised against a plain vector of lines — with no
/// Grid, no Screen and no terminal behind it.
class CommandBlockLineSource
{
  public:
    CommandBlockLineSource() = default;
    CommandBlockLineSource(CommandBlockLineSource&&) = default;
    CommandBlockLineSource(CommandBlockLineSource const&) = default;
    CommandBlockLineSource& operator=(CommandBlockLineSource&&) = default;
    CommandBlockLineSource& operator=(CommandBlockLineSource const&) = default;
    virtual ~CommandBlockLineSource() = default;

    /// How many lines can be walked, counting the line the scan starts at and everything above it.
    [[nodiscard]] virtual size_t lineCount() const = 0;

    /// The flags of the line @p index lines ABOVE the starting line (0 being the starting line itself).
    [[nodiscard]] virtual LineFlags flagsAt(size_t index) const = 0;

    /// The text of that line.
    ///
    /// Only asked for the lines the scan has decided belong to a block, so a scrollback with no shell
    /// integration anywhere in it costs a flags-only walk and not one string per line.
    [[nodiscard]] virtual std::string textAt(size_t index) const = 0;
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
