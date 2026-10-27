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
    /// How often a connection that mirrors the echo state asks each followed session's PTY for it when no
    /// output or input has: a program may turn echo off after its prompt, or back on without printing, and
    /// nothing else would say so. Coarse on purpose: it bounds how late a lock appears, not a frame.
    core::platform::SteadyDuration inputEchoCheck = std::chrono::milliseconds { 250 };
};

/// Why a flush is scheduled.
enum class FlushReason : uint8_t
{
    Coalesce = 0,    ///< Bulk output: wait `coalesceDelay`.
    Echo = 1,        ///< Output answering a recent, not yet answered input: flush now.
    FirstOutput = 2, ///< The first increment after a snapshot: flush now.
};

/// Whether an increment has been sent since the session's last snapshot.
enum class IncrementHistory : uint8_t
{
    NoneSinceSnapshot = 0,
    SomeSinceSnapshot = 1,
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
};

/// Decides why (and thereby how soon) to flush.
/// @param policy The echo window and coalescing delay.
/// @param inputs The facts to decide on.
/// @return Echo if a recent input is still unanswered, else FirstOutput if no increment has been
///         sent since the snapshot, else Coalesce.
[[nodiscard]] constexpr FlushReason decideFlush(FlushPolicy const& policy, FlushInputs const& inputs) noexcept
{
    if (inputs.lastInput && inputs.now - *inputs.lastInput <= policy.echoWindow
        && inputs.answeredInput != inputs.lastInput)
        return FlushReason::Echo;
    if (inputs.increments == IncrementHistory::NoneSinceSnapshot)
        return FlushReason::FirstOutput;
    return FlushReason::Coalesce;
}

/// The time at which a flush for the given reason is due.
/// @param policy The coalescing delay.
/// @param reason Why the flush is scheduled.
/// @param now The current time.
/// @return `now` for an immediate reason, `now + coalesceDelay` to coalesce.
[[nodiscard]] constexpr core::platform::SteadyTimePoint flushDeadline(
    FlushPolicy const& policy, FlushReason reason, core::platform::SteadyTimePoint now) noexcept
{
    return reason == FlushReason::Coalesce ? now + policy.coalesceDelay : now;
}

} // namespace vthost
