// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/PromptRegion.h>

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

        if ((marks.flags & HeadOnlyLineFlags).any())
            sawAnyMark = true;

        // Checked before Marked, so that a line which is BOTH a prompt start and an output start reads as
        // "a command is running" rather than "here is a fresh prompt". Reaching an output start at all
        // means the cursor sits at or below it, which is the command's territory, not the prompt's.
        if (marks.flags.contains(LineFlag::OutputStart))
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
