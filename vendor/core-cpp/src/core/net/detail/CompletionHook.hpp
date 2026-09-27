// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::resumeSoonOn` -- one declaration for the header that calls it (`IoAwaitable.hpp`) and
/// the one that befriends it (`EventLoop.hpp`), neither of which includes the other.

#include <coroutine>

namespace core::net
{

class EventLoop;

namespace detail
{

    /// Hands @p waiter to @p loop's ready queue, to be resumed in its drain step: `ResultAwaitable`'s
    /// out-of-line completion hook, out of line so its header need not include the loop's. Loop
    /// thread only, as @c EventLoop::resumeSoon. Internal: a transport completes an operation
    /// through `ResultAwaitable::complete`.
    ///
    /// **The waiter must take itself back out of the queue if its frame is destroyed first**
    /// (@c cancelPendingOn), as @c ResultAwaitable's destructor does: the claim the queue entry holds
    /// on @p unownedRoot is an @c async::detail::CountedClaim, which relies on exactly that.
    /// @param loop The loop to resume on.
    /// @param waiter The suspended coroutine.
    /// @param unownedRoot The root of @p waiter's chain where nobody owns it -- the loop is then to
    ///        free the chain rather than resume it if it is torn down first -- or an empty handle
    ///        (@c async::detail::unownedRootOf).
    void resumeSoonOn(EventLoop& loop,
                      std::coroutine_handle<> waiter,
                      std::coroutine_handle<> unownedRoot) noexcept;

} // namespace detail

} // namespace core::net
