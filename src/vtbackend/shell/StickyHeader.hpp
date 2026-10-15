// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/StickyHeaderKind.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace vtbackend
{

/// When the sticky command header may cover the top row (profile key `sticky_header.mode`).
enum class StickyHeaderMode : uint8_t
{
    Never = 0, ///< No header -- except the evicted-output placeholder at the very top of the scrollback.
    Scrolled,  ///< Only while the viewport is scrolled back into history -- the default.
    Always,    ///< Also while following live output, so a long build keeps its command in view.
};

/// Whether the evicted-output placeholder may stand in for a block whose head has left the scrollback
/// (profile key `sticky_header.show_evicted`).
enum class EvictedPlaceholder : uint8_t
{
    Hidden = 0, ///< Such a block shows no header at all.
    /// At the very top of the scrollback whatever the mode says; anywhere else wherever the mode would show
    /// the command header -- the default.
    Shown,
};

/// The sticky header's configuration, as the terminal consumes it.
struct StickyHeaderSettings
{
    StickyHeaderMode mode = StickyHeaderMode::Scrolled; ///< When the command header may appear.
    /// Whether the evicted-output placeholder may stand in for an evicted head.
    EvictedPlaceholder evictedPlaceholder = EvictedPlaceholder::Shown;

    bool operator==(StickyHeaderSettings const&) const = default;
};

/// Where the head of the viewport's top-row block is, relative to the viewport.
enum class StickyHeadPosition : uint8_t
{
    Visible = 0, ///< On screen -- the top row IS the head -- so a header would only repeat it.
    Above,       ///< Above the viewport, and still in the scrollback.
    Evicted,     ///< The command's own row is gone from the scrollback; the block's record outlives it.
};

/// Which part of its block the viewport's top row is.
enum class TopRowPart : uint8_t
{
    Input = 0, ///< The prompt, or the command typed at it: already on screen, so no header.
    Output,    ///< What the command printed.
};

/// Whether the viewport follows live output or has been scrolled back.
enum class ViewportScroll : uint8_t
{
    Live = 0, ///< At the bottom, following output.
    Scrolled, ///< Scrolled back into history.
};

/// Whether the viewport's top row is the very top of the scrollback: the oldest row it still holds.
///
/// A row of the live page never is, even with no scrollback kept and nothing older than it: that is the
/// page, not the top of a scrollback.
enum class TopRowAge : uint8_t
{
    Newer = 0, ///< Older rows are still there, or the row is on the live page.
    Oldest,    ///< A scrollback row with nothing older surviving: the viewport is at the very top.
};

/// Whether the cursor drawn stands on the viewport's top row: the row a header would cover.
enum class TopRowCursor : uint8_t
{
    Elsewhere = 0, ///< On another row: the vi-mode cursor in vi mode, the terminal's own outside it.
    ViCursor,      ///< The vi-mode cursor is on it: `gg` put it there, say, and a header would hide it.
    /// The terminal's own cursor is on it, outside vi mode: the application is writing that row -- a
    /// `clear`-and-redraw loop, say -- and a header would hide what it writes.
    TextCursor,
};

/// The block the viewport's top row belongs to, as much of it as the decision reads.
struct StickyHeaderBlockFacts
{
    CommandBlockId id {};                                   ///< The block.
    CommandBlockState state = CommandBlockState::Prompting; ///< Where it is in its cycle.
    StickyHeadPosition head = StickyHeadPosition::Visible;  ///< Where its head is.
    TopRowPart topRow = TopRowPart::Input;                  ///< What part of it the top row is.

    [[nodiscard]] bool operator==(StickyHeaderBlockFacts const&) const noexcept = default;
};

/// Everything stickyHeaderFor() reads, gathered by the terminal once per frame.
struct StickyHeaderInput
{
    StickyHeaderSettings settings {};               ///< The profile's configuration.
    ScreenType screen = ScreenType::Primary;        ///< The screen of the DISPLAYED page.
    ViewportScroll viewport = ViewportScroll::Live; ///< Following output, or scrolled back.
    TopRowAge topRowAge = TopRowAge::Newer;         ///< Whether the top row is the oldest one kept.
    TopRowCursor cursor = TopRowCursor::Elsewhere;  ///< Whether the cursor drawn is on the top row.

    /// The top row's block; nullopt when it belongs to none -- no shell integration, or a record the
    /// store's bound has already dropped.
    std::optional<StickyHeaderBlockFacts> block {};
};

/// What the sticky header shows on this frame.
struct StickyHeaderDecision
{
    StickyHeaderKind kind = StickyHeaderKind::Command; ///< Which overlay.
    CommandBlockId block {};                           ///< Whose -- what a press on it jumps to.

    [[nodiscard]] bool operator==(StickyHeaderDecision const&) const noexcept = default;
};

/// Decides whether the top row is covered by the sticky command header, the evicted-output placeholder,
/// or nothing.
///
/// Pure: the terminal gathers the input in O(1) per frame, so the whole policy is this function and its
/// table tests.
///
/// @param input What the viewport shows and what the profile allows.
/// @return The overlay to draw, or nullopt for none.
[[nodiscard]] std::optional<StickyHeaderDecision> stickyHeaderFor(StickyHeaderInput const& input) noexcept;

/// Whether any block at all could get a header from stickyHeaderFor() at this viewport position: what the
/// terminal asks before it looks the top row's block up, so a frame no block can change pays for no lookup.
///
/// Exact: false exactly when stickyHeaderFor() answers nullopt for every block it could be handed while no
/// cursor is on the top row (with one there, it answers a header for no more blocks). Not a
/// test of following live output -- a collapsed fold can make the live page's top row the oldest one kept,
/// where the evicted placeholder shows whatever the mode.
/// @param settings The profile's configuration.
/// @param screen The screen of the displayed page.
/// @param viewport Following output, or scrolled back.
/// @param topRowAge Whether the top row is the oldest one kept.
/// @return Whether a header is possible here.
[[nodiscard]] bool stickyHeaderPossible(StickyHeaderSettings const& settings,
                                        ScreenType screen,
                                        ViewportScroll viewport,
                                        TopRowAge topRowAge) noexcept;

/// Blank columns kept right of the sticky header's chip, so its last glyph ends left of the overlay
/// scrollbar's 12-logical-pixel strip (SessionChrome.qml) instead of under its thumb. Enough for any cell
/// at least 6 px wide.
constexpr inline auto StickyHeaderChipInset = ColumnCount(2);

/// The status chip the sticky header right-aligns: `running 12s`, `✓ 3s`, `✗ 2 1m 04s`, `✗ SIGKILL 3s`.
///
/// A finished block's mark is the one its colour tells (@see outcomeOf): `✓`, or `✗` followed by what
/// ended it -- the signal that killed it, else its exit code (the one a shell would have reported for an
/// OSC 3008 verdict) when that is not zero -- as the gutter tooltip and the finish notification name it.
///
/// A running block's time is @p now less its wall-clock start, the only start a record keeps; once the
/// block finishes, the record's steady duration takes over. Either reads as every other surface prints a
/// duration (@see formatCommandDuration), so a wall clock stepped backwards reads `0ms`.
/// @param record The block's record.
/// @param now The injected wall clock's time (TerminalClocks::wall) -- never system_clock::now().
/// @return The chip text; empty for a block still at its prompt, which has run nothing to name.
[[nodiscard]] std::u32string stickyHeaderChip(CommandBlockRecord const& record,
                                              std::chrono::system_clock::time_point now);

/// The evicted-output placeholder for a block whose command line is @p commandLine.
///
/// Built on every frame it shows, so only as much of the command line as @p budget columns can show is
/// read: the raw line is cut first, to four bytes a column -- the most one column's codepoint takes -- and
/// only then sanitised, rather than sanitising up to MaxRecordedCommandLineBytes to show one row of it.
/// @param commandLine The record's command line; untrusted, so it is read through its display form.
/// @param budget The columns the text may take; what lies beyond them may be cut.
/// @return `⋯ <command> — earlier output evicted`, or `⋯ earlier output evicted` without a command.
[[nodiscard]] std::u32string evictedPlaceholderText(CommandLineText const& commandLine, ColumnCount budget);

} // namespace vtbackend
