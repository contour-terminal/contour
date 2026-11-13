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

/// How applyMoveNode makes the workspace a NewWorkspace target lands in, and removes a workspace a move
/// leaves empty: the host's own path (SessionHost::createEmptyWorkspace, discardWorkspace), which names and
/// claims the workspace and forgets both again, so that no workspace of the host's is ever unnamed and none a
/// move emptied lingers. Both are required.
struct MoveWorkspaceHooks
{
    /// Makes an empty workspace.
    std::function<vtworkspace::WorkspaceId()> create;
    /// Removes a workspace that holds no tab: one made by @c create that the move did not fill, or the
    /// workspace the move took its last tab out of.
    std::function<void(vtworkspace::WorkspaceId)> discard;
};

/// Moves the pane or tab @p move names to the target it names. No session is spawned or closed; the
/// model's observers are told, so every client is pushed the new layout -- the workspace the move emptied, if
/// any, among it with no tab -- and that workspace is then removed (@p workspaces).
/// @param model The daemon's model.
/// @param move The client's request.
/// @param workspaces Makes the workspace of a NewWorkspace target, and removes a workspace the move empties.
/// @return Where focus landed, or why nothing changed.
[[nodiscard]] std::expected<vtworkspace::MoveResult, NodeMoveFailure> applyMoveNode(
    vtworkspace::SessionModel& model, proto::MoveNode const& move, MoveWorkspaceHooks const& workspaces);

/// Moves @p source to @p target in the host's model: what applyMoveNode() does once it has read the request,
/// and what the control API's `pane.move` does with what it resolved. A NewWorkspace target lands in a
/// workspace made by @p workspaces, and a workspace the move empties is removed by it.
/// @param model The daemon's model.
/// @param source What moves.
/// @param target Where it goes.
/// @param workspaces Makes the workspace of a NewWorkspace target, and removes a workspace the move empties.
/// @return Where focus landed, or why nothing changed.
[[nodiscard]] std::expected<vtworkspace::MoveResult, NodeMoveFailure> applyMove(
    vtworkspace::SessionModel& model,
    vtworkspace::MoveSource const& source,
    vtworkspace::MoveTarget const& target,
    MoveWorkspaceHooks const& workspaces);

} // namespace vthost
