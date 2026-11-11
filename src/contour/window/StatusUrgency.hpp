// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one order in which statuses compete for attention. The tab strip, the sidebar, the taskbar and the
/// workspace chip all rank by it, so a surface never disagrees with another about what matters more.
///
/// The order, most urgent first: NeedsYou, Failed, DoneUnseen, Working, UnseenOutput, Idle.
///   - A bell is an explicit request for attention, so it is NeedsYou, level with a program that is blocked.
///   - A failure comes next, then a finished job whose result has not been seen: both are news the user
///     must act on or read, while something still running needs nothing yet.
///   - Unseen output sits just above idle: it is only a hint that something moved.
///   - Whatever the user has seen (a seen Done, a seen Error) is Idle: it no longer asks for anything.
/// The tables TabBadgeRules and BadgeRows list their rows in this order; the static_asserts below keep them
/// so.

#include <contour/window/StatusKinds.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>

namespace contour::window
{

/// How urgently a status asks for the user. The ordinal is the rank: larger is more urgent.
enum class Urgency : uint8_t
{
    Idle = 0,     ///< Nothing to show.
    UnseenOutput, ///< Output arrived out of sight.
    Working,      ///< Something runs.
    DoneUnseen,   ///< Something finished and its result has not been seen.
    Failed,       ///< Something failed and it has not been seen.
    NeedsYou,     ///< The user is asked for: a blocked program or a bell.
};

/// The urgency of a tab badge kind.
/// @param kind The badge kind.
/// @return Its urgency; Bell maps to NeedsYou, being an explicit request for attention.
[[nodiscard]] constexpr Urgency urgencyOf(TabBadgeKind kind) noexcept
{
    switch (kind)
    {
        case TabBadgeKind::None: return Urgency::Idle;
        case TabBadgeKind::Output: return Urgency::UnseenOutput;
        case TabBadgeKind::Working: return Urgency::Working;
        case TabBadgeKind::Done: return Urgency::DoneUnseen;
        case TabBadgeKind::Error: return Urgency::Failed;
        case TabBadgeKind::Blocked:
        case TabBadgeKind::Bell: return Urgency::NeedsYou;
    }
    return Urgency::Idle;
}

/// The urgency of a program status badge.
/// @param kind The badge kind.
/// @param seen Whether the user has seen the record; a seen Done or Error asks for nothing.
/// @return Its urgency.
[[nodiscard]] constexpr Urgency urgencyOf(BadgeKind kind, StatusSeen seen) noexcept
{
    switch (kind)
    {
        case BadgeKind::None: return Urgency::Idle;
        case BadgeKind::Working: return Urgency::Working;
        case BadgeKind::Blocked: return Urgency::NeedsYou;
        case BadgeKind::Done: return seen == StatusSeen::Unseen ? Urgency::DoneUnseen : Urgency::Idle;
        case BadgeKind::Error: return seen == StatusSeen::Unseen ? Urgency::Failed : Urgency::Idle;
    }
    return Urgency::Idle;
}

/// The most urgent of @p urgencies.
/// @param urgencies The statuses of the parts of a whole (the panes of a tab, the tabs of a workspace).
/// @return The maximum; Idle for none.
[[nodiscard]] constexpr Urgency rollUp(std::span<Urgency const> urgencies) noexcept
{
    auto result = Urgency::Idle;
    for (auto const urgency: urgencies)
        result = std::max(result, urgency);
    return result;
}

} // namespace contour::window
