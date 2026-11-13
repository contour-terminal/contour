// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The vocabulary of status kinds the tab strip, sidebar and taskbar share. Dependency-free, so the urgency
/// order (StatusUrgency.hpp) and the tables that are checked against it can both include it.

#include <cstddef>
#include <cstdint>

namespace contour::window
{

/// What a tab's program-status badge shows (OSC 7501).
enum class BadgeKind : uint8_t
{
    None = 0, ///< Nothing: no record, or none worth showing.
    Working,  ///< A spinner, with the progress when the program gave one.
    Blocked,  ///< `!`: the program waits for the user.
    Done,     ///< `✓`: finished, with results the user has not seen.
    Error,    ///< `✕`: failed.
};

/// Whether the user has seen what a status record reports.
enum class StatusSeen : uint8_t
{
    Unseen = 0, ///< The record is newer than the last one the user saw.
    Seen,       ///< The user has seen this record, or a later one.
};

/// The badge kinds a tab can show. The enumerators keep their order (TabBadgeLooks is indexed by them); what
/// decides between them is TabBadgeRules, ordered by urgency (see StatusUrgency.hpp). Done and Error show
/// only until the user has seen them (BadgeVisibility::WhileUnseen).
enum class TabBadgeKind : uint8_t
{
    None = 0, ///< No badge.
    Output,   ///< Unseen output.
    Done,     ///< Finished well.
    Working,  ///< Running, with progress when known.
    Error,    ///< Failed.
    Blocked,  ///< Waits for the user.
    Bell,     ///< The bell rang while the tab was out of sight.
};

/// Number of @c TabBadgeKind enumerators, derived from the last one.
inline constexpr size_t TabBadgeKindCount = static_cast<size_t>(TabBadgeKind::Bell) + 1;

} // namespace contour::window
