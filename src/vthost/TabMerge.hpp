// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a client's MergeTabIntoSplit does to the daemon's model: the same vtworkspace merge a GUI window
/// does in-process, applied to the model every attached client mirrors. Pure over the model, so each
/// refusal is testable without a socket.

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vthost
{

/// Why a MergeTabIntoSplit changed nothing.
enum class TabMergeError : uint8_t
{
    UnknownSession = 1, ///< No pane hosts a session it names: it closed, or never existed.
    UnknownSide = 2,    ///< The side is not one this host knows, as from a newer client.
    Refused = 3,        ///< The model refused: both sessions are in the same tab.
};

/// @param error The refusal.
/// @return A lower-case phrase for a log line.
[[nodiscard]] constexpr std::string_view describe(TabMergeError error) noexcept
{
    switch (error)
    {
        case TabMergeError::UnknownSession: return "unknown session";
        case TabMergeError::UnknownSide: return "unknown side";
        case TabMergeError::Refused: return "the model refused the merge";
    }
    return "unknown error";
}

/// Reads a wire side, never casting it.
/// @param side A vtworkspace::SplitSide as the wire spells it.
/// @return The side, or nullopt for a byte no enumerator has.
[[nodiscard]] std::optional<vtworkspace::SplitSide> splitSideFrom(uint8_t side) noexcept;

/// Grafts the tab holding @p merge's tab session beside the pane hosting its target session. No session is
/// spawned or closed; the model's observers are told, so every client is pushed one new layout.
/// @param model The daemon's model.
/// @param merge The client's request.
/// @return Success, or why nothing changed.
[[nodiscard]] std::expected<void, TabMergeError> applyTabMerge(vtworkspace::SessionModel& model,
                                                               proto::MergeTabIntoSplit const& merge);

} // namespace vthost
