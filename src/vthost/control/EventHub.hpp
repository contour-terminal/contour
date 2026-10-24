// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Who is subscribed to which control API events, and the delivery of one event to them.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vtcontrol/ControlApi.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/Events.hpp>
#include <vtcontrol/Uuid.hpp>

namespace vthost::control
{

/// Which events a subscription takes.
struct EventFilter
{
    std::vector<std::string> kinds;      ///< Catalog names of the events taken; empty for every kind.
    std::optional<vtcontrol::Uuid> pane; ///< Only events about this pane; nullopt for every object.
};

/// @param filter A subscription's filter.
/// @param event An event.
/// @return Whether the subscription takes the event.
[[nodiscard]] bool accepts(EventFilter const& filter, vtcontrol::Event const& event);

/// The subscriptions of every sink. Loop thread only: the host's signals reach the loop before they are
/// published, so neither a subscription nor a delivery ever crosses a thread here.
///
/// Subscription ids are never reused, and are unique across sinks. A sink holds at most a fixed number of
/// subscriptions at once, so that a connection subscribing in a loop costs the daemon bounded memory and
/// bounded work per event.
class EventHub
{
  public:
    /// @param maxPerSink The most subscriptions one sink may hold at once.
    explicit EventHub(std::size_t maxPerSink): _maxPerSink(maxPerSink) {}

    /// @param sink Where the subscription's events go. Borrowed: forget() it before it goes.
    /// @param filter Which events it takes.
    /// @return The subscription's id, or `LimitReached` when @p sink already holds its share.
    [[nodiscard]] std::expected<uint32_t, vtcontrol::ControlError> subscribe(vtcontrol::EventSink& sink,
                                                                             EventFilter filter);

    /// Ends a subscription, and tells @p sink so (EventSink::subscriptionEnded).
    /// @param sink The sink that subscribed.
    /// @param subscription Its id.
    /// @return Nothing, or `NotFound` when @p sink holds no such subscription.
    [[nodiscard]] std::expected<void, vtcontrol::ControlError> unsubscribe(vtcontrol::EventSink& sink,
                                                                           uint32_t subscription);

    /// Hands @p event to every subscription that takes it, in subscription order. A sink may subscribe or
    /// unsubscribe as it is handed one: who is handed it was settled before the first delivery.
    /// @param event The event.
    void publish(vtcontrol::Event const& event);

    /// Drops every subscription of @p sink.
    /// @param sink The sink.
    void forget(vtcontrol::EventSink& sink);

    /// @return How many subscriptions there are.
    [[nodiscard]] std::size_t size() const noexcept { return _subscriptions.size(); }

    /// @param sink A sink.
    /// @return How many subscriptions it holds.
    [[nodiscard]] std::size_t countOf(vtcontrol::EventSink const& sink) const noexcept;

  private:
    struct Subscription
    {
        uint32_t id = 0;
        vtcontrol::EventSink* sink = nullptr;
        EventFilter filter;
    };

    std::size_t _maxPerSink;
    std::vector<Subscription> _subscriptions; ///< In subscription order, which is delivery order.
    std::unordered_set<uint32_t> _live;       ///< The ids in @ref _subscriptions.
    std::unordered_map<vtcontrol::EventSink const*, std::size_t> _perSink; ///< Subscriptions per sink.
    uint32_t _nextId = 1;
};

} // namespace vthost::control
