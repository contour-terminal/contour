// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/ProgramStatusBadge.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
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

/// What one window waits for, as the application-wide count reads it.
struct WindowWaiting
{
    uint64_t window = 0;  ///< The window.
    unsigned waiting = 0; ///< How many of its tabs wait.
    /// The daemon the window shows, when it follows one: windows on the same daemon see the same tabs, so
    /// that daemon counts once. Absent for an in-process window, whose tabs are its own.
    std::optional<uint64_t> daemon {};
    uint64_t focusOrder = 0; ///< Larger is more recently focused; picks which window speaks for its daemon.
};

/// The application-wide count of waiting tabs: each in-process window's own, plus each daemon's once, as its
/// most recently focused window counts it.
/// @param windows Every window.
/// @return The total.
[[nodiscard]] inline unsigned applicationWaiting(std::span<WindowWaiting const> windows)
{
    auto total = 0u;
    auto speaking = std::vector<WindowWaiting const*> {};
    for (auto const& entry: windows)
    {
        if (!entry.daemon)
        {
            total += entry.waiting;
            continue;
        }
        auto const known = std::ranges::find(
            speaking, entry.daemon, [](WindowWaiting const* other) { return other->daemon; });
        if (known == speaking.end())
            speaking.push_back(&entry);
        else if (entry.focusOrder > (*known)->focusOrder)
            *known = &entry;
    }
    for (auto const* entry: speaking)
        total += entry->waiting;
    return total;
}

/// Counts the tabs of @p directory that wait for the user, as tallyWaitingTabs() counts a window's own: a tab
/// waits when its panes' program status earns it the Blocked badge, which shows whatever the user has seen.
/// @param directory Every workspace a daemon window can list.
/// @param except The workspace not to count -- the one the window displays, whose tabs it counts itself;
///        none to count every one.
/// @return How many tabs wait.
[[nodiscard]] inline unsigned tabsWaitingIn(WorkspaceDirectory const& directory,
                                            std::optional<uint64_t> except)
{
    auto waiting = 0u;
    for (auto const& workspace: directory.workspaces)
    {
        if (except == workspace.id)
            continue;
        for (auto const& tab: workspace.tabs)
        {
            auto records = std::vector<vtbackend::ProgramStatusRecord> {};
            for (auto const& pane: tab.panes)
                records.insert(records.end(), pane.status.begin(), pane.status.end());
            if (badgeFor(records, AllRecordsSeen).kind == BadgeKind::Blocked)
                ++waiting;
        }
    }
    return waiting;
}

} // namespace contour::window
