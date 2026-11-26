// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// When the native session flushes its pending screen increment: at once, or after a coalescing delay.
///
/// Output that answers a keystroke (echo) and the first output after a snapshot flush at once, so
/// typing feels local; everything else coalesces. The echo window is 50 ms, wider than a few ms, to
/// catch echo from shells with heavy prompt hooks and from SSH on a LAN; what keeps bulk output
/// coalescing is the one-immediate-flush-per-input credit (`answeredInput`), not the window's width.
///
/// An echo is held back, for at most `redrawGrace`, while the program has the cursor hidden: that is a redraw
/// in progress. PSReadLine hides the cursor, redraws the line and shows it again for every key, and ConPTY
/// hands the hide over in a read of its own; flushed then, the window drew the cursor gone for a coalescing
/// window on every key. The credit stays unspent, so the update that shows the cursor again flushes at once,
/// with the whole redraw; a program that keeps it hidden is answered within the grace.
///
/// Beside it, ActivityMark paces how often a pane's output is marked as activity.
///
/// The decision is pure — no clock, no socket — so every rule is table-testable.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace vthost
{

/// Tuning of the flush decision.
struct FlushPolicy
{
    /// How long after an input its output still counts as the echo of that input.
    core::platform::SteadyDuration echoWindow = std::chrono::milliseconds { 50 };
    /// How long bulk output is held back to coalesce into one increment.
    core::platform::SteadyDuration coalesceDelay = std::chrono::milliseconds { 20 };
    /// How long an echo waits for the program to show the cursor it hid to redraw. ConPTY hands a redraw over
    /// within about a millisecond; a program that keeps the cursor hidden pays this, not `coalesceDelay`.
    core::platform::SteadyDuration redrawGrace = std::chrono::milliseconds { 5 };
    /// How often a connection that mirrors the echo state asks each followed session's PTY for it when no
    /// output or input has: a program may turn echo off after its prompt, or back on without printing, and
    /// nothing else would say so. Coarse on purpose: it bounds how late a lock appears, not a frame.
    core::platform::SteadyDuration inputEchoCheck = std::chrono::milliseconds { 250 };
    /// How long after the connection attached, a followed session's output, or an input, the idle check
    /// keeps itself armed. Past it the connection is quiet and wakes nothing; the next activity re-arms it.
    core::platform::SteadyDuration inputEchoWindow = std::chrono::seconds { 2 };
    /// The least time between two output marks of a pane the connection does not mirror: a pane printing
    /// without pause costs its window one small PDU per interval, not one per flush.
    core::platform::SteadyDuration activityMarkInterval = std::chrono::seconds { 1 };
};

/// Why a flush is scheduled.
enum class FlushReason : uint8_t
{
    Coalesce = 0,    ///< Bulk output: wait `coalesceDelay`.
    Echo = 1,        ///< Output answering a recent, not yet answered input: flush now.
    FirstOutput = 2, ///< The first increment after a snapshot: flush now.
    AwaitRedraw = 3, ///< An echo while the cursor is hidden: wait `redrawGrace` for the redraw to end.
};

/// Whether an increment has been sent since the session's last snapshot.
enum class IncrementHistory : uint8_t
{
    NoneSinceSnapshot = 0,
    SomeSinceSnapshot = 1,
};

/// Whether the program shows the cursor (DECTCEM) as the update is flushed.
enum class CursorVisibility : uint8_t
{
    Visible = 0,
    Hidden = 1,
};

/// The facts the flush decision looks at.
struct FlushInputs
{
    /// The current time.
    core::platform::SteadyTimePoint now;
    /// When the session last received input, if ever.
    std::optional<core::platform::SteadyTimePoint> lastInput;
    /// The input stamp this connection has already spent its immediate flush on, if any.
    std::optional<core::platform::SteadyTimePoint> answeredInput;
    /// Whether an increment went out since the last snapshot.
    IncrementHistory increments = IncrementHistory::NoneSinceSnapshot;
    /// Whether the program shows the cursor now.
    CursorVisibility cursor = CursorVisibility::Visible;
};

/// Decides why (and thereby how soon) to flush.
/// @param policy The echo window and coalescing delay.
/// @param inputs The facts to decide on.
/// @return Echo if a recent input is still unanswered (AwaitRedraw while the cursor is hidden), else
///         FirstOutput if no increment has been sent since the snapshot, else Coalesce.
[[nodiscard]] constexpr FlushReason decideFlush(FlushPolicy const& policy, FlushInputs const& inputs) noexcept
{
    if (inputs.lastInput && inputs.now - *inputs.lastInput <= policy.echoWindow
        && inputs.answeredInput != inputs.lastInput)
        return inputs.cursor == CursorVisibility::Visible ? FlushReason::Echo : FlushReason::AwaitRedraw;
    if (inputs.increments == IncrementHistory::NoneSinceSnapshot)
        return FlushReason::FirstOutput;
    return FlushReason::Coalesce;
}

/// The time at which a flush for the given reason is due.
/// @param policy The coalescing delay and the redraw grace.
/// @param reason Why the flush is scheduled.
/// @param now The current time.
/// @return `now` for an immediate reason, `now + redrawGrace` to await a redraw, `now + coalesceDelay` to
///         coalesce.
[[nodiscard]] constexpr core::platform::SteadyTimePoint flushDeadline(
    FlushPolicy const& policy, FlushReason reason, core::platform::SteadyTimePoint now) noexcept
{
    switch (reason)
    {
        case FlushReason::Coalesce: return now + policy.coalesceDelay;
        case FlushReason::AwaitRedraw: return now + policy.redrawGrace;
        case FlushReason::Echo:
        case FlushReason::FirstOutput: return now;
    }
    return now;
}

/// Whether a pane's output reached its main display since the last look.
enum class OutputReach : uint8_t
{
    StatusLineOnly = 0, ///< Only the host-writable status line was written: nothing the user would see.
    MainDisplay,        ///< The main display was written to.
};

/// Whether a pane's output may be marked now.
enum class MarkPace : uint8_t
{
    TooSoon = 0, ///< One was marked less than the interval before.
    Marked,      ///< It is marked as of now.
};

/// Whether a pane holds output the pace kept from being marked.
enum class HeldOutput : uint8_t
{
    None = 0, ///< Every output was marked, or seen since.
    Pending,  ///< Output came too soon after the last mark: a trailing mark owes it.
};

/// How a pane's output was last marked: what tells news from a status-line rewrite, and what paces the next
/// mark. Kept per pane by the ledger's host and by each connection that summarizes panes it does not mirror.
struct ActivityMark
{
    /// When its output was last marked, which paces the next mark; none before the first.
    std::optional<core::platform::SteadyTimePoint> markedAt = std::nullopt;
    /// Its terminal's mainDisplayOutputCount() at the last look.
    uint64_t mainDisplayOutput = 0;
    /// Whether output came since the last mark that the pace held back.
    HeldOutput held = HeldOutput::None;

    /// Takes the main display's output count now.
    /// @param count The terminal's mainDisplayOutputCount() now.
    /// @return MainDisplay when it moved since the last look; StatusLineOnly when it did not.
    [[nodiscard]] constexpr OutputReach look(uint64_t count) noexcept
    {
        return std::exchange(mainDisplayOutput, count) == count ? OutputReach::StatusLineOnly
                                                                : OutputReach::MainDisplay;
    }

    /// Marks the output at @p now, unless the last mark is less than @p interval old: then the output is
    /// held, for a trailing mark to make once the interval is up (trailingDue()).
    /// @param now The current time.
    /// @param interval The least time between two marks.
    /// @return Marked, or TooSoon (and the output is held).
    [[nodiscard]] constexpr MarkPace mark(core::platform::SteadyTimePoint now,
                                          core::platform::SteadyDuration interval) noexcept
    {
        if (markedAt && now - *markedAt < interval)
        {
            held = HeldOutput::Pending;
            return MarkPace::TooSoon;
        }
        markedAt = now;
        held = HeldOutput::None;
        return MarkPace::Marked;
    }

    /// @param interval The least time between two marks.
    /// @return When the held output is due its trailing mark, or std::nullopt when none is held.
    [[nodiscard]] constexpr std::optional<core::platform::SteadyTimePoint> trailingDue(
        core::platform::SteadyDuration interval) const noexcept
    {
        if (held == HeldOutput::None || !markedAt)
            return std::nullopt;
        return *markedAt + interval;
    }
};

} // namespace vthost
