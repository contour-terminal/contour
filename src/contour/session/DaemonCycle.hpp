// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which daemon the CycleDaemon action moves a window to. A pure decision over the listed daemons, so the
/// ring's order and its wrap-around are table-tested without a daemon or a window.

#include <contour/config/Actions.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <span>
#include <string>

namespace contour::session
{

/// The daemon to show after (or before) @p current in @p ring; stepping past either end wraps. A @p current
/// that is not in the ring (a daemon no longer listed) steps to the first entry going Next and to the last
/// going Previous.
///
/// @param ring The daemons to step through, in order.
/// @param current The daemon the window is on now.
/// @param direction Which way to step.
/// @return The daemon target, with no workspace named; none when the ring holds nothing but @p current.
[[nodiscard]] inline std::optional<SwitchTarget> nextDaemon(std::span<SwitchTarget const> ring,
                                                            SwitchTarget const& current,
                                                            actions::CycleDirection direction)
{
    auto const isCurrent = [&](SwitchTarget const& entry) {
        return entry.kind == current.kind && entry.daemon == current.daemon;
    };
    if (ring.empty())
        return std::nullopt;
    auto const found = std::ranges::find_if(ring, isCurrent);

    auto const forward = direction == actions::CycleDirection::Next;
    auto position = size_t {};
    if (found == ring.end())
        position = forward ? 0 : ring.size() - 1;
    else
    {
        auto const at = static_cast<size_t>(std::distance(ring.begin(), found));
        position = forward ? (at + 1) % ring.size() : (at + ring.size() - 1) % ring.size();
    }

    if (isCurrent(ring[position]))
        return std::nullopt;
    auto next = ring[position];
    next.workspace = {};
    return next;
}

} // namespace contour::session
