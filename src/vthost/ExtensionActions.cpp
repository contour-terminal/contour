// SPDX-License-Identifier: Apache-2.0
#include <vthost/ExtensionActions.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>

#include <vthost/Logging.hpp>

namespace vthost
{

ExtensionActions::ExtensionActions(std::function<void(std::string_view extension, ServedHint hint)> raise):
    _raise { std::move(raise) }
{
}

Origin ExtensionActions::originOf(ConnectionId const& connection)
{
    auto const lock = std::scoped_lock { _mutex };
    if (auto const known = std::ranges::find(_origins, connection, &std::pair<ConnectionId, Origin>::first);
        known != _origins.end())
        return known->second;
    auto const origin = _nextOrigin++;
    _origins.emplace_back(connection, origin);
    return origin;
}

void ExtensionActions::forget(ConnectionId const& connection)
{
    auto const lock = std::scoped_lock { _mutex };
    auto const gone = std::ranges::find(_origins, connection, &std::pair<ConnectionId, Origin>::first);
    if (gone == _origins.end())
        return;
    for (auto& handed: _handedOut | std::views::values)
        std::erase(handed, gone->second);
    _origins.erase(gone);
}

void ExtensionActions::deliver(std::string_view extension, std::string_view action, Origin origin)
{
    {
        auto const lock = std::scoped_lock { _mutex };
        auto queue = _queues.find(extension);
        if (queue == _queues.end())
            queue =
                _queues.emplace(std::string(extension), std::deque<std::pair<std::string, Origin>> {}).first;
        if (queue->second.size() == MaxQueuedActions)
        {
            extensionLog()("daemon extension {}: dropping its oldest invoked action {}: {} are queued",
                           extension,
                           queue->second.front().first,
                           MaxQueuedActions);
            queue->second.pop_front();
        }
        queue->second.emplace_back(std::string(action), origin);
    }
    if (_raise)
        _raise(extension, ServedHint::Actions);
}

bool ExtensionActions::holdsActionsFor(std::string_view extension) const
{
    auto const lock = std::scoped_lock { _mutex };
    auto const queue = _queues.find(extension);
    return queue != _queues.end() && !queue->second.empty();
}

std::vector<std::pair<std::string, Origin>> ExtensionActions::take(std::string_view extension)
{
    auto const lock = std::scoped_lock { _mutex };
    auto taken = std::vector<std::pair<std::string, Origin>> {};
    if (auto const queue = _queues.find(extension); queue != _queues.end())
    {
        std::ranges::move(queue->second, std::back_inserter(taken));
        queue->second.clear();
    }
    // What the extension was handed, it may act for -- and only that.
    if (!taken.empty())
    {
        auto& handed = _handedOut[std::string(extension)];
        for (auto const origin: taken | std::views::values)
            if (!std::ranges::contains(handed, origin))
                handed.push_back(origin);
    }
    return taken;
}

std::optional<ConnectionId> ExtensionActions::connectionOf(std::string_view extension, Origin origin) const
{
    auto const lock = std::scoped_lock { _mutex };
    if (auto const handed = _handedOut.find(extension);
        handed == _handedOut.end() || !std::ranges::contains(handed->second, origin))
        return std::nullopt;
    auto const known = std::ranges::find(_origins, origin, &std::pair<ConnectionId, Origin>::second);
    if (known == _origins.end())
        return std::nullopt;
    return known->first;
}

} // namespace vthost
