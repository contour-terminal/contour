// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/PromptRegion.hpp>

#include <ranges>

namespace vtbackend
{

std::expected<PromptRegion, PromptRegionError> findLivePromptRegion(PromptRegionLineSource const& lines,
                                                                    size_t maxScanLines)
{
    auto region = PromptRegion {};
    auto sawInputBegin = false;
    auto sawAnyMark = false;

    for (auto const index: std::views::iota(size_t { 0 }, maxScanLines))
    {
        if (!lines.hasLineAt(index))
            break;

        auto const marks = lines.marksAt(index);

        if ((marks.flags & ShellMarkLineFlags).any())
            sawAnyMark = true;

        // Checked before Marked, so that a line which is BOTH a prompt start and an output start reads as
        // "a command is running" rather than "here is a fresh prompt". Reaching an output start at all
        // means the cursor sits at or below it, which is the command's territory, not the prompt's.
        //
        // Unless the line also carries a CommandEnd and the prompt start: then the output start is the
        // finished command's, one that printed nothing (`cd`), whose ;C, ;D and the next prompt's ;A all
        // landed on this line. The block rows (commandBlockRows()) read such a line the same way.
        auto const outputHasEnded =
            marks.flags.contains(LineFlag::CommandEnd) && marks.flags.contains(LineFlag::Marked);
        if (marks.flags.contains(LineFlag::OutputStart) && !outputHasEnded)
            return std::unexpected(PromptRegionError::InCommandOutput);

        // The FIRST one found is the one closest to the cursor, and therefore this prompt's — an older
        // prompt further up must not overwrite it.
        if (!sawInputBegin && marks.flags.contains(LineFlag::PromptEnd))
        {
            sawInputBegin = true;
            region.inputBegin = marks.promptEndOffset;
            region.inputBeginIndex = index;
        }

        if (marks.flags.contains(LineFlag::Marked))
        {
            region.startIndex = index;
            return region;
        }
    }

    // Nothing above the cursor carries a mark of any kind: there is no shell integration here, as opposed
    // to integration whose prompt start happens to be out of reach.
    if (!sawAnyMark)
        return std::unexpected(PromptRegionError::NoPromptMark);

    return std::unexpected(PromptRegionError::OutOfReach);
}

} // namespace vtbackend
