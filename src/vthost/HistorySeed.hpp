// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Putting captured history (WorkspaceCapture.hpp) back into a fresh terminal, so that a restored pane
/// shows what the pane it replaces had shown, scrolled up behind a marker that says so.

#include <vtbackend/screen/Terminal.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Lands @p history in @p terminal's primary history, oldest row first, leaves the page blank with the
/// cursor at its top-left, adopts the command-block records the rows name, then writes @p marker through
/// the parser. Rows wider than the terminal are cut, not wrapped. Call it before the terminal's pump starts:
/// it takes the terminal lock itself and assumes nothing else is writing.
/// @param terminal A fresh terminal showing its primary screen.
/// @param history What capturePaneHistory() returned.
/// @param marker What to write under the restored rows, as the parser reads it (see restoredMarker()).
void seedRestoredHistory(vtbackend::Terminal& terminal, proto::Delta const& history, std::string_view marker);

/// @param takenAt When the snapshot was taken, in local time.
/// @return `ESC[2m── restored · snapshot of YYYY-MM-DD HH:MM ──ESC[m` and a line break: the dim marker
///         written under the restored rows.
[[nodiscard]] std::string restoredMarker(std::chrono::local_seconds takenAt);

} // namespace vthost
