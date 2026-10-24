// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a client's EditTab does to the daemon's model: one row per TabEditKind, so a ninth kind is a row
/// and an enumerator. Pure over the model, so every row is testable without a socket.

#include <cstdint>
#include <expected>
#include <string_view>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vthost
{

/// Why an EditTab changed nothing.
enum class TabEditError : uint8_t
{
    UnknownSession = 1, ///< No pane hosts the session it names: it closed, or never existed.
    UnknownKind = 2,    ///< The kind is not one this host knows, as from a newer client.
};

/// @param error The refusal.
/// @return A lower-case phrase for a log line.
[[nodiscard]] constexpr std::string_view describe(TabEditError error) noexcept
{
    switch (error)
    {
        case TabEditError::UnknownSession: return "unknown session";
        case TabEditError::UnknownKind: return "unknown kind of tab edit";
    }
    return "unknown error";
}

/// Whether an applied edit moved the model.
enum class TabEditEffect : uint8_t
{
    Unchanged = 0, ///< The model held the value already; nothing was mutated, no event fired.
    Changed = 1,   ///< The model took the value, and its observers were told.
};

/// Applies @p edit to @p model. Each kind compares before it mutates, so a client re-asserting state it
/// shares with the model costs no event, and so no layout push to any client.
/// @param model The daemon's model.
/// @param edit The client's edit.
/// @return Whether the model moved, or why the edit was refused.
[[nodiscard]] std::expected<TabEditEffect, TabEditError> applyTabEdit(vtworkspace::SessionModel& model,
                                                                      proto::EditTab const& edit);

} // namespace vthost
