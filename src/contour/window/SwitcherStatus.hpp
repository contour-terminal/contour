// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the switcher shows of a workspace's status: the urgency it ranks by, the glyph that draws it, the git
/// text, the reason and when it happened -- the same vocabulary the sidebar speaks, so a workspace looks the
/// same in the sidebar, the switcher and the workspace chip.
///
/// The window's own daemon's workspaces take it from the sidebar's workspace rows, which were judged against
/// this window's seen-state. Another daemon's come from its `workspace.list`: that window never saw those
/// panes and the daemon keeps no seen-state, so a pane's records and its last finished command are judged by
/// one table (RemoteStatusRules), a done or failed status counting as unseen for as long as it stands (the
/// owner's ruling). Pure and Qt-free.

#include <contour/window/SidebarTree.hpp>
#include <contour/window/StatusKinds.hpp>
#include <contour/window/StatusUrgency.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <vtcontrol/Api.hpp>

namespace contour::window
{

/// A workspace's (or a daemon's) status, as a switcher row shows it.
struct SwitcherStatus
{
    Urgency urgency = Urgency::Idle;         ///< What it asks of the user; the rank.
    TabBadgeKind glyph = TabBadgeKind::None; ///< The glyph that draws it (TabBadgeLooks).
    std::string gitText;                     ///< `feat/auth ↑2`; empty when none.
    ReasonParts reason;                      ///< Words, then the monospace command.
    /// Own daemon: SidebarRow::time, preformatted. Other daemons: how the time is kept and the progress, the
    /// text formatted from `since` against the clock (switcherTimeText).
    TimeText time;
    std::optional<std::chrono::system_clock::time_point> since; ///< Other daemons: when it began or ended.

    bool operator==(SwitcherStatus const&) const = default;
};

/// What one pane of another daemon tells of itself, as far as its status goes.
enum class RemoteSignal : uint8_t
{
    None = 0,      ///< Nothing to say: idle, or a command that succeeded.
    Working,       ///< A program reports that it works.
    ProgramDone,   ///< A program reports that it is done.
    CommandFailed, ///< The last command exited with a status other than 0.
    ProgramError,  ///< A program reports that it failed.
    Blocked,       ///< A program waits for the user.
};

/// How one signal shows.
struct RemoteStatusRule
{
    RemoteSignal signal = RemoteSignal::None; ///< The signal the row describes.
    Urgency urgency = Urgency::Idle;          ///< What it asks of the user.
    TabBadgeKind glyph = TabBadgeKind::None;  ///< The glyph that draws it.
};

/// The rules, one per RemoteSignal in enumerator order. A done or failed status is unseen for as long as it
/// stands: this window never saw the pane, and its daemon keeps no seen-state.
inline constexpr auto RemoteStatusRules = std::array {
    RemoteStatusRule { .signal = RemoteSignal::None, .urgency = Urgency::Idle, .glyph = TabBadgeKind::None },
    RemoteStatusRule {
        .signal = RemoteSignal::Working, .urgency = Urgency::Working, .glyph = TabBadgeKind::Working },
    RemoteStatusRule {
        .signal = RemoteSignal::ProgramDone, .urgency = Urgency::DoneUnseen, .glyph = TabBadgeKind::Done },
    RemoteStatusRule {
        .signal = RemoteSignal::CommandFailed, .urgency = Urgency::Failed, .glyph = TabBadgeKind::Error },
    RemoteStatusRule {
        .signal = RemoteSignal::ProgramError, .urgency = Urgency::Failed, .glyph = TabBadgeKind::Error },
    RemoteStatusRule {
        .signal = RemoteSignal::Blocked, .urgency = Urgency::NeedsYou, .glyph = TabBadgeKind::Blocked },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, RemoteStatusRules.size()),
                                  [](std::size_t index) {
                                      return RemoteStatusRules[index].signal
                                             == static_cast<RemoteSignal>(index);
                                  }),
              "RemoteStatusRules lists every RemoteSignal once, in enumerator order");

/// @param signal A signal.
/// @return Its rule.
[[nodiscard]] constexpr RemoteStatusRule const& remoteStatusRule(RemoteSignal signal) noexcept
{
    return RemoteStatusRules[static_cast<std::size_t>(signal)];
}

/// The status of one of this window's daemon's workspaces, from its sidebar row.
/// @param workspaceRow The workspace's row (SidebarListing::workspaces).
/// @return Its urgency, glyph, git text, reason and preformatted time, as the sidebar row has them.
[[nodiscard]] SwitcherStatus switcherStatusOf(SidebarRow const& workspaceRow);

/// The status of another daemon's workspace, from its `workspace.list` entry: the most urgent of its panes'
/// program-status records and failed last commands (RemoteStatusRules), the first on a tie, with its git
/// text. The reason and the time are the sidebar's (statusReasonOf, statusTimeKind, finishedText), so a
/// workspace reads the same wherever it is listed.
/// @param workspace The workspace as listed.
/// @return Its status; `since` is when the deciding record changed or the failed command ended.
[[nodiscard]] SwitcherStatus switcherStatusOf(vtcontrol::WorkspaceInfo const& workspace);

/// The status of a whole (a daemon of its workspaces).
/// @param parts The statuses of its parts.
/// @return The most urgent part, the first among equals; the idle status for none.
[[nodiscard]] SwitcherStatus rollUpStatus(std::span<SwitcherStatus const> parts);

/// When a status happened, as a row says it.
/// @param status The status.
/// @param now The wall clock.
/// @return Its preformatted time when it has one, else its time kept as `time.kind` says from `since`
///         (formatTimeText, the sidebar's), else empty.
[[nodiscard]] std::string switcherTimeText(SwitcherStatus const& status,
                                           std::chrono::system_clock::time_point now);

/// Reads a control API timestamp: RFC 3339 in UTC, `2026-10-24T12:00:00.123Z`, the fraction optional.
/// @param text The timestamp.
/// @return The instant; nullopt for anything else (another offset, a date out of range, trailing text).
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseRfc3339Utc(std::string_view text);

} // namespace contour::window
