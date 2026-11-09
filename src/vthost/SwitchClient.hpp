// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which attached client a workspace switch acts on. A pure decision, so every row of the table is testable
/// without a socket or a session.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <vthost/ConnectionId.hpp>

namespace vthost
{

/// How long `workspace.switch` waits for the window's SwitchReply before it gives up.
inline constexpr auto SwitchAnswerTimeout = std::chrono::seconds { 30 };

/// Why no client could be chosen to switch.
enum class SwitchClientError : uint8_t
{
    NoClient = 0, ///< No client shows the workspace.
    Ambiguous,    ///< Several do, and none of them is known to have typed last.
};

/// What chooseSwitchClient() decided.
struct SwitchClientChoice
{
    std::expected<ConnectionId, SwitchClientError> chosen; ///< The client to switch, or why none.
    std::vector<ConnectionId> candidates;                  ///< The clients that were considered.
};

/// The client a switch acts on: the last typist among those showing the workspace, else the only one.
/// @param showing The clients that show the workspace.
/// @param lastTypist The connection that typed last into the workspace; nullopt for none known.
/// @return The choice, with the candidates it considered.
[[nodiscard]] inline SwitchClientChoice chooseSwitchClient(std::span<ConnectionId const> showing,
                                                           std::optional<ConnectionId> const& lastTypist)
{
    auto choice =
        SwitchClientChoice { .chosen = std::unexpected(SwitchClientError::NoClient),
                             .candidates = std::vector<ConnectionId>(showing.begin(), showing.end()) };
    if (showing.empty())
        return choice;
    if (lastTypist)
    {
        for (auto const& client: showing)
            if (client == *lastTypist)
            {
                choice.chosen = client;
                return choice;
            }
    }
    if (showing.size() == 1)
        choice.chosen = showing.front();
    else
        choice.chosen = std::unexpected(SwitchClientError::Ambiguous);
    return choice;
}

} // namespace vthost
