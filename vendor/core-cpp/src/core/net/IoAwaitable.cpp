// SPDX-License-Identifier: Apache-2.0
//
// The one hop that keeps `<core/net/EventLoop.hpp>` out of `<core/net/ISocket.hpp>`.
//
// `ResultAwaitable::onStop` needs exactly one thing from the loop -- `requestCancel` -- and a
// header that carries a whole class definition for one call imposes it on every translation unit
// downstream. `SocketContract.hpp` already forward-declares `EventLoop` and puts its own loop-using
// function out of line for this reason; this is the same trade in the header that made the
// forward declaration moot.

#include <core/net/IoAwaitable.hpp>

#include <core/net/EventLoop.hpp>

#include <tuple>
#include <utility>

namespace core::net
{

void requestCancelOn(EventLoop& loop, ParkId park) noexcept
{
    loop.requestCancel(park);
}

void detail::resumeSoonOn(EventLoop& loop,
                          std::coroutine_handle<> waiter,
                          std::coroutine_handle<> unownedRoot) noexcept
{
    loop.resumeCompleted(waiter, unownedRoot);
}

void cancelPendingOn(EventLoop& loop, std::coroutine_handle<> waiter) noexcept
{
    std::ignore = loop.cancelPending(waiter);
}

} // namespace core::net
