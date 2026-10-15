// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <optional>
#include <ranges>
#include <vector>

namespace vtbackend
{

namespace
{
    /// What one pixel of the track shows so far.
    struct PixelWinner
    {
        ScrollbarMarkKind kind {};
        int64_t visibleRow {};
        int64_t targetStableId {};
    };

    /// Whether @p candidate takes the pixel from @p incumbent.
    ///
    /// The higher kind wins; between two of one kind the topmost mark does, and between two on one row the
    /// smaller id -- so the result never depends on the order the marks were reported in.
    [[nodiscard]] bool outranks(ScrollbarMarkInput const& candidate, PixelWinner const& incumbent) noexcept
    {
        if (candidate.kind != incumbent.kind)
            return candidate.kind > incumbent.kind;
        if (candidate.visibleRow != incumbent.visibleRow)
            return candidate.visibleRow < incumbent.visibleRow;
        return candidate.targetStableId < incumbent.targetStableId;
    }
} // namespace

std::vector<ScrollbarTick> scrollbarMarks(std::span<ScrollbarMarkInput const> inputs,
                                          int64_t visibleRowCount,
                                          int trackPixels,
                                          ScrollbarMarkSources sources)
{
    // A hidden or minimized scrollbar: no pixel to put a tick on, and the slots below are one per pixel. No
    // rows and no sources need no test of their own, as the filters below then drop every input.
    if (trackPixels <= 0)
        return {};

    // One slot per pixel and nothing per input: a flood of commands costs one pass over them and a
    // track's worth of memory, however many there are.
    auto winners = std::vector<std::optional<PixelWinner>>(static_cast<size_t>(trackPixels));

    for (auto const& input: inputs)
    {
        if (!sources.test(scrollbarMarkSourceOf(input.kind)))
            continue;

        // Outside the range the scrollbar spans: no fraction of the track stands for it. A range of no rows
        // has no row inside it, so nothing below divides by zero.
        if (input.visibleRow < 0 || input.visibleRow >= visibleRowCount)
            continue;

        // Rows are LineCount-sized (int), so the product stays far inside int64_t.
        auto const pixel = static_cast<size_t>(input.visibleRow * trackPixels / visibleRowCount);
        auto& winner = winners[pixel];
        if (!winner || outranks(input, *winner))
            winner = PixelWinner { .kind = input.kind,
                                   .visibleRow = input.visibleRow,
                                   .targetStableId = input.targetStableId };
    }

    auto ticks = std::vector<ScrollbarTick> {};
    for (auto const pixel: std::views::iota(0, trackPixels))
        if (auto const& winner = winners[static_cast<size_t>(pixel)])
            ticks.push_back(ScrollbarTick {
                .pixel = pixel, .kind = winner->kind, .targetStableId = winner->targetStableId });
    return ticks;
}

RGBColor scrollbarMarkColor(ColorPalette const& palette, ScrollbarMarkKind kind) noexcept
{
    if (auto const outcome = scrollbarMarkKindRow(kind).outcome)
        return palette.blockStatusColor(*outcome);
    return palette.foldMarkerHoverColors().foreground;
}

} // namespace vtbackend
