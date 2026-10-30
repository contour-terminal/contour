// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/TabBadge.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace contour::window
{

/// How many tabs wait for the user, per window and in all.
struct AttentionTally
{
    /// Each window holding a tab, in the order its first tab was listed, with its count; a window with
    /// none waiting is listed with zero, so that one which just dropped to zero is told.
    std::vector<std::pair<uint64_t, unsigned>> perWindow {};
    unsigned total = 0; ///< The count over every window.

    bool operator==(AttentionTally const&) const = default;
};

/// Counts the tabs that wait for the user: those whose resolved badge is Blocked (a tab counts while it is
/// in front too: a question waiting in the window the user just left is still waiting).
/// @param tabs Every tab of every window, as its window's id and the badge it shows.
/// @return The count per window and in all.
[[nodiscard]] inline AttentionTally tallyWaitingTabs(std::span<std::pair<uint64_t, TabBadgeKind> const> tabs)
{
    auto tally = AttentionTally {};
    for (auto const& [window, kind]: tabs)
    {
        auto entry = std::ranges::find(tally.perWindow, window, &std::pair<uint64_t, unsigned>::first);
        if (entry == tally.perWindow.end())
            entry = tally.perWindow.insert(tally.perWindow.end(), { window, 0u });
        if (kind == TabBadgeKind::Blocked)
        {
            ++entry->second;
            ++tally.total;
        }
    }
    return tally;
}

} // namespace contour::window
