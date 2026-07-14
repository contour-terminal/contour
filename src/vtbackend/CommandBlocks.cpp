// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CommandBlocks.h>

#include <crispy/utils.h>

#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace vtbackend
{

namespace
{
    /// A block being reconstructed: its lines are collected newest-first and joined exactly once.
    ///
    /// Collecting and joining once is what keeps the walk linear. Growing a string by prepending each
    /// line to it instead copies the whole text accumulated so far, twice, for every line — quadratic in
    /// the size of the block. This walk runs on the GUI thread, under the terminal lock, every time the
    /// user right-clicks, over a block that may be the entire scrollback.
    struct BlockUnderConstruction
    {
        std::vector<std::string> promptLines; ///< Newest first.
        std::vector<std::string> outputLines; ///< Newest first.

        [[nodiscard]] bool empty() const noexcept { return promptLines.empty() && outputLines.empty(); }

        [[nodiscard]] CommandBlockText finish() &&
        {
            return CommandBlockText {
                // Reversed, because the walk collected them bottom-up and the screen reads top-down.
                .prompt = crispy::join_with(promptLines | std::views::reverse, "\n"),
                .output = crispy::join_with(outputLines | std::views::reverse, "\n"),
                .outputLineCount = static_cast<int>(outputLines.size()),
            };
        }
    };

    /// Where a backward walk stands between two command blocks.
    enum class ScanState : uint8_t
    {
        Searching, ///< Looking for the CommandEnd that closes the next block.
        InOutput,  ///< Collecting the command's output, walking up towards its OutputStart.
        InPrompt,  ///< Collecting the prompt, walking up towards the Marked line that begins it.
    };
} // namespace

std::string textOf(CommandBlockText const& block, CommandBlockPart part)
{
    switch (part)
    {
        case CommandBlockPart::Prompt: return block.prompt;
        case CommandBlockPart::Output: return block.output;
        case CommandBlockPart::PromptAndOutput:
            if (block.prompt.empty())
                return block.output;
            if (block.output.empty())
                return block.prompt;
            return block.prompt + '\n' + block.output;
    }
    return {};
}

std::vector<CommandBlockText> scanCommandBlocksBackward(CommandBlockLineSource const& lines, size_t maxBlocks)
{
    auto blocks = std::vector<CommandBlockText> {};
    if (maxBlocks == 0)
        return blocks;

    auto state = ScanState::Searching;
    auto current = BlockUnderConstruction {};

    /// Opens the block that the CommandEnd on line @p index closes.
    auto const openBlockAt = [&](size_t index, LineFlags flags) {
        // The line a command was closed on is not necessarily a line of its own. When the output did not
        // end in a newline the cursor never left it, so the shell's next prompt is painted onto the same
        // line, and only the columns BEFORE the ;D belong to the command. In the ordinary case — output
        // ended in a newline, the prompt got a fresh line — there is nothing before the ;D and the whole
        // line belongs to the prompt.
        if (auto tail = lines.textBeforeCommandEndAt(index); !tail.empty())
            current.outputLines.push_back(std::move(tail));

        // The line may carry the OutputStart of the very command it also ends: a command whose output fits
        // on the line it started on (`printf hello`), or one that printed nothing at all.
        state = flags.contains(LineFlag::OutputStart) ? ScanState::InPrompt : ScanState::InOutput;
    };

    /// Closes the block just reconstructed and decides where the walk stands afterwards.
    auto const finalizeBlock = [&](size_t index, LineFlags flags) {
        blocks.push_back(std::move(current).finish());
        current = {};

        // The line that begins this block's prompt may ALSO carry the CommandEnd of the block before it —
        // a shell's precmd hook emits OSC 133;D (the previous command finished) and OSC 133;A (a new prompt
        // starts) back to back, so both land on the same line. Chain straight into that older block's
        // output rather than resuming the search one line further up, which would step clean over the
        // boundary we are already standing on.
        if (flags.contains(LineFlag::CommandEnd) && blocks.size() < maxBlocks)
            openBlockAt(index, flags);
        else
            state = ScanState::Searching;
    };

    // A single forward pass, asking for each logical line only once and only as far as it needs to go: the
    // source walks the grid lazily behind hasLineAt(), so a "copy the last command" over a 100'000-line
    // scrollback reads the handful of lines above the cursor and stops.
    for (auto index = size_t { 0 }; lines.hasLineAt(index) && blocks.size() < maxBlocks; ++index)
    {
        auto const flags = lines.flagsAt(index);

        switch (state)
        {
            case ScanState::Searching:
                if (flags.contains(LineFlag::CommandEnd))
                    openBlockAt(index, flags);
                break;

            case ScanState::InOutput:
                if (flags.contains(LineFlag::Marked) && !flags.contains(LineFlag::OutputStart))
                {
                    // A prompt with no OutputStart below it: the command printed nothing at all, so this
                    // line is its prompt and the block is already complete.
                    current.promptLines.push_back(lines.textFromCommandEndAt(index));
                    finalizeBlock(index, flags);
                    break;
                }
                current.outputLines.push_back(lines.textAt(index));
                if (flags.contains(LineFlag::OutputStart))
                    state = ScanState::InPrompt;
                break;

            case ScanState::InPrompt:
                // Only the prompt's half of the line: the Marked line that begins a prompt is the very
                // line the previous command may have ended part-way into.
                current.promptLines.push_back(lines.textFromCommandEndAt(index));
                if (flags.contains(LineFlag::Marked))
                    finalizeBlock(index, flags);
                break;
        }
    }

    // The walk ran out of lines mid-block: keep whatever it did manage to reconstruct, but only if that is
    // something. A walk that collected nothing at all — a freshly cleared screen, whose only line is the
    // prompt carrying the CommandEnd of a command that has scrolled away — describes no command, and an
    // all-empty block would have callers copy "" to the clipboard for a command that never existed.
    if (state != ScanState::Searching && blocks.size() < maxBlocks && !current.empty())
        blocks.push_back(std::move(current).finish());

    return blocks;
}

} // namespace vtbackend
