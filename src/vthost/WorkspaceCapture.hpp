// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Reading a live workspace into the snapshot model: the layout as the model holds it, and per pane what
/// is needed to bring it back: its directory, profile, the program it runs, and a tail of its history.

#include <vtbackend/screen/Terminal.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <vthost/SessionSnapshot.hpp>
#include <vthost/WorkspacePersistenceConfig.hpp>

namespace vthost
{

class SessionHost;

/// What a capture keeps.
struct CaptureOptions
{
    ScrollbackPersistence scrollback = ScrollbackPersistence::Tail;
    std::size_t scrollbackRows = 1000; ///< The most rows kept per pane.
};

/// Captures the history of one pane.
///
/// The primary grid's rows above the cursor's row, the newest @p maxRows of them, trailing blank rows
/// dropped; the hyperlinks those rows use; and the records of the command blocks they name, a Running one
/// closed as Finished and Implicit (what it will be by the time the shell is back), a Prompting one left out.
/// The caller holds the terminal lock.
/// @param terminal The pane's terminal.
/// @param maxRows The most rows to keep.
/// @return A snapshot-style Delta holding the rows, hyperlinks and records.
[[nodiscard]] proto::Delta capturePaneHistory(vtbackend::Terminal const& terminal, std::size_t maxRows);

/// Captures every window, tab and pane of @p host. Takes each terminal's lock in turn.
/// @param host The host whose model is read.
/// @param options What to keep.
/// @param now When the snapshot is taken.
/// @return The workspace; windows without a tab are left out, as there is nothing in them to restore.
[[nodiscard]] WorkspaceSnapshot captureWorkspace(SessionHost& host,
                                                 CaptureOptions const& options,
                                                 std::chrono::sys_seconds now);

} // namespace vthost
