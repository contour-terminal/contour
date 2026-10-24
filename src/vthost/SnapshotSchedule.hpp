// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// When a workspace snapshot is written: the pure decision behind WorkspacePersistence's timers.
///
/// One rule shapes the table: a pane whose shell was killed may be a casualty of the machine going down
/// (the system stops every shell before the daemon), so for SchedulePolicy::killGrace after a kill nothing
/// that would drop it from the file is written, and a shutdown inside that grace keeps the file as it was.

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace vthost
{

/// What asks for a snapshot to be written.
enum class SnapshotTrigger : std::uint8_t
{
    Periodic = 0,           ///< The periodic timer, or a deferred deadline, fired.
    LayoutChanged,          ///< A tab, split, ratio, title or colour changed.
    SessionExited,          ///< A pane's program ended on its own.
    SessionClosedByRequest, ///< A client closed a pane.
    SessionKilled,          ///< A signal ended a pane's program.
    Shutdown,               ///< The daemon is about to end.
};

/// What to do about a trigger.
enum class WriteAction : std::uint8_t
{
    Skip = 0, ///< Write nothing.
    WriteNow, ///< Capture and write now.
    Defer,    ///< Ask again at WriteDecision::at.
};

/// The schedule's timings.
struct SchedulePolicy
{
    std::chrono::seconds killGrace { 5 };      ///< How long after a kill nothing drops a pane from the file.
    std::chrono::seconds layoutDebounce { 1 }; ///< How long a layout change waits for the next one.
};

/// Whether the workspace changed since the last write.
enum class SnapshotState : std::uint8_t
{
    Clean = 0,
    Dirty,
};

/// What the schedule remembers between triggers.
struct ScheduleState
{
    SnapshotState snapshot = SnapshotState::Clean;
    std::optional<core::platform::SteadyTimePoint> lastKill {}; ///< When a session was last killed.
};

/// The answer to one trigger.
struct WriteDecision
{
    WriteAction action = WriteAction::Skip;
    core::platform::SteadyTimePoint at {}; ///< For Defer: when to ask again.

    bool operator==(WriteDecision const&) const = default;
};

namespace detail
{
    /// @return When the kill grace ends, while it has not ended at @p now; else nullopt.
    [[nodiscard]] constexpr std::optional<core::platform::SteadyTimePoint> killGraceEnd(
        ScheduleState const& state,
        core::platform::SteadyTimePoint now,
        SchedulePolicy const& policy) noexcept
    {
        if (!state.lastKill || now - *state.lastKill >= policy.killGrace)
            return std::nullopt;
        return *state.lastKill + policy.killGrace;
    }

    /// @return Write now, or once the kill grace ends.
    [[nodiscard]] constexpr WriteDecision writeOutsideKillGrace(ScheduleState const& state,
                                                                core::platform::SteadyTimePoint now,
                                                                SchedulePolicy const& policy) noexcept
    {
        if (auto const end = killGraceEnd(state, now, policy))
            return WriteDecision { .action = WriteAction::Defer, .at = *end };
        return WriteDecision { .action = WriteAction::WriteNow };
    }
} // namespace detail

/// Decides what @p trigger does.
/// @param trigger What asks.
/// @param state Whether the workspace changed, and when a session was last killed (a SessionKilled trigger's
///        own kill already recorded or not: it is deferred by the full grace either way).
/// @param now The loop's time.
/// @param policy The timings.
/// @return Skip, WriteNow, or Defer with the time to ask again (as Periodic).
[[nodiscard]] constexpr WriteDecision decideSnapshotWrite(SnapshotTrigger trigger,
                                                          ScheduleState const& state,
                                                          core::platform::SteadyTimePoint now,
                                                          SchedulePolicy const& policy) noexcept
{
    switch (trigger)
    {
        case SnapshotTrigger::Periodic:
            if (state.snapshot == SnapshotState::Clean)
                return WriteDecision {};
            return detail::writeOutsideKillGrace(state, now, policy);
        case SnapshotTrigger::LayoutChanged: {
            auto const settled = now + policy.layoutDebounce;
            auto const graceEnd = detail::killGraceEnd(state, now, policy).value_or(settled);
            return WriteDecision { .action = WriteAction::Defer, .at = std::max(settled, graceEnd) };
        }
        case SnapshotTrigger::SessionExited:
        case SnapshotTrigger::SessionClosedByRequest:
            return detail::writeOutsideKillGrace(state, now, policy);
        case SnapshotTrigger::SessionKilled:
            return WriteDecision { .action = WriteAction::Defer, .at = now + policy.killGrace };
        case SnapshotTrigger::Shutdown:
            // The shells were killed moments ago: the machine is going down, and the file keeps them.
            if (detail::killGraceEnd(state, now, policy))
                return WriteDecision {};
            return WriteDecision { .action = WriteAction::WriteNow };
    }
    return WriteDecision {};
}

} // namespace vthost
