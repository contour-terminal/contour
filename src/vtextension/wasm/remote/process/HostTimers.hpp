// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The deadlines the extension host supervisor keeps: each a budget that runs, may be paused and resumed, and
/// expires once it ran out. Pure: told the time by its caller, so that it is tested with explicit instants.

#include <core/Clock.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace vtextension::wasm::remote::process
{

/// What names one timer: the helper it is about, and which of that helper's timers it is.
struct TimerKey
{
    uint64_t helper = 0; ///< The supervised helper's key.
    uint32_t serial = 0; ///< Which of its timers.

    auto operator<=>(TimerKey const&) const = default;
};

/// Deadlines of one thread (the supervisor's). Not thread-safe; every instant is its caller's.
///
/// A timer's budget runs while it is armed and not paused; a paused one keeps what is left of it and never
/// expires. The table is per supervisor and small, so every query scans it.
class HostTimers
{
  public:
    /// Arms @p key, replacing a timer armed under it.
    /// @param key The timer.
    /// @param now The instant its budget starts running.
    /// @param budget How long it may run before it expires.
    void arm(TimerKey key, core::SteadyTimePoint now, core::SteadyDuration budget);

    /// Stops @p key's budget from running, keeping what is left of it; an unknown or paused key is ignored.
    /// @param key The timer.
    /// @param now The instant it stops.
    void pause(TimerKey key, core::SteadyTimePoint now) noexcept;

    /// Lets @p key's budget run again; an unknown or running key is ignored.
    /// @param key The timer.
    /// @param now The instant it runs again.
    void resume(TimerKey key, core::SteadyTimePoint now) noexcept;

    /// Forgets @p key, for good; an unknown key is ignored.
    void disarm(TimerKey key) noexcept;

    /// Forgets every timer of @p helper.
    void disarmHelper(uint64_t helper) noexcept;

    /// Removes and returns every running timer whose budget ran out by @p now.
    /// @return Their keys, in key order.
    [[nodiscard]] std::vector<TimerKey> expire(core::SteadyTimePoint now);

    /// @return When the earliest running timer expires; nothing if none runs. Paused timers are excluded.
    [[nodiscard]] std::optional<core::SteadyTimePoint> nextDeadline() const noexcept;

    /// @return How many timers are armed, paused ones included.
    [[nodiscard]] std::size_t armed() const noexcept;

  private:
    /// One timer: what is left of its budget, and since when that runs; nothing while it is paused.
    struct Entry
    {
        core::SteadyDuration remaining {};
        std::optional<core::SteadyTimePoint> runningSince;
    };

    /// @return When @p entry expires; nothing while it is paused.
    [[nodiscard]] static std::optional<core::SteadyTimePoint> deadlineOf(Entry const& entry) noexcept;

    std::map<TimerKey, Entry> _timers;
};

} // namespace vtextension::wasm::remote::process
