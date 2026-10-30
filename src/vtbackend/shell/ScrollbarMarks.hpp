// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>

#include <core/Flags.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace vtbackend
{

struct ColorPalette;

/// What a scrollbar tick stands for.
///
/// Declared in ASCENDING priority, and the order is load-bearing: where several marks fall on one pixel
/// of the track, the highest enumerator is the one drawn, so a failure is never hidden behind the success
/// that ran beside it.
enum class ScrollbarMarkKind : uint8_t
{
    Command = 0, ///< A finished command that succeeded, or whose exit status is unknown.
    UserMark,    ///< A line the user marked with vi `mm` (LineFlag::UserMark).
    SearchMatch, ///< A row the active search matches.
    Running,     ///< A command that is still running.
    Failure,     ///< A finished command that exited non-zero, or was killed.
};

/// How a tick of some kind is drawn.
enum class ScrollbarTickStyle : uint8_t
{
    Solid = 0, ///< Filled with the kind's colour.
    Hollow,    ///< A one-point outline in the kind's colour: something still going on.
};

/// The groups of marks `scrollbar.marks` switches on, one bit each.
///
/// A bitmask rather than one enum per group because every combination is legal and each is one token of
/// a YAML list: flags that genuinely combine.
enum class ScrollbarMarkSource : uint8_t
{
    Failures = 1 << 0,      ///< ScrollbarMarkKind::Failure.
    Commands = 1 << 1,      ///< ScrollbarMarkKind::Command.
    UserMarks = 1 << 2,     ///< ScrollbarMarkKind::UserMark.
    Running = 1 << 3,       ///< ScrollbarMarkKind::Running.
    SearchMatches = 1 << 4, ///< ScrollbarMarkKind::SearchMatch.
};

/// A set of ScrollbarMarkSource bits.
using ScrollbarMarkSources = core::Flags<ScrollbarMarkSource>;

/// Every source -- the default, so a user who never heard of the setting sees every tick.
constexpr inline auto AllScrollbarMarkSources = ScrollbarMarkSources {
    ScrollbarMarkSource::Failures, ScrollbarMarkSource::Commands,      ScrollbarMarkSource::UserMarks,
    ScrollbarMarkSource::Running,  ScrollbarMarkSource::SearchMatches,
};

/// One mark the scrollbar may draw, as Terminal::scrollbarMarkInputs() reports it.
struct ScrollbarMarkInput
{
    /// The row within the scrollbar's range, the oldest history row being 0, counted in VISIBLE rows: a
    /// collapsed fold is its one head row, exactly as in Viewport::scrollableLineCount().
    int64_t visibleRow {};

    /// What the mark stands for.
    ScrollbarMarkKind kind {};

    /// The line a click brings to the top: a block's head, or the user-marked or matched line itself.
    int64_t targetStableId {};

    [[nodiscard]] bool operator==(ScrollbarMarkInput const&) const noexcept = default;
};

/// One tick of the track, as scrollbarMarks() hands it out.
struct ScrollbarTick
{
    int pixel {};              ///< Offset down the track, in [0, trackPixels).
    ScrollbarMarkKind kind {}; ///< The highest-priority kind among the marks on this pixel.
    int64_t targetStableId {}; ///< That mark's target (@see ScrollbarMarkInput::targetStableId).

    [[nodiscard]] bool operator==(ScrollbarTick const&) const noexcept = default;
};

/// Whether a mark of some kind keeps an overlay scrollbar shown at the bottom of the scrollback.
enum class OverlayHold : uint8_t
{
    Quiet = 0,  ///< The bar hides at the bottom whether or not the mark is above the view.
    KeepsShown, ///< While the mark is above the view the bar stays shown, so it is noticed.
};

/// Whether any mark that holds an overlay scrollbar is above the view (@see marksAbove).
enum class MarksAbove : uint8_t
{
    None = 0, ///< No held mark is above the view.
    Some,     ///< At least one is.
};

/// How many MarksAbove values there are, derived from the last one.
inline constexpr size_t MarksAboveCount = static_cast<size_t>(MarksAbove::Some) + 1;

/// Everything that sets one kind of tick apart from the others.
struct ScrollbarMarkKindRow
{
    /// The kind the row describes.
    ScrollbarMarkKind kind {};

    /// Its name, as std::format prints it.
    std::string_view name;

    /// The `scrollbar.marks` source whose token switches it on. No default: no source is a zero value.
    ScrollbarMarkSource source;

    /// The outcome of the command blocks it stands for, whose `block_status` colour it is drawn in; none for
    /// a mark the user placed, which stands for no block.
    std::optional<CommandBlockOutcome> outcome;

    /// Whether a mark of this kind above the view keeps an overlay scrollbar shown. A finished command leaves
    /// a mark after every prompt, so only the kinds worth a look hold the bar. No default, so a row that
    /// forgets it does not compile.
    OverlayHold overlay;

    /// How a tick of this kind is drawn. No default, so a row that forgets it does not compile.
    ScrollbarTickStyle style;
};

/// One row per kind, in enumerator order.
///
/// The one place a kind is described: the formatter, scrollbarMarkSourceOf(), scrollbarMarkKindOf(),
/// scrollbarMarkColor() and ScrollbarMarkKindList all read it, so a fifth kind is an enumerator and a row.
constexpr inline auto ScrollbarMarkKindTable = std::array {
    ScrollbarMarkKindRow { .kind = ScrollbarMarkKind::Command,
                           .name = "Command",
                           .source = ScrollbarMarkSource::Commands,
                           .outcome = CommandBlockOutcome::Success,
                           .overlay = OverlayHold::Quiet,
                           .style = ScrollbarTickStyle::Solid },
    ScrollbarMarkKindRow { .kind = ScrollbarMarkKind::UserMark,
                           .name = "UserMark",
                           .source = ScrollbarMarkSource::UserMarks,
                           .outcome = std::nullopt,
                           .overlay = OverlayHold::KeepsShown,
                           .style = ScrollbarTickStyle::Solid },
    ScrollbarMarkKindRow { .kind = ScrollbarMarkKind::SearchMatch,
                           .name = "SearchMatch",
                           .source = ScrollbarMarkSource::SearchMatches,
                           .outcome = std::nullopt,
                           .overlay = OverlayHold::Quiet,
                           .style = ScrollbarTickStyle::Solid },
    ScrollbarMarkKindRow { .kind = ScrollbarMarkKind::Running,
                           .name = "Running",
                           .source = ScrollbarMarkSource::Running,
                           .outcome = CommandBlockOutcome::Running,
                           .overlay = OverlayHold::KeepsShown,
                           .style = ScrollbarTickStyle::Hollow },
    ScrollbarMarkKindRow { .kind = ScrollbarMarkKind::Failure,
                           .name = "Failure",
                           .source = ScrollbarMarkSource::Failures,
                           .outcome = CommandBlockOutcome::Failure,
                           .overlay = OverlayHold::KeepsShown,
                           .style = ScrollbarTickStyle::Solid },
};

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, ScrollbarMarkKindTable.size()),
                                  [](size_t index) {
                                      return static_cast<size_t>(ScrollbarMarkKindTable[index].kind) == index;
                                  }),
              "ScrollbarMarkKindTable is indexed by ScrollbarMarkKind");

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, CommandBlockOutcomeCount),
                                  [](size_t outcome) {
                                      return std::ranges::count(
                                                 ScrollbarMarkKindTable,
                                                 std::optional { static_cast<CommandBlockOutcome>(outcome) },
                                                 &ScrollbarMarkKindRow::outcome)
                                             == 1;
                                  }),
              "every command block outcome is drawn as exactly one kind of tick");

