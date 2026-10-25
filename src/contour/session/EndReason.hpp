// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
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

/// Whether an ended pane offers to start its session again.
enum class FooterRestart : uint8_t
{
    Unavailable = 0, ///< No Restart: only Close.
    Offered,         ///< Restart and Close.
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

/// One row of the ended-pane footer table: what the footer says and whether it offers a Restart.
struct PaneFooterRow
{
    EndReason end;                      ///< The end the row describes.
    FooterRestart restart;              ///< Whether the end can be restarted, where the hosting allows it.
    std::string_view message;           ///< The footer text when there is no detail to name.
    std::string_view messageWithDetail; ///< The footer text naming the detail, at its one `{}`.
};

/// The footer table, one row per EndReason in enumerator order.
inline constexpr auto PaneFooterRows = std::array {
    PaneFooterRow { .end = EndReason::None,
                    .restart = FooterRestart::Unavailable,
                    .message = "",
                    .messageWithDetail = "" },
    PaneFooterRow { .end = EndReason::Exited,
                    .restart = FooterRestart::Offered,
                    .message = "Process exited.",
                    .messageWithDetail = "Process exited with code {}." },
    PaneFooterRow { .end = EndReason::Signalled,
                    .restart = FooterRestart::Offered,
                    .message = "Process ended.",
                    .messageWithDetail = "Process ended by signal {}." },
    PaneFooterRow { .end = EndReason::StartFailed,
                    .restart = FooterRestart::Offered,
                    .message = "The terminal could not be started.",
                    .messageWithDetail = "The terminal could not be started: {}." },
    PaneFooterRow { .end = EndReason::Disconnected,
                    .restart = FooterRestart::Unavailable,
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

/// Whether an ended pane offers Restart.
/// @param end  How the session ended.
/// @param host Whether its hosting can start it again here.
/// @return The row's offer, or Unavailable when the hosting cannot restart.
[[nodiscard]] constexpr FooterRestart restartOf(EndReason end, RestartSupport host) noexcept
{
    if (host == RestartSupport::Unsupported)
        return FooterRestart::Unavailable;
    return paneFooterRowOf(end).restart;
}

} // namespace contour::session
