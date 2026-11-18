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
/// The decision is pure — no clock, no socket — so every rule is table-testable.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

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

} // namespace vthost
