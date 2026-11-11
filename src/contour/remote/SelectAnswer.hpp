// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the daemon answered a window's selection request, and what a switch waiting for one makes of the
/// answers. Pure, so the rule is a table test.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string_view>

namespace contour::remote
{

/// What the daemon answered a selection request (NativeController::select()) with.
struct SelectAnswer
{
    uint64_t request = 0;  ///< The request's number, as NativeController::select() returned it.
    uint64_t asked = 0;    ///< The workspace it asked for; 0 for a new one.
    uint64_t selected = 0; ///< The workspace the connection selects as of the answer.
    bool operator==(SelectAnswer const&) const = default;
};

/// Judges a switch that waits for its selection by the answers that came. Only the answer to its own request
/// counts: an answer to an earlier request may state another workspace although it asked for the same one
/// (switches to B, then A, then B, whose first two answers the daemon gave in one batch stating A).
/// @param request The switch's selection request number.
/// @param to The workspace it switches to; 0 for one the daemon makes.
/// @param before The workspaces the connection had been pushed before the switch: a new one is not among
///        them.
/// @param answers The answers that came, oldest first.
/// @return Why the daemon did not take the selection; none when it did, or has not answered it yet.
[[nodiscard]] inline std::optional<std::string_view> switchRefusal(uint64_t request,
                                                                   uint64_t to,
                                                                   std::set<uint64_t> const& before,
                                                                   std::span<SelectAnswer const> answers)
{
    auto const own = std::ranges::find(answers, request, &SelectAnswer::request);
    if (own == answers.end())
        return std::nullopt;
    if (to != 0)
        return own->selected == to
                   ? std::nullopt
                   : std::optional { std::string_view { "the daemon has no such workspace" } };
    return !before.contains(own->selected)
               ? std::nullopt
               : std::optional { std::string_view { "the daemon could not make a workspace" } };
}

} // namespace contour::remote
