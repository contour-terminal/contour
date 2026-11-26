// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonSections.hpp>

#include <ranges>
#include <utility>
#include <vector>

namespace contour::window
{

SectionsSubscription::SectionsSubscription(std::function<void()> unsubscribe):
    _unsubscribe { std::move(unsubscribe) }
{
}

SectionsSubscription::~SectionsSubscription()
{
    reset();
}

SectionsSubscription::SectionsSubscription(SectionsSubscription&& other) noexcept:
    _unsubscribe { std::exchange(other._unsubscribe, nullptr) }
{
}

SectionsSubscription& SectionsSubscription::operator=(SectionsSubscription&& other) noexcept
{
    if (this != &other)
    {
        reset();
        _unsubscribe = std::exchange(other._unsubscribe, nullptr);
    }
    return *this;
}

void SectionsSubscription::reset() noexcept
{
    if (auto const unsubscribe = std::exchange(_unsubscribe, nullptr))
        unsubscribe();
}

SectionsSubscription SectionsSubscribers::subscribe(std::function<void()> changed)
{
    auto const id = _next++;
    _subscribers->emplace(id, std::move(changed));
    // Weak: a subscription that outlives the subscribers ends nothing.
    return SectionsSubscription { [weak = std::weak_ptr { _subscribers }, id] {
        if (auto const subscribers = weak.lock())
            subscribers->erase(id);
    } };
}

void SectionsSubscribers::notify() const
{
    // By id, each looked up as its turn comes: a call may end another subscription, or its own.
    auto const ids = std::ranges::to<std::vector>(*_subscribers | std::views::keys);
    auto const subscribers = _subscribers; // a call may destroy what holds these
    for (auto const id: ids)
        if (auto const found = subscribers->find(id); found != subscribers->end())
        {
            auto const changed = found->second; // a copy: the call may end its own subscription
            changed();
        }
}

} // namespace contour::window
