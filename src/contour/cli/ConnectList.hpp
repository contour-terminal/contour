// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The targets the Connect picker offers, as data: grouped, filtered, numbered. Qt-free, so the picker's
/// model, `contour connect --list` and the launchers' menus (the Dock menu, the jump list) all read one
/// list and the decisions are tested without a window or a daemon.

#include <contour/config/LaunchTarget.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::cli
{

/// Where a row sits in the picker.
enum class ConnectGroup : uint8_t
{
    Typed = 0,    ///< The leading "Connect to ADDRESS" row an address typed in the field adds.
    ThisComputer, ///< The local daemon, and this window only.
    SavedHosts,   ///< A host of `hosts:`.
};

/// Whether the local daemon is up.
enum class DaemonRunState : uint8_t
{
    NotRunning = 0, ///< Nothing accepts connections: connecting starts it.
    Running,        ///< A daemon accepts connections.
    Probing,        ///< Not known yet: the probe is still asking.
};

/// What the picker knows of the local daemon.
struct LocalDaemonState
{
    DaemonRunState run = DaemonRunState::NotRunning; ///< Whether it is up.
    std::optional<std::size_t> sessions {};          ///< How many sessions it hosts, when it said.
};

/// One row of the picker.
struct ConnectRow
{
    config::LaunchTarget target;      ///< What choosing it connects to.
    ConnectGroup group;               ///< Where it sits.
    std::string title;                ///< The row's name: "Persistent sessions", a host's name.
    std::string detail;               ///< The line under it: the daemon's status, a host's address.
    std::string tag;                  ///< `SSH` or `TLS` on a host; empty otherwise.
    std::string text;                 ///< The target's text, as `contour connect` reads it.
    std::optional<std::size_t> quick; ///< The digit that picks it (1 to 9); none past the ninth row.
};

/// The most rows a quick-select digit reaches.
inline constexpr std::size_t QuickSelectCount = 9;

/// The modifier that, with a digit, picks a row.
enum class QuickSelectKey : uint8_t
{
    Alt = 0, ///< Alt+1 to Alt+9, where Command is not the application's key.
    Command, ///< ⌘1 to ⌘9 on macOS.
};

/// @param key The modifier.
/// @param digit The digit, 1 to 9.
/// @return What a row shows for it: `⌘3`, `Alt+3`.
[[nodiscard]] std::string quickSelectLabel(QuickSelectKey key, std::size_t digit);

/// What the list is made of.
struct ConnectListInputs
{
    std::map<std::string, config::HostEntry> const& hosts; ///< The saved hosts; borrowed.
    LocalDaemonState daemon;                               ///< The local daemon, as probed.
};

/// The rows to show for what has been typed.
///
/// Unfiltered, it is both groups in full. A filter keeps the rows whose name, address or kind contain every
/// word of it, case-insensitively, and an `ssh://` or `tls://` address that parses adds a leading
/// "Connect to ADDRESS" row. Whatever is left is numbered 1 to 9 in order, so filtering renumbers.
/// @param inputs The saved hosts and the daemon's state.
/// @param filter What the user typed in the field.
/// @return The rows, in display order.
[[nodiscard]] std::vector<ConnectRow> connectRows(ConnectListInputs const& inputs, std::string_view filter);

/// @param rows The rows shown.
/// @param lastUsed The text of the target last connected to; empty for none.
/// @return The row the selection starts on: the last used target's, else the first.
[[nodiscard]] std::size_t initialSelection(std::vector<ConnectRow> const& rows, std::string_view lastUsed);

/// @param rows The rows shown.
/// @param digit A quick-select digit, 1 to 9.
/// @return The index of the row it picks, or none.
[[nodiscard]] std::optional<std::size_t> rowForQuickSelect(std::vector<ConnectRow> const& rows,
                                                           std::size_t digit);

/// What `contour connect --list` prints.
enum class ListFormat : uint8_t
{
    Text = 0, ///< One target per line: its text, then its detail.
    Json,     ///< A JSON array of objects.
};

/// Spells the unfiltered list.
/// @param rows The rows (connectRows() with an empty filter).
/// @param format How to write them.
/// @return The listing, newline-terminated.
[[nodiscard]] std::string renderTargetList(std::vector<ConnectRow> const& rows, ListFormat format);

/// @param state The local daemon.
/// @return What its row says: `Local daemon · running · 3 sessions`, `Local daemon · not running · starts on
///         connect`.
[[nodiscard]] std::string describeLocalDaemon(LocalDaemonState const& state, std::string_view label);

} // namespace contour::cli
