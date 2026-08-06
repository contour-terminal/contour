// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Picks the @c EventSource to drive an @c EventLoop with.
///
/// Every backend implements the same interface and behaves identically; they differ
/// only in what a wait costs. This is the one place that knows which exist, so call
/// sites say @c makeDefaultEventSource() rather than carrying `#ifdef`s of their own.

#include <memory>

#include <net/EventSource.hpp>

namespace net
{

/// Which multiplexing backend an @c EventSource uses.
///
/// There is deliberately no IOCP entry. IOCP is a *completion* port, not a
/// readiness poller: a caller issues an overlapped `WSARecv`/`WSASend` and is told
/// when that specific operation finished, whereas @c EventSource answers "which
/// registered descriptor is ready now". Expressing IOCP here would mean rewriting
/// @c WindowsSocket (today `WSAEventSelect` + `recv`) and the @c ISocket contract
/// around completions — a socket-layer redesign, not another event source. Windows
/// therefore stays on @c Poll, whose `WaitForMultipleObjects` path already handles
/// the >64-handle case via @c WaitChunking.
enum class EventSourceKind : std::uint8_t
{
    Poll = 0, ///< poll(2) / WaitForMultipleObjects. Portable; a wait is O(registered).
    Epoll,    ///< epoll(7), Linux only. A wait is O(ready).
    Kqueue,   ///< kqueue(2), macOS/BSD only. A wait is O(ready).
};

/// Creates the best @c EventSource available on this platform.
///
/// Prefers the scalable native backend and falls back to @c PollEventSource when it
/// is unavailable — either because the platform has none or because the kernel
/// refused to create one (fd exhaustion). The fallback is silent by design: every
/// backend is behaviourally equivalent, so a caller has nothing to decide.
/// @return An event source, never null.
[[nodiscard]] std::unique_ptr<EventSource> makeDefaultEventSource();

/// Creates an @c EventSource of a specific kind — for tests that need to exercise
/// one backend in particular rather than whatever this platform prefers.
/// @param kind The backend to build.
/// @return The event source, or nullptr if @p kind is unavailable here or its
///         underlying kernel object could not be created.
[[nodiscard]] std::unique_ptr<EventSource> makeEventSource(EventSourceKind kind);

/// @return The kind @c makeDefaultEventSource() prefers on this platform, ignoring
///         whether construction would actually succeed.
[[nodiscard]] EventSourceKind preferredEventSourceKind() noexcept;

} // namespace net
