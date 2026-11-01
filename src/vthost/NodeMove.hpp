// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a client's MoveNode does to the daemon's model: the vtworkspace move a GUI window does in-process,
/// applied to the model every attached client mirrors. Pure over the model, so each refusal is testable
/// without a socket.

#include <expected>
#include <functional>
#include <optional>

#include <vthost/NodeMoveError.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Move.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vthost
{

/// A refused MoveNode: the host's reason, and the model's when the model is what refused.
struct NodeMoveFailure
{
    NodeMoveError error = NodeMoveError::UnknownSession; ///< Why nothing changed.
    std::optional<vtworkspace::MoveError> model = {};    ///< Set exactly when @c error is Model.
};

/// How applyMoveNode makes the window a NewWindow target lands in, and removes a window a move leaves empty:
/// the host's own path (SessionHost::createEmptyWindow, discardWindow), which names and claims the window and
/// forgets both again, so that no window of the host's is ever unnamed and none a move emptied lingers. Both
/// are required.
struct MoveWindowHooks
{
    /// Makes an empty window.
    std::function<vtworkspace::WindowId()> create;
    /// Removes a window that holds no tab: one made by @c create that the move did not fill, or the window
    /// the move took its last tab out of.
    std::function<void(vtworkspace::WindowId)> discard;
};

/// Moves the pane or tab @p move names to the target it names. No session is spawned or closed; the
/// model's observers are told, so every client is pushed the new layout -- the window the move emptied, if
/// any, among it with no tab -- and that window is then removed (@p windows).
/// @param model The daemon's model.
/// @param move The client's request.
/// @param windows Makes the window of a NewWindow target, and removes a window the move empties.
/// @return Where focus landed, or why nothing changed.
[[nodiscard]] std::expected<vtworkspace::MoveResult, NodeMoveFailure> applyMoveNode(
    vtworkspace::SessionModel& model, proto::MoveNode const& move, MoveWindowHooks const& windows);

} // namespace vthost
