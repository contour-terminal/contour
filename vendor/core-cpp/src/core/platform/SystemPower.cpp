// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemPower.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace core::platform::detail
{

/// The registry's shared state: handlers in subscription order, and who is running one.
class PowerHandlerRegistryState: public std::enable_shared_from_this<PowerHandlerRegistryState>
{
  public:
    [[nodiscard]] PowerSubscription subscribe(ISystemPowerMonitor::Handler handler)
    {
        Require(handler != nullptr);
        auto const lock = std::scoped_lock { _mutex };
        auto const id = ++_lastId;
        _entries.push_back(Entry {
            .id = id, .handler = std::make_shared<ISystemPowerMonitor::Handler>(std::move(handler)) });
        return PowerSubscription { shared_from_this(), id };
    }

    /// `noexcept` is the exception policy: a throwing handler terminates (see PowerHandlerRegistry).
    void dispatch(SystemPowerEvent event) noexcept
    {
        // A handler may destroy the registry's owner, and with it the registry's reference to this state.
        auto const keepAlive = shared_from_this();
        // Copy-constructed rather than assigned into an empty vector, which GCC 15 flags at -O3.
        auto const snapshot = [this] {
            auto const lock = std::scoped_lock { _mutex };
            return _entries;
        }();
        auto const self = std::this_thread::get_id();
        for (auto const& entry: snapshot)
        {
            {
                auto const lock = std::scoped_lock { _mutex };
                if (!isSubscribed(entry.id))
                    continue;
                _running.push_back(Running { .id = entry.id, .thread = self });
            }
            (*entry.handler)(event);
            unmarkRunning(entry.id, self);
        }
    }

    [[nodiscard]] std::size_t size() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _entries.size();
    }

    void unsubscribe(std::uint64_t id) noexcept
    {
        // Declared before the lock, so the handler dies after it is released: destroying a handler can
        // unsubscribe another token of this very registry.
        auto erased = Entry {};
        auto lock = std::unique_lock { _mutex };
        if (auto const it =
                std::ranges::find_if(_entries, [id](Entry const& entry) { return entry.id == id; });
            it != _entries.end())
        {
            erased = std::move(*it);
            _entries.erase(it);
        }
        auto const self = std::this_thread::get_id();
        _idle.wait(lock, [&] {
            return std::ranges::none_of(
                _running, [&](Running const& run) { return run.id == id && run.thread != self; });
        });
    }

  private:
    struct Entry
    {
        std::uint64_t id;
        std::shared_ptr<ISystemPowerMonitor::Handler> handler;
    };

    struct Running
    {
        std::uint64_t id;
        std::thread::id thread;
    };

    /// Marks a handler as no longer running, and wakes an unsubscribe waiting for it.
    void unmarkRunning(std::uint64_t id, std::thread::id thread) noexcept
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            auto const it = std::ranges::find_if(
                _running, [&](Running const& run) { return run.id == id && run.thread == thread; });
            if (it != _running.end())
                _running.erase(it);
        }
        _idle.notify_all();
    }

    [[nodiscard]] bool isSubscribed(std::uint64_t id) const
    {
        return std::ranges::any_of(_entries, [id](Entry const& entry) { return entry.id == id; });
    }

    mutable std::mutex _mutex;
    std::condition_variable _idle;
    std::vector<Entry> _entries;
    std::vector<Running> _running;
    std::uint64_t _lastId = 0;
};

PowerHandlerRegistry::PowerHandlerRegistry(): _state { std::make_shared<PowerHandlerRegistryState>() }
{
}

PowerSubscription PowerHandlerRegistry::subscribe(ISystemPowerMonitor::Handler handler)
{
    return _state->subscribe(std::move(handler));
}

void PowerHandlerRegistry::dispatch(SystemPowerEvent event) noexcept
{
    _state->dispatch(event);
}

std::size_t PowerHandlerRegistry::size() const
{
    return _state->size();
}

} // namespace core::platform::detail

namespace core::platform
{

PowerSubscription::PowerSubscription(std::shared_ptr<detail::PowerHandlerRegistryState> state,
                                     std::uint64_t id) noexcept:
    _state { std::move(state) }, _id { id }
{
}

PowerSubscription::PowerSubscription(PowerSubscription&& other) noexcept:
    _state { std::move(other._state) }, _id { std::exchange(other._id, 0) }
{
}

PowerSubscription& PowerSubscription::operator=(PowerSubscription&& other) noexcept
{
    if (this != &other)
    {
        release();
        _state = std::move(other._state);
        _id = std::exchange(other._id, 0);
    }
    return *this;
}

PowerSubscription::~PowerSubscription()
{
    release();
}

void PowerSubscription::release() noexcept
{
    if (auto const state = std::exchange(_state, nullptr))
        state->unsubscribe(_id);
    _id = 0;
}

} // namespace core::platform
