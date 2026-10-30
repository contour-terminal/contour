// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Putting captured history (WorkspaceCapture.hpp) back into a fresh terminal, so that a restored pane
/// shows what the pane it replaces had shown, scrolled up behind a divider that says so.

#include <vtbackend/screen/Terminal.hpp>

#include <chrono>
#include <string>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// What a restored pane's divider says: when the session it replaces ended, and which day it is now.
struct RestoreDivider
{
    std::chrono::local_seconds sessionEndedAt {}; ///< The snapshot's `takenAt`, in the daemon's local time.
    std::chrono::local_days today {};             ///< The daemon's current local day.

    /// @return true where both fields are equal.
    [[nodiscard]] bool operator==(RestoreDivider const&) const = default;
};

/// @param divider What the divider is about.
/// @return `Restored · session ended hh:mm` when the session ended on @p divider's `today`, otherwise
///         `Restored · session ended YYYY-MM-DD hh:mm`.
[[nodiscard]] std::string restoredDividerText(RestoreDivider const& divider);

/// The divider's row, as plain text: the words of restoredDividerText() centred between `─` rules, one space
/// either side, the left rule taking the floor of the leftover cells. Every glyph in it is one cell wide.
/// @param divider What the divider is about.
/// @param columns The pane's width.
/// @return The row, @p columns cells wide; the text alone where @p columns is less than its width plus 2.
[[nodiscard]] std::string restoredDivider(RestoreDivider const& divider, vtbackend::ColumnCount columns);

/// Lands @p history in @p terminal's primary history, oldest row first, leaves the page blank with the
/// cursor at its top-left, adopts the command-block records the rows name, then writes the divider (see
/// restoredDivider()) in SGR dim on the page's top row, cut to the terminal's width and marked not
/// wrappable so that a later resize cuts it rather than wrapping it onto a second row. The cursor ends on
/// the row below. Call it before the terminal's pump starts: it takes the terminal lock itself and assumes
/// nothing else is writing.
/// @param terminal A fresh terminal showing its primary screen.
/// @param history What capturePaneHistory() returned; empty for a pane that restores no rows.
/// @param divider What the divider under the restored rows says.
void seedRestoredHistory(vtbackend::Terminal& terminal,
                         proto::Delta const& history,
                         RestoreDivider const& divider);

} // namespace vthost
