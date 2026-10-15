// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The actions that change the state of the terminal a session HOSTS, and where each one runs.
///
/// A pane attached to a daemon shows a mirror of a terminal the daemon runs: its grid is rebuilt from
/// the daemon's deltas, so an action that only changed the local terminal would be undone, or worse
/// silently kept apart from the session it claims to act on, by the next delta. Such an action has to
/// be performed where the session lives, and the mirror then follows like it follows any output.
///
/// Two tables below say what happens: which action is which operation, and where each operation runs on
/// each kind of remote end. An operation a vi key asks for rather than an action -- the user marks
/// (TerminalSession::userMarkRequested) -- has no row in the first. Adding one is a row in each, an
/// enumerator here, and on the daemon's side the wire enumerator it travels as
/// (vthost::proto::EngineOperation, which contour::remote::toWireOperation pins to this one) and its row in
/// the host's own table (vthost::SessionHost).

#include <contour/config/Actions.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace contour::session
{

/// An operation on a session's terminal engine that a remote end can be asked to perform.
enum class HostedOperation : uint8_t
{
    ClearToPrompt = 0,        ///< Drop every row above the current prompt (actions::ClearToPrompt).
    ClearHistoryAndReset = 1, ///< Hard reset, dropping the scrollback (actions::ClearHistoryAndReset).
    SoftReset = 2,            ///< DECSTR (actions::SoftReset).
    WriteScreen = 3,          ///< Parse text as if the application wrote it (actions::WriteScreen).
    SetUserMark = 4,          ///< Place the user mark (vi `mm`) on a row's logical line.
    ClearUserMark = 5,        ///< Take the user mark off a row's logical line.
};

/// A row of the session's own terminal, as its primary grid names it: a stable id, which names that row only
/// within the grid's stable-id generation (vtbackend::Grid::stableIdGeneration) it was taken in.
struct HostedRow
{
    int64_t stableId = 0;    ///< The row's stable id.
    uint64_t generation = 0; ///< The stable-id generation @ref stableId was taken in.

    bool operator==(HostedRow const&) const = default;
};

/// One request for a hosted operation.
struct HostedOperationRequest
{
    HostedOperation operation = HostedOperation::ClearToPrompt; ///< What to perform.
    std::string text {}; ///< What WriteScreen writes; empty for every other operation.
    /// The row SetUserMark and ClearUserMark act on, which the remote end names in its own grid; unset for
    /// every other operation.
    std::optional<HostedRow> row {};

    bool operator==(HostedOperationRequest const&) const = default;
};

/// Where a hosted operation runs, as the session's factory decides it.
enum class HostedOperationRoute : uint8_t
{
    RunHere = 0, ///< On the local terminal: the session is local, or its remote end mirrors what it parses.
    RunOnHost,   ///< Sent to the host, which performs it; the local mirror follows through its deltas.
    Refused,     ///< The remote end has no way to perform it, and performing it here would not stick.
    Unreachable, ///< It runs on the remote end, and the connection to that end is gone.
};

/// The kind of remote end a session's terminal belongs to.
enum class RemoteHostKind : uint8_t
{
    Daemon = 0, ///< A Contour daemon: the local terminal is a mirror rebuilt from its deltas.
    Tmux = 1,   ///< A tmux server: the local terminal parses the pane's output itself.
};

/// One operation: how a notice names it, where it runs on each kind of remote end, and the tmux command that
/// performs its server half.
struct HostedOperationRow
{
    HostedOperation operation;                ///< The operation.
    std::string_view description;             ///< A lower-case verb phrase for a notice.
    std::array<HostedOperationRoute, 2> runs; ///< Where it runs, indexed by RemoteHostKind.
    /// The tmux command that performs the operation's server half on a pane (the pane is its `-t` target),
    /// or empty where tmux has none.
    /// @see contour::remote::tmuxOperationCommand
    std::string_view tmuxCommand {};
};

/// Every hosted operation, one row each.
///
/// A daemon performs each of them, because its client's terminal is only a mirror. A tmux tab's terminal
/// parses the pane's output, so an operation that changes no more than that terminal runs there as it does
/// in a local tab; ClearHistoryAndReset also clears the server's history with `clear-history`, as iTerm2's
/// tmux integration clears a buffer. ClearToPrompt is refused: tmux has no command for it, and dropping rows
/// locally would leave the server's pane, which a re-attach replays, as it was.
///
/// The user marks are vi `mm` on a daemon's mirror, which holds no mark of its own: the daemon's row carries
/// it, so every client of the session shows it and the daemon's reflow keeps it. A tmux tab's terminal is no
/// mirror and keeps its marks, as a local tab does.
constexpr auto HostedOperationTable = std::array {
    HostedOperationRow { .operation = HostedOperation::ClearToPrompt,
                         .description = "clear to the prompt",
                         .runs = { HostedOperationRoute::RunOnHost, HostedOperationRoute::Refused } },
    HostedOperationRow { .operation = HostedOperation::ClearHistoryAndReset,
                         .description = "clear the history and reset",
                         .runs = { HostedOperationRoute::RunOnHost, HostedOperationRoute::RunHere },
                         .tmuxCommand = "clear-history" },
    HostedOperationRow { .operation = HostedOperation::SoftReset,
                         .description = "soft reset",
                         .runs = { HostedOperationRoute::RunOnHost, HostedOperationRoute::RunHere } },
    HostedOperationRow { .operation = HostedOperation::WriteScreen,
                         .description = "write to the screen",
                         .runs = { HostedOperationRoute::RunOnHost, HostedOperationRoute::RunHere } },
    HostedOperationRow { .operation = HostedOperation::SetUserMark,
                         .description = "mark the line",
                         .runs = { HostedOperationRoute::RunOnHost, HostedOperationRoute::RunHere } },
    HostedOperationRow { .operation = HostedOperation::ClearUserMark,
                         .description = "unmark the line",
                         .runs = { HostedOperationRoute::RunOnHost, HostedOperationRoute::RunHere } },
};

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: HostedOperationTable)
            if (static_cast<std::size_t>(row.operation) != index++)
                return false;
        return true;
    }(),
    "HostedOperationTable needs one row per HostedOperation, in enumerator order");

