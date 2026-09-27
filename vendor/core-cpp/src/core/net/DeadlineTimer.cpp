// SPDX-License-Identifier: Apache-2.0
#include <core/net/DeadlineTimer.hpp>

#include <cassert>
#include <tuple>
#include <utility>

namespace core::net
{

DeadlineTimer::DeadlineTimer(EventLoop& loop,
                             platform::SteadyTimePoint deadline,
                             Callback onExpired,
                             void* state):
    _loop(&loop), _onExpired(onExpired), _state(state)
{
    // Checked HERE, where the mistake is. `addTimer` asserts its own callback, but the one it is
    // handed is `&DeadlineTimer::fire`, which is never null -- so a null `onExpired` would sail
    // past it and be called a turn later from inside `runOnce`, with nothing naming the
    // construction site.
    assert(onExpired != nullptr
           && "DeadlineTimer with no callback: it would be armed, fired a turn later from inside "
              "the loop's drain, and call through a null pointer there");
    _timer = loop.addTimer(deadline, &DeadlineTimer::fire, this);
}

DeadlineTimer::~DeadlineTimer()
{
    disarm();
}

void DeadlineTimer::disarm() noexcept
{
    if (_settled)
        return;
    _settled = true;
    // Retired rather than left to elapse. Upstream could not do this -- its `Schedule` had no
    // cancellation -- so a settled operation left one parked frame behind for up to a poll
    // interval, and a reactor destroyed in that window never freed it: one leaked frame per dial
    // and per cache exchange, which is what made an ASan build of fastcache-cc exit non-zero.
    if (auto const timer = std::exchange(_timer, TimerId::invalid()))
        std::ignore = _loop->cancelTimer(timer);
}

void DeadlineTimer::fire(void* state)
{
    auto* const self = static_cast<DeadlineTimer*>(state);

    // Read out before anything can destroy `*self`: the user's callback is allowed to, and after
    // the call below this object may no longer exist.
    auto* const callback = self->_onExpired;
    auto* const callbackState = self->_state;

    // Settled FIRST, and the id dropped with it. The loop has already taken this park out of its
    // table, so a `cancelTimer` from the destructor the callback triggers would report false and
    // retire nothing -- but it must not be asked at all, because by then the id names a park that
    // is gone and the answer would be a coincidence rather than a contract.
    self->_settled = true;
    self->_timer = TimerId::invalid();

    callback(callbackState);
}

} // namespace core::net
