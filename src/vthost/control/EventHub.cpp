// SPDX-License-Identifier: Apache-2.0
#include <vthost/control/EventHub.hpp>

#include <algorithm>
#include <format>
#include <ranges>

#include <vtcontrol/ObjectRef.hpp>

namespace vthost::control
{

bool accepts(EventFilter const& filter, vtcontrol::Event const& event)
{
    if (!filter.kinds.empty() && !std::ranges::contains(filter.kinds, vtcontrol::eventName(event.payload)))
        return false;
    return !filter.pane
           || event.origin.pane
                  == vtcontrol::toString(
                      vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = *filter.pane });
}

std::expected<uint32_t, vtcontrol::ControlError> EventHub::subscribe(vtcontrol::EventSink& sink,
                                                                     EventFilter filter)
{
    auto& count = _perSink[&sink];
    if (count >= _maxPerSink)
        return std::unexpected(vtcontrol::ControlError {
            .code = vtcontrol::ControlErrorCode::LimitReached,
            .message = std::format("this connection holds {} subscriptions, as many as it may", count),
            .candidates = {} });
    ++count;
    auto const id = _nextId++;
    _subscriptions.push_back(Subscription { .id = id, .sink = &sink, .filter = std::move(filter) });
    _live.insert(id);
    return id;
}

std::expected<void, vtcontrol::ControlError> EventHub::unsubscribe(vtcontrol::EventSink& sink,
                                                                   uint32_t subscription)
{
    // Another sink's subscription is as unknown to this one as one that never was.
    if (std::erase_if(
            _subscriptions,
            [&](Subscription const& entry) { return entry.id == subscription && entry.sink == &sink; })
        == 0)
        return std::unexpected(
            vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::NotFound,
                                      .message = std::format("no subscription {}", subscription),
                                      .candidates = {} });
    _live.erase(subscription);
    if (auto const held = _perSink.find(&sink); held != _perSink.end() && --held->second == 0)
        _perSink.erase(held);
    sink.subscriptionEnded(subscription);
    return {};
}

void EventHub::publish(vtcontrol::Event const& event)
{
    auto targets = std::vector<std::pair<vtcontrol::EventSink*, uint32_t>> {};
    for (auto const& subscription: _subscriptions)
        if (accepts(subscription.filter, event))
            targets.emplace_back(subscription.sink, subscription.id);
    for (auto const& [sink, id]: targets)
    {
        // Dropped by an earlier delivery: not handed what it no longer subscribes to.
        if (_live.contains(id))
            sink->deliverEvent(id, event);
    }
}

void EventHub::forget(vtcontrol::EventSink& sink)
{
    std::erase_if(_subscriptions, [&](Subscription const& entry) {
        if (entry.sink != &sink)
            return false;
        _live.erase(entry.id);
        return true;
    });
    _perSink.erase(&sink);
}

std::size_t EventHub::countOf(vtcontrol::EventSink const& sink) const noexcept
{
    auto const held = _perSink.find(&sink);
    return held != _perSink.end() ? held->second : 0;
}

} // namespace vthost::control
