// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Why the daemon refused a MoveNode, apart from the move itself (NodeMove.hpp): what a client reads off a
/// MoveNodeRefused needs this vocabulary and nothing of the model or the wire.

#include <cstdint>
#include <string_view>

namespace vthost
{

/// Why a MoveNode changed nothing.
enum class NodeMoveError : uint8_t
{
    UnknownSession = 0, ///< No pane hosts a session the request names: it closed, or never existed.
    UnknownKind,        ///< A source or target kind this host does not know, or a pane source not naming one
                        ///< session.
    UnknownSide,        ///< An edge target's side this host does not know.
    StaleTab,           ///< The source tab no longer holds exactly the sessions the request named.
    Model,              ///< The model refused; NodeMoveFailure::model says why.
};

/// @param error The refusal.
/// @return A lower-case phrase for a log line.
[[nodiscard]] constexpr std::string_view describe(NodeMoveError error) noexcept
{
    switch (error)
    {
        case NodeMoveError::UnknownSession: return "unknown session";
        case NodeMoveError::UnknownKind: return "unknown source or target kind";
        case NodeMoveError::UnknownSide: return "unknown side";
        case NodeMoveError::StaleTab: return "the tab no longer holds the sessions the request named";
        case NodeMoveError::Model: return "the model refused the move";
    }
    return "unknown error";
}

} // namespace vthost