/// The row that describes @p kind.
/// @param kind The kind of mark.
/// @return Its row of ScrollbarMarkKindTable.
[[nodiscard]] constexpr ScrollbarMarkKindRow const& scrollbarMarkKindRow(ScrollbarMarkKind kind) noexcept
{
    return ScrollbarMarkKindTable[static_cast<size_t>(kind)];
}

/// Every kind, in enumerator order.
///
/// What a per-kind list -- the colours a frontend draws its ticks in -- is built over, so it grows with the
/// table rather than with a loop someone has to remember to extend.
constexpr inline auto ScrollbarMarkKindList = [] {
    auto kinds = std::array<ScrollbarMarkKind, ScrollbarMarkKindTable.size()> {};
    std::ranges::transform(ScrollbarMarkKindTable, kinds.begin(), &ScrollbarMarkKindRow::kind);
    return kinds;
}();

/// The `scrollbar.marks` source that shows marks of @p kind.
/// @param kind The kind of mark.
/// @return The source whose token switches it on.
[[nodiscard]] constexpr ScrollbarMarkSource scrollbarMarkSourceOf(ScrollbarMarkKind kind) noexcept
{
    return scrollbarMarkKindRow(kind).source;
}

/// The tick a command block with @p outcome draws as.
/// @param outcome The block's outcome (@see outcomeOf).
/// @return Its kind of mark.
[[nodiscard]] constexpr ScrollbarMarkKind scrollbarMarkKindOf(CommandBlockOutcome outcome) noexcept
{
    // Found for every outcome: the table's second static_assert says so.
    return std::ranges::find(
               ScrollbarMarkKindTable, std::optional { outcome }, &ScrollbarMarkKindRow::outcome)
        ->kind;
}

