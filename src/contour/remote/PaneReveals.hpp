// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which of the panes a daemon revealed (RevealPane) a window still raises and focuses: the decision, apart
/// from the windows that carry it out, so that it is tested without any.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace contour::remote
{

/// How long a revealed pane may wait for the layout that brings it before the reveal is dropped: a reveal
/// carried out later than this would steal the focus from whatever the user does by then.
inline constexpr auto RevealPatience = std::chrono::seconds { 2 };

/// How many looks (at a layout, or at a later reveal) a reveal waits for its pane before it is dropped.
inline constexpr int RevealLooks = 3;

/// A pane the daemon revealed, not shown here yet.
struct PendingReveal
{
    uint64_t session = 0;                       ///< The daemon's id of its session.
    core::platform::SteadyTimePoint arrived {}; ///< When the reveal arrived, on the controller's clock.
    int looksLeft = RevealLooks;                ///< How many more looks before it is dropped.

    bool operator==(PendingReveal const&) const = default;
};

/// Looks at every pending reveal once: one whose pane @p reveal finds is carried out; one that is older than
/// RevealPatience, or out of looks, is dropped; the rest wait.
/// @param pending The reveals that wait, oldest first.
/// @param now The time on the clock the reveals were stamped with.
/// @param reveal Raises and focuses the pane of a daemon session; false when no window here shows it yet.
/// @return The reveals that still wait, oldest first.
template <typename Reveal>
[[nodiscard]] std::vector<PendingReveal> followReveals(std::vector<PendingReveal> const& pending,
                                                       core::platform::SteadyTimePoint now,
                                                       Reveal const& reveal)
{
    auto waiting = std::vector<PendingReveal> {};
    for (auto entry: pending)
    {
        if (now - entry.arrived > RevealPatience)
            continue;
        if (reveal(entry.session))
            continue;
        if (--entry.looksLeft >= 0)
            waiting.push_back(entry);
    }
    return waiting;
}

} // namespace contour::remote
