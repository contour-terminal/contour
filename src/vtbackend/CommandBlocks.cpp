// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CommandBlocks.h>

#include <string_view>
#include <utility>

namespace vtbackend
{

namespace
{
    /// Prepends @p lineText to @p accumulator, which is being filled backwards.
    void prependLine(std::string& accumulator, std::string_view lineText)
    {
        if (accumulator.empty())
            accumulator = lineText;
        else
            accumulator = std::string(lineText).append(1, '\n').append(accumulator);
    }

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
    auto current = CommandBlockText {};

    /// Closes the block just reconstructed and decides where the walk stands afterwards.
    auto const finalizeBlock = [&](LineFlags flags) {
        blocks.push_back(std::move(current));
        current = {};

        // The line that begins this block's prompt may ALSO carry the CommandEnd of the block before it —
        // a shell's precmd hook emits OSC 133;D (the previous command finished) and OSC 133;A (a new prompt
        // starts) back to back, so both land on the same line. Chain straight into that older block's
        // output rather than resuming the search one line further up, which would step clean over the
        // boundary we are already standing on.
        state = flags.contains(LineFlag::CommandEnd) && blocks.size() < maxBlocks ? ScanState::InOutput
                                                                                  : ScanState::Searching;
    };

    for (auto index = size_t { 0 }; index < lines.lineCount() && blocks.size() < maxBlocks; ++index)
    {
        auto const flags = lines.flagsAt(index);

        switch (state)
        {
            case ScanState::Searching:
                if (!flags.contains(LineFlag::CommandEnd))
                    break;
                state = ScanState::InOutput;
                // A CommandEnd line that is ALSO Marked belongs to the NEXT prompt (see finalizeBlock): the
                // shell printed the new prompt onto it, so its text is that prompt and not the last line of
                // the output that just ended. Taking it would leak the new prompt into every copied command
                // output. A CommandEnd line that is NOT Marked, on the other hand, is a genuine last output
                // line — a command whose output did not end in a newline, so the cursor never left it.
                if (!flags.contains(LineFlag::Marked))
                {
                    current.output = lines.textAt(index);
                    current.outputLineCount = 1;
                }
                break;

            case ScanState::InOutput:
                if (flags.contains(LineFlag::OutputStart))
                {
                    prependLine(current.output, lines.textAt(index));
                    ++current.outputLineCount;
                    state = ScanState::InPrompt;
                }
                else if (flags.contains(LineFlag::Marked))
                {
                    // A prompt with no OutputStart below it: the command printed nothing at all, so this
                    // line is its prompt and the block is already complete.
                    current.prompt = lines.textAt(index);
                    finalizeBlock(flags);
                }
                else
                {
                    prependLine(current.output, lines.textAt(index));
                    ++current.outputLineCount;
                }
                break;

            case ScanState::InPrompt:
                prependLine(current.prompt, lines.textAt(index));
                if (flags.contains(LineFlag::Marked))
                    finalizeBlock(flags);
                break;
        }
    }

    // The walk ran out of lines mid-block: keep whatever it did manage to reconstruct.
    if (state != ScanState::Searching && blocks.size() < maxBlocks)
        blocks.push_back(std::move(current));

    return blocks;
}

} // namespace vtbackend
