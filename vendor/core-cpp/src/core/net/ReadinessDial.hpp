// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::dialReadiness` — one outbound connection over a readiness-driven loop.
///
/// **De-templatised from fastcached's `Net/ReactorDial.hpp`** (`DialReadiness<Traits>`, at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), which was templated on a platform triple because
/// upstream had an `EpollReactor` and a `KqueueReactor` with no common base — and then shipped an
/// `EpollConnector` and a `KqueueConnector` that were identical but for the type names. core-cpp
/// has ONE @c EventLoop over an @c IoBackend, so the template has nothing left to vary over and
/// the platform difference lives in `detail/DialPrimitives.hpp`, which is four OS calls rather
/// than a whole dial.
///
/// What is kept verbatim in meaning is the property that made upstream's shape right: **the
/// per-dial state lives in the dialling coroutine's own frame.** Its address is stable for
/// exactly as long as the loop can reach it, and it disappears with the attempt — where a
/// connector holding a slot per dial would let a dial that timed out while still in flight tie
/// one up.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/KeepAlive.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/platform/Clock.hpp>

namespace core::net
{

class EventLoop;

namespace detail
{

    /// Dials one candidate endpoint, parking on write readiness until it settles.
    ///
    /// **It checks `SO_ERROR` and never consults which callback fired** (Ruling R101). Readiness
    /// is not success: a refused connect also makes the socket ready, and which of readable,
    /// writable or failed the kernel reports varies by platform — on macOS a failed connect
    /// arrives as a writable event. `getsockopt(SO_ERROR)` is the only thing that distinguishes
    /// them, and skipping it hands the caller a socket whose first write fails.
    ///
    /// @param loop The loop the resulting socket is pinned to and whose backend watches the dial.
    ///        Not owned; a pointer, since coroutine reference parameters can dangle.
    /// @param endpoint The candidate to dial. **By value**: this is a coroutine, so a reference
    ///        parameter could dangle at the first suspend.
    /// @param deadline When to give up on THIS candidate; `SteadyTimePoint::max()` for never.
    /// @param options The buffer sizes, asked for before the connect (@c applySocketBufferSizes),
    ///        and keepalive, armed after it (@c applyStreamSocketOptions).
    /// @return The connected socket, or why this candidate did not produce one.
    /// @throws async::OperationCancelled if the awaiting flow's own stop token is stopped while
    ///         the dial is outstanding. A cancel from the FLOW unwinds; the socket is closed on
    ///         the way out, so nothing is leaked.
    [[nodiscard]] async::Task<SocketResult> dialReadiness(EventLoop* loop,
                                                          ResolvedEndpoint endpoint,
                                                          platform::SteadyTimePoint deadline,
                                                          StreamSocketOptions options);

} // namespace detail

} // namespace core::net
