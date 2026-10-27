// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <variant>
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

MarksAbove marksAbove(std::span<ScrollbarMarkInput const> marks,
                      int64_t firstVisibleRow,
                      ScrollbarMarkSources sources) noexcept
{
    auto const holds = [&](ScrollbarMarkInput const& mark) {
        auto const& row = scrollbarMarkKindRow(mark.kind);
        return mark.visibleRow < firstVisibleRow && sources.test(row.source)
               && row.overlay == OverlayHold::KeepsShown;
    };
    return std::ranges::any_of(marks, holds) ? MarksAbove::Some : MarksAbove::None;
}

bool marksOnScrollbarTrack(RGBColor color, RGBColor pane) noexcept
{
    constexpr auto Black = RGBColor { 0x00, 0x00, 0x00 };
    constexpr auto White = RGBColor { 0xFF, 0xFF, 0xFF };
    return std::ranges::all_of(
        std::array { pane,
                     mixColor(pane, Black, ScrollbarTrackMaxTint),
                     mixColor(pane, White, ScrollbarTrackMaxTint) },
        [&](RGBColor ground) { return contrastRatio(color, ground) >= MinimumMarkContrast; });
}

namespace
{
    /// The search tick: the highlight's fill, else the scheme's yellow, else the first of its bright yellow
    /// and its text colour that marks on the track, else black or white, whichever stands off the pane more.
    [[nodiscard]] RGBColor searchTickColor(ColorPalette const& palette) noexcept
    {
        auto candidates = std::array<RGBColor, 3> {};
        auto count = size_t { 0 };
        // Yellow means search. A highlight that borrows the cell's own colours names none, and the tick
        // then falls back to the scheme's yellow.
        if (auto const* const fill = std::get_if<RGBColor>(&palette.searchHighlight.background))
            candidates[count++] = *fill;
        candidates[count++] = palette.normalColor(3);
        candidates[count++] = palette.brightColor(3);
        for (auto const color: std::span { candidates }.first(count))
            if (marksOnScrollbarTrack(color, palette.defaultBackground))
                return color;
        constexpr auto Black = RGBColor { 0x00, 0x00, 0x00 };
        constexpr auto White = RGBColor { 0xFF, 0xFF, 0xFF };
        return contrastRatio(Black, palette.defaultBackground)
                       >= contrastRatio(White, palette.defaultBackground)
                   ? Black
                   : White;
    }
} // namespace

RGBColor scrollbarMarkColor(ColorPalette const& palette, ScrollbarMarkKind kind) noexcept
{
    if (auto const outcome = scrollbarMarkKindRow(kind).outcome)
        return palette.blockStatusColor(*outcome);
    if (kind == ScrollbarMarkKind::SearchMatch)
        return searchTickColor(palette);
    return palette.foldMarkerHoverColors().foreground;
}

} // namespace vtbackend
