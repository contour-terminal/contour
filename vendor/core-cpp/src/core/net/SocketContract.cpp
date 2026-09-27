// SPDX-License-Identifier: Apache-2.0
#include <core/net/SocketContract.hpp>

#include <core/net/EventLoop.hpp>

#include <cassert>

namespace core::net::contract
{

void assertTeardownIsSerialisedWithDispatch([[maybe_unused]] EventLoop const& loop) noexcept
{
    assert(loop.teardownIsSerialisedWithDispatch()
           && "a loop-owned socket, listener or dial was destroyed on a thread that is not the "
              "loop's while a turn had not returned: clearing its pending operation races the "
              "readiness dispatch, so the backend may complete into storage this destructor has "
              "already freed (see core/net/SocketContract.hpp and fastcached#668)");
}

} // namespace core::net::contract
