// SPDX-License-Identifier: Apache-2.0
#include <vthost/ConnectionWorkspaces.hpp>

#include <algorithm>
#include <utility>

namespace vthost
{

void ConnectionWorkspaces::publish(ConnectionId const& connection,
                                   std::optional<vtworkspace::WorkspaceId> selected)
{
    auto const lock = std::lock_guard { _mutex };
    std::erase_if(_selections, [&](Selection const& entry) { return entry.connection == connection; });
    if (!selected)
    {
        std::erase_if(_focus, [&](auto const& stamp) { return stamp.first == connection; });
        return;
    }
    _selections.push_back(Selection { .connection = connection, .workspace = *selected });
}

void ConnectionWorkspaces::noteFocused(ConnectionId const& connection)
{
    auto const lock = std::lock_guard { _mutex };
    std::erase_if(_focus, [&](auto const& stamp) { return stamp.first == connection; });
    _focus.emplace_back(connection, ++_focusClock);
}

std::optional<vtworkspace::WorkspaceId> ConnectionWorkspaces::selectedBy(ConnectionId const& connection) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const found = std::ranges::find(_selections, connection, &Selection::connection);
    return found != _selections.end() ? std::optional { found->workspace } : std::nullopt;
}

std::optional<ConnectionId> ConnectionWorkspaces::mostRecentlyFocused() const
{
    auto const lock = std::lock_guard { _mutex };
    auto best = std::optional<ConnectionId> {};
    auto bestStamp = uint64_t { 0 };
    for (auto const& selection: _selections)
    {
        auto const stamp =
            std::ranges::find(_focus, selection.connection, &std::pair<ConnectionId, uint64_t>::first);
        auto const focused = stamp != _focus.end() ? stamp->second : uint64_t { 0 };
        // A window never focused still counts, after every one that was: the first attached of those.
        if (!best || focused > bestStamp)
        {
            best = selection.connection;
            bestStamp = focused;
        }
    }
    return best;
}

} // namespace vthost