/// Buckets @p inputs into the ticks a scrollbar track of @p trackPixels draws.
///
/// A mark lands on pixel `visibleRow * trackPixels / visibleRowCount` -- the fraction of the track the
/// handle's top reaches once that row is the viewport's top line. Marks on one pixel merge into one tick
/// of the highest kind (@see ScrollbarMarkKind); between two of the same kind the topmost row wins, so a
/// click goes to the first of the blocks the tick stands for. Kinds @p sources leaves out are dropped
/// BEFORE merging, so a hidden failure never hides a visible command.
///
/// One pass over @p inputs and one slot per pixel: the result holds at most @p trackPixels ticks and the
/// memory is the track's, however many marks a flood of OSC 133 cycles produced.
///
/// @param inputs The marks, in any order.
/// @param visibleRowCount The rows the scrollbar spans: its scrollable rows plus the page.
/// @param trackPixels The track's height in pixels.
/// @param sources Which kinds to draw (`scrollbar.marks`).
/// @return The ticks, ascending by pixel and at most one per pixel; empty when there is no track, no row
///         or no source.
[[nodiscard]] std::vector<ScrollbarTick> scrollbarMarks(std::span<ScrollbarMarkInput const> inputs,
                                                        int64_t visibleRowCount,
                                                        int trackPixels,
                                                        ScrollbarMarkSources sources);

/// Whether a mark that holds an overlay scrollbar is above the view.
///
/// A mark counts when its kind is one @p sources draws and its row's ScrollbarMarkKindRow::overlay is
/// OverlayHold::KeepsShown: a mark the bar does not draw cannot be what keeps it on screen.
/// @param marks The marks, in any order (Terminal::scrollbarMarkInputs()).
/// @param firstVisibleRow The visible row at the top of the view, counted as ScrollbarMarkInput::visibleRow;
///                        at the bottom of the scrollback it is the scrollable row count.
/// @param sources Which kinds the bar draws (`scrollbar.marks`).
/// @return MarksAbove::Some when such a mark lies on a row before @p firstVisibleRow.
[[nodiscard]] MarksAbove marksAbove(std::span<ScrollbarMarkInput const> marks,
                                    int64_t firstVisibleRow,
                                    ScrollbarMarkSources sources) noexcept;

/// The most the native scrollbar's track tints the pane toward black or white (its ink at the larger of the
/// light and dark alphas), as a fraction: a tick must stand off the pane and off the track over it.
inline constexpr float ScrollbarTrackMaxTint = 0.10f;

/// Whether @p color stands off @p pane, and off every track the scrollbar can draw over it (up to
/// ScrollbarTrackMaxTint toward black or white), at MinimumMarkContrast.
/// @param color The tick's colour.
/// @param pane The pane's background.
/// @return Whether a tick of @p color is visible wherever it is drawn.
[[nodiscard]] bool marksOnScrollbarTrack(RGBColor color, RGBColor pane) noexcept;

/// The colour a tick of @p kind is drawn in under @p palette.
///
/// A kind that stands for command blocks takes the scheme's `block_status` colour of its outcome (the
/// gutter's, so a block reads the same in both places); a search match takes the colour of the scheme's
/// search highlight, opaque, and the scheme's own yellow where that highlight borrows the cell's colours --
/// or, where that does not stand off the pane and the scrollbar's track at MinimumMarkContrast, the scheme's
/// bright yellow, or black or white, in that order; a
/// user mark, which stands for no block, takes the fold column's hovered text colour -- the scheme's
/// full-strength text colour unless it sets one -- apart from every outcome.
/// @param palette The session's colour palette.
/// @param kind The kind of tick.
/// @return Its colour.
[[nodiscard]] RGBColor scrollbarMarkColor(ColorPalette const& palette, ScrollbarMarkKind kind) noexcept;

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::ScrollbarMarkKind>: formatter<std::string_view>
{
    auto format(vtbackend::ScrollbarMarkKind value, auto& ctx) const
    {
        return formatter<string_view>::format(vtbackend::scrollbarMarkKindRow(value).name, ctx);
    }
};
