// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A host that does nothing until a test tells it to.

#include <core/net/IHostScheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

namespace core::net::testing
{

/// An @c IHostScheduler that records what was asked of it and fires it on demand.
///
/// This is what makes @c HostDrivenBackend a thing every platform tests rather than
/// something only a browser can observe. A case asserts the exact delay the backend
/// asked for — which is the whole of `armWakeAt`'s contract — and fires the callbacks
/// itself, so nothing waits on a real clock.
class ManualHostScheduler final: public IHostScheduler
{
  public:
    /// One thing the host was asked to do.
    struct Request
    {
        std::chrono::milliseconds delay {}; ///< What the caller asked to wait.
        HostCallback fn = nullptr;          ///< What to call.
        void* state = nullptr;              ///< What to call it with.
    };

    ManualHostScheduler() = default;
    ManualHostScheduler(ManualHostScheduler const&) = delete;
    ManualHostScheduler(ManualHostScheduler&&) = delete;
    ManualHostScheduler& operator=(ManualHostScheduler const&) = delete;
    ManualHostScheduler& operator=(ManualHostScheduler&&) = delete;

    /// Delivers everything still pending, cleared requests included, as a browser
    /// eventually delivers every timer it accepted.
    ///
    /// A callback's state may be something only the callback frees — a
    /// @c HostDrivenBackend's pump ticket is — so dropping it here would leak one per
    /// case that ends with a pump out. Safe because the host outlives every backend it
    /// serves: what fires here finds its backend gone and runs nothing.
    ~ManualHostScheduler()
    {
        _pending.insert(_pending.end(), _cleared.begin(), _cleared.end());
        _cleared.clear();
        pump();
    }

    void callAfter(std::chrono::milliseconds delay, HostCallback fn, void* state) override
    {
        ++_requestCount;
        _pending.push_back(Request { .delay = delay, .fn = fn, .state = state });
    }

    /// @return Everything asked of this host and not yet fired, in the order it was
    ///         asked. A case asserts its size (the coalescing) and its delays (the
    ///         clamp).
    [[nodiscard]] std::vector<Request> const& pending() const noexcept { return _pending; }

    /// @return How many requests are waiting to be fired.
    [[nodiscard]] std::size_t pendingCount() const noexcept { return _pending.size(); }

    /// @return How many requests this host has been given in all, fired or not.
    [[nodiscard]] std::size_t requestCount() const noexcept { return _requestCount; }

    /// Fires everything currently pending, soonest delay first.
    ///
    /// Taken out BEFORE any of it runs: a callback drives a turn of the loop, which
    /// asks for the next pump, and that request belongs to the next @c pump() rather
    /// than to this one — otherwise a loop that always re-arms would never return from
    /// here.
    void pump()
    {
        auto firing = std::exchange(_pending, {});
        std::ranges::stable_sort(firing, {}, &Request::delay);
        for (auto const& request: firing)
            if (request.fn != nullptr)
                request.fn(request.state);
    }

    /// Takes everything pending out of @c pending() without firing it now — for a case
    /// that has finished with the backend and does not want its teardown to run a turn.
    /// What was cleared is delivered at destruction, when its backend is gone, because
    /// its state may be something only the callback frees.
    void clear()
    {
        _cleared.insert(_cleared.end(), _pending.begin(), _pending.end());
        _pending.clear();
    }

  private:
    std::vector<Request> _pending;
    std::vector<Request> _cleared; ///< Taken out by @c clear(), delivered at destruction.
    std::size_t _requestCount = 0;
};

} // namespace core::net::testing
