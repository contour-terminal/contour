// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/FinishNotification.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/window/HostChipState.hpp>

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#include <vthost/client/ConnectionState.hpp>

namespace contour::window
{

/// What a pane shows over and around its terminal.
enum class PaneState : uint8_t
{
    Live = 0,     ///< The session runs; the pane is the terminal alone.
    Reconnecting, ///< The daemon connection is being sought again; the grid is frozen and dimmed.
    Ended,        ///< The session ended and the pane waits for Restart or Close in its footer.
};

/// Number of @c PaneState enumerators, derived from the last one.
inline constexpr size_t PaneStateCount = static_cast<size_t>(PaneState::Ended) + 1;

/// Decides a pane's state from its session's end and its host connection.
/// @param end        How the pane's session ended; None while it runs.
/// @param connection The state of the daemon connection the session is mirrored over (Connected for a
///                   local one).
/// @return Ended once the session ended, whatever the connection; else Reconnecting while the connection
///         is being (re)established, folded as the window's host chip folds it (hostChipStateOf), so the
///         pane's pill and the chip never disagree; else Live.
[[nodiscard]] constexpr PaneState paneStateOf(session::EndReason end,
                                              vthost::client::ConnectionState connection) noexcept
{
    if (end != session::EndReason::None)
        return PaneState::Ended;
    if (hostChipStateOf(connection) == HostChipState::Reconnecting)
        return PaneState::Reconnecting;
    return PaneState::Live;
}

/// Whether a pane's footer is up, and which.
enum class PaneFooterKind : uint8_t
{
    None = 0,    ///< No footer.
    Ended,       ///< The session ended and the pane waits for the footer's answer (an EndReason's row).
    Interrupted, ///< The shell runs, over the command a restart stopped (InterruptedFooterRow).
};

/// Number of @c PaneFooterKind enumerators, derived from the last one.
inline constexpr size_t PaneFooterKindCount = static_cast<size_t>(PaneFooterKind::Interrupted) + 1;

/// Whether a restored pane's shell is still over the command a restart stopped.
enum class InterruptionPresence : uint8_t
{
    Absent = 0, ///< Nothing was stopped, or the pane has started a command of its own since.
    Present,    ///< The command is waiting for the user's answer.
};

/// Whether a session's shell has shown a prompt (an unfinished command block).
enum class ShellPromptPresence : uint8_t
{
    Absent = 0, ///< No prompt mark has arrived: typed text would not land at a prompt.
    Present,    ///< The shell reported a prompt.
};

/// Decides which footer a pane shows.
/// @param end          How the pane's session ended; None while it runs.
/// @param interruption Whether a restart's stopped command is still waiting.
/// @return Ended once the session ended, whatever else; else Interrupted while the command waits; else None.
[[nodiscard]] constexpr PaneFooterKind paneFooterOf(session::EndReason end,
                                                    InterruptionPresence interruption) noexcept
{
    if (end != session::EndReason::None)
        return PaneFooterKind::Ended;
    return interruption == InterruptionPresence::Present ? PaneFooterKind::Interrupted : PaneFooterKind::None;
}

/// The primary button of the Interrupted footer.
/// @param prompt Whether the shell has shown a prompt yet.
/// @return RunAgain once it has; None before, as there is no prompt to type the command at and the button
///         would do nothing.
[[nodiscard]] constexpr session::FooterAction interruptionActionOf(ShellPromptPresence prompt) noexcept
{
    return prompt == ShellPromptPresence::Present ? session::InterruptedFooterRow.action
                                                  : session::FooterAction::None;
}

/// The Interrupted footer's text.
/// @param commandLine The command line the snapshot recorded: untrusted, whatever wrote to the terminal.
/// @return The row's message naming the line's display form (control characters and bidirectional overrides
///         made visible), cut to platform::MaxCommandNameBytes.
[[nodiscard]] inline std::string interruptedFooterMessage(vtbackend::CommandLineText const& commandLine)
{
    auto const shown = vtbackend::truncateUtf8(commandLine.display(), platform::MaxCommandNameBytes);
    // The row's text is data, not a literal, so it cannot be a std::format_string.
    return std::vformat(session::InterruptedFooterRow.message, std::make_format_args(shown));
}

} // namespace contour::window
