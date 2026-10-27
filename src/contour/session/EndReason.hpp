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
    None = 0, ///< No primary button: only the dismissal.
    Restart,  ///< Start the ended session again in the same pane.
    RunAgain, ///< Ask to type the command a restart stopped at the shell's prompt, and run it.
};

/// Number of @c FooterAction enumerators, derived from the last one.
inline constexpr std::size_t FooterActionCount = static_cast<std::size_t>(FooterAction::RunAgain) + 1;

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
};

/// The footer table, one row per EndReason in enumerator order.
inline constexpr auto PaneFooterRows = std::array {
    PaneFooterRow { .end = EndReason::None,
                    .action = FooterAction::None,
                    .actionLabel = "",
                    .dismiss = FooterDismiss::Close,
                    .message = "",
                    .messageWithDetail = "" },
    PaneFooterRow { .end = EndReason::Exited,
                    .action = FooterAction::Restart,
                    .actionLabel = "Restart",
                    .dismiss = FooterDismiss::Close,
                    .message = "Process exited.",
                    .messageWithDetail = "Process exited with code {}." },
    PaneFooterRow { .end = EndReason::Signalled,
                    .action = FooterAction::Restart,
                    .actionLabel = "Restart",
                    .dismiss = FooterDismiss::Close,
                    .message = "Process ended.",
                    .messageWithDetail = "Process ended by signal {}." },
    PaneFooterRow { .end = EndReason::StartFailed,
                    .action = FooterAction::Restart,
                    .actionLabel = "Restart",
                    .dismiss = FooterDismiss::Close,
                    .message = "The terminal could not be started.",
                    .messageWithDetail = "The terminal could not be started: {}." },
    PaneFooterRow { .end = EndReason::Disconnected,
                    .action = FooterAction::None,
                    .actionLabel = "",
                    .dismiss = FooterDismiss::Close,
                    .message = "Disconnected from the Contour daemon. Its sessions are still running; "
                               "reattach with: contour client",
                    .messageWithDetail = "Disconnected from the Contour daemon. Its sessions are still "
                                         "running; reattach with: contour client" },
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
/// @return The row's detail form when @p detail is non-empty, else its plain message.
[[nodiscard]] inline std::string paneFooterMessage(EndReason end, std::string_view detail)
{
    auto const& row = paneFooterRowOf(end);
    if (detail.empty())
        return std::string { row.message };
    // The row's text is data, not a literal, so it cannot be a std::format_string.
    return std::vformat(row.messageWithDetail, std::make_format_args(detail));
}

/// The primary button an ended pane offers.
/// @param end  How the session ended.
/// @param host Whether its hosting can start it again here.
/// @return The row's action, or None when the hosting cannot restart.
[[nodiscard]] constexpr FooterAction offeredAction(EndReason end, RestartSupport host) noexcept
{
    auto const& row = paneFooterRowOf(end);
    if (row.action == FooterAction::Restart && host == RestartSupport::Unsupported)
        return FooterAction::None;
    return row.action;
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
