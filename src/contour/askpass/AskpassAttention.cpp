// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassAttention.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace contour::askpass
{

void AskpassAttention::publish(std::vector<SinkExposure> sinks)
{
    auto const lock = std::lock_guard { _mutex };
    _sinks = std::move(sinks);
}

bool AskpassAttention::isAttended(AskpassRouteId route) const
{
    auto const lock = std::lock_guard { _mutex };
    auto routes = std::vector<AskpassRouteId> {};
    for (auto const& sink: _sinks)
        routes.push_back(sink.route);
    auto const picked = pickAskpassSinkRoute(routes, route);
    if (!picked)
        return true;
    auto const found = std::ranges::find(_sinks, *picked, &SinkExposure::route);
    return found->state == SinkExposure::State::OnScreen;
}

} // namespace contour::askpass
