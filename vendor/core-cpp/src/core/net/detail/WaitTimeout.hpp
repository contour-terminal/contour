// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Pure conversion from the timeout an @c IoBackend is given to the one its native
/// wait takes.
///
/// Three backends want milliseconds as an `int` (poll(2), `epoll_wait`,
/// `WaitForMultipleObjects`) and kqueue wants a `timespec`; all four want the same
/// three answers for the same three inputs, and the interesting one is not obvious.
/// Free of every platform header so it is unit-tested on every OS.

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace core::net::detail
{

/// Converts a backend timeout into the millisecond count poll(2), `epoll_wait` and
/// `WaitForMultipleObjects` all take.
///
/// A positive duration under a millisecond is rounded UP to 1 rather than truncated
/// to 0. Truncation turns a wait into a poll, and a loop whose next deadline is
/// 400µs away then spins at 100% CPU until the deadline crosses — a busy-wait that
/// costs a core and shows up as nothing else.
/// @param timeout How long to wait, or nullopt for an indefinite wait.
/// @return -1 for an indefinite wait, 0 for a zero duration (a poll), else the wait
///         in milliseconds, at least 1 and capped at `INT_MAX`.
[[nodiscard]] constexpr int toTimeoutMillis(std::optional<platform::SteadyDuration> timeout) noexcept
{
    if (!timeout.has_value())
        return -1;
    if (*timeout <= platform::SteadyDuration::zero())
        return 0;
    auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(*timeout).count();
    return static_cast<int>(std::clamp<std::int64_t>(millis, 1, std::numeric_limits<int>::max()));
}

} // namespace core::net::detail