/// @param operation The operation.
/// @return Its row; every enumerator has one.
[[nodiscard]] constexpr HostedOperationRow const& rowOf(HostedOperation operation) noexcept
{
    return HostedOperationTable.at(static_cast<std::size_t>(operation));
}

/// @param operation The operation.
/// @return What @p operation does, as a lower-case verb phrase for a notice.
[[nodiscard]] constexpr std::string_view describe(HostedOperation operation) noexcept
{
    return rowOf(operation).description;
}

/// @param operation The operation.
/// @param host The kind of remote end the session's terminal belongs to.
/// @return Where @p operation runs for a session on @p host, while the connection to it is up.
[[nodiscard]] constexpr HostedOperationRoute routeOf(HostedOperation operation, RemoteHostKind host) noexcept
{
    return rowOf(operation).runs.at(static_cast<std::size_t>(host));
}

/// One row of the table: an action, and the hosted operation it is.
/// @tparam Action The action type.
/// @tparam RowOperation The operation it maps to.
template <typename Action, HostedOperation RowOperation>
struct HostedActionRow
{
    using ActionType = Action;                                 ///< The action type.
    static constexpr HostedOperation Operation = RowOperation; ///< The operation it maps to.
};

/// Every action that changes hosted engine state, one row each.
using HostedActionTable =
    std::tuple<HostedActionRow<actions::ClearToPrompt, HostedOperation::ClearToPrompt>,
               HostedActionRow<actions::ClearHistoryAndReset, HostedOperation::ClearHistoryAndReset>,
               HostedActionRow<actions::SoftReset, HostedOperation::SoftReset>,
               HostedActionRow<actions::WriteScreen, HostedOperation::WriteScreen>>;

/// @tparam Action An action type.
/// @return The hosted operation @p Action is, or std::nullopt for an action the table does not name.
template <typename Action>
[[nodiscard]] constexpr std::optional<HostedOperation> hostedOperationOf() noexcept
{
    return []<typename... Rows>(std::type_identity<std::tuple<Rows...>>) {
        auto found = std::optional<HostedOperation> {};
        ((std::same_as<Action, typename Rows::ActionType> ? void(found = Rows::Operation) : void()), ...);
        return found;
    }(std::type_identity<HostedActionTable> {});
}

/// The request that performs @p action on a session's host.
/// @param action An action the table names.
/// @return The request, carrying the action's text where it has one.
template <typename Action>
    requires(hostedOperationOf<Action>().has_value())
[[nodiscard]] HostedOperationRequest hostedOperationRequest(Action const& action)
{
    auto request = HostedOperationRequest { .operation = *hostedOperationOf<Action>() };
    if constexpr (requires { action.chars; })
        request.text = action.chars;
    return request;
}

} // namespace contour::session
