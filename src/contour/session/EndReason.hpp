// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace contour::session
{

/// How a session that waits in its pane, rather than closing it, came to an end.
///
/// Only the ends a pane outlives are named here: a shell that exited too soon after starting, a device
/// that never started, and a lost daemon connection. Every other end closes the pane and is never
/// recorded. (Not `SessionEnd`: that name is TerminalSessionManager's teardown intent.)
enum class EndReason : uint8_t
{
    None = 0,     ///< The session has not ended.
    Exited,       ///< Its program exited on its own; the detail is the exit code.
    Signalled,    ///< Its program was ended by a signal; the detail is the signal.
    StartFailed,  ///< Its device could not be started; the detail is the platform's reason.
    Disconnected, ///< The daemon connection it was mirrored over went away; the shell still runs there.
};

/// What a pane footer's primary button does.
enum class FooterAction : uint8_t
{
    None = 0,  ///< No primary button: only the dismissal.
    Restart,   ///< Start the ended session again in the same pane.
    Reconnect, ///< Seek the daemon again, and take the pane's session back from it.
    RunAgain,  ///< Ask to type the command a restart stopped at the shell's prompt, and run it.
};

/// What a pane footer's dismissing button does.
enum class FooterDismiss : uint8_t
{
    Close = 0, ///< Close the pane: its session is over.
    Dismiss,   ///< Take the footer away; the pane and its live shell stay.
};

/// Whether the hosting a session runs under can start a session again in the same pane.
///
/// Converted from SessionHosting at the boundary: a local session can be respawned here, while a
/// remote-hosted one belongs to the daemon (or tmux server), which is the only one that may start it again.
enum class RestartSupport : uint8_t
{
    Unsupported = 0, ///< Remote-hosted: the remote end owns the session.
    Supported,       ///< Local: a new session can be spawned in the same pane.
};

/// One row of the ended-pane footer table: what the footer says and what its buttons do.
struct PaneFooterRow
{
    EndReason end;                      ///< The end the row describes.
    FooterAction action;                ///< The primary button, where the hosting allows it.
    std::string_view actionLabel;       ///< What it says: empty exactly when @ref action is None.
    FooterDismiss dismiss;              ///< What the dismissing button does.
    std::string_view message;           ///< The footer text when there is no detail to name.
    std::string_view messageWithDetail; ///< The footer text naming the detail, at its one `{}`.
    /// The footer text while the row's @ref action is offered and there is no detail; empty says @ref
    /// message.
    std::string_view messageWithAction;
};

/// The footer table, one row per EndReason in enumerator order.
inline constexpr auto PaneFooterRows = std::array {
    PaneFooterRow { .end = EndReason::None,
                    .action = FooterAction::None,
                    .actionLabel = "",
                    .dismiss = FooterDismiss::Close,
                    .message = "",
                    .messageWithDetail = "",
                    .messageWithAction = "" },
    PaneFooterRow { .end = EndReason::Exited,
                    .action = FooterAction::Restart,
                    .actionLabel = "Restart",
                    .dismiss = FooterDismiss::Close,
                    .message = "Process exited.",
                    .messageWithDetail = "Process exited with code {}.",
                    .messageWithAction = "" },
    PaneFooterRow { .end = EndReason::Signalled,
                    .action = FooterAction::Restart,
                    .actionLabel = "Restart",
                    .dismiss = FooterDismiss::Close,
                    .message = "Process ended.",
                    .messageWithDetail = "Process ended by signal {}.",
                    .messageWithAction = "" },
    PaneFooterRow { .end = EndReason::StartFailed,
                    .action = FooterAction::Restart,
                    .actionLabel = "Restart",
                    .dismiss = FooterDismiss::Close,
                    .message = "The terminal could not be started.",
                    .messageWithDetail = "The terminal could not be started: {}.",
                    .messageWithAction = "" },
    PaneFooterRow { .end = EndReason::Disconnected,
                    .action = FooterAction::Reconnect,
                    .actionLabel = "Reconnect",
                    .dismiss = FooterDismiss::Close,
                    .message = "Disconnected from the Contour daemon. Its sessions are still running; "
                               "reattach with: contour client",
                    .messageWithDetail = "Could not reconnect: {}.",
                    .messageWithAction =
                        "Disconnected from the Contour daemon. Its sessions are still running." },
};

/// Looks up the footer row for @p end.
/// @param end The end to describe.
/// @return Its row.
[[nodiscard]] constexpr PaneFooterRow const& paneFooterRowOf(EndReason end) noexcept
{
    return PaneFooterRows.at(static_cast<std::size_t>(end));
}

/// The footer text for @p end.
/// @param end    The end to describe.
/// @param detail What the end carries (an exit code, a signal, the failure); empty for none.
/// @param offered The primary button the footer offers (offeredAction()).
/// @return The row's detail form when @p detail is non-empty; else, while the row's action is offered and the
///         row has words for it, those; else its plain message.
[[nodiscard]] inline std::string paneFooterMessage(EndReason end,
                                                   std::string_view detail,
                                                   FooterAction offered)
{
    auto const& row = paneFooterRowOf(end);
    if (detail.empty())
    {
        auto const offeredWords =
            offered != FooterAction::None && offered == row.action && !row.messageWithAction.empty();
        return std::string { offeredWords ? row.messageWithAction : row.message };
    }
    // The row's text is data, not a literal, so it cannot be a std::format_string.
    return std::vformat(row.messageWithDetail, std::make_format_args(detail));
}

/// Whether the host application can seek a lost daemon connection again from an ended pane.
enum class ReconnectSupport : uint8_t
{
    Unsupported =
        0,     ///< No reconnect: only a local session, or a tmux window whose gateway has no supervisor.
    Supported, ///< The app is attached to a daemon and can start a new supervised run.
};

/// The primary button an ended pane offers.
/// @param end       How the session ended.
/// @param restart   Whether its hosting can start it again here.
/// @param reconnect Whether the app can seek the daemon again.
/// @return The row's action, or None when the hosting or the app cannot do it.
[[nodiscard]] constexpr FooterAction offeredAction(EndReason end,
                                                   RestartSupport restart,
                                                   ReconnectSupport reconnect) noexcept
{
    auto const& row = paneFooterRowOf(end);
    switch (row.action)
    {
        case FooterAction::Restart:
            return restart == RestartSupport::Supported ? row.action : FooterAction::None;
        case FooterAction::Reconnect:
            return reconnect == ReconnectSupport::Supported ? row.action : FooterAction::None;
        case FooterAction::None:
        case FooterAction::RunAgain: break;
    }
    return FooterAction::None;
}

/// What the footer over a restored shell says and does. Not an EndReason's row: nothing ended, the pane's
/// shell runs, and what the footer names is the command the restart stopped.
struct InterruptionFooterRow
{
    std::string_view message;     ///< The footer text, at its one `{}` for the command.
    FooterAction action;          ///< The primary button, once the shell is at a prompt.
    std::string_view actionLabel; ///< What it says.
    FooterDismiss dismiss;        ///< What the dismissing button does.
};

/// The footer of a pane whose command a restart stopped.
inline constexpr auto InterruptedFooterRow =
    InterruptionFooterRow { .message = "“{}” was stopped when the session ended.",
                            .action = FooterAction::RunAgain,
                            .actionLabel = "Run Again",
                            .dismiss = FooterDismiss::Dismiss };

/// The label of a dismissing button.
/// @param dismiss What it does.
/// @return "Close" or "Dismiss".
[[nodiscard]] constexpr std::string_view dismissLabelOf(FooterDismiss dismiss) noexcept
{
    switch (dismiss)
    {
        case FooterDismiss::Close: return "Close";
        case FooterDismiss::Dismiss: return "Dismiss";
    }
    return "";
}

} // namespace contour::session
