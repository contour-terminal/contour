// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ThreadedAddressResolver` — the shipped @c IAsyncAddressResolver: name resolution moved off
/// the calling thread and onto a small fixed pool.
///
/// Imported from fastcached's `Net/ThreadedAddressResolver.{hpp,cpp}` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`.

#include <core/net/IAsyncAddressResolver.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace core::net
{

/// How many resolver threads, and how much work may wait for them.
///
/// At namespace scope rather than nested in @c ThreadedAddressResolver, and that is not filing: a
/// defaulted parameter of a nested type whose own default member initializers are not yet
/// complete is rejected outright.
struct ThreadedResolverOptions
{
    /// Fixed pool size. Never one thread per dial.
    ///
    /// Two rather than one, because a single five-second SERVFAIL would otherwise
    /// head-of-line-block every other dial behind it. Two rather than `hardware_concurrency`,
    /// because this work is I/O-bound and a pool sized to cores puts thirty idle threads in a
    /// process that dials three peers.
    std::size_t threads { 2 };

    /// Largest number of lookups that may be waiting for a thread.
    std::size_t maxQueueDepth { 256 };
};

/// Name resolution moved off the calling thread, onto a fixed pool.
///
/// `getaddrinfo` has no portable asynchronous form and takes no timeout, so a thread is
/// unavoidable: the choice is only WHOSE. This puts it on a small pool of its own so that a loop
/// thread — which is carrying every other connection on that loop — never waits for a resolver.
///
/// It **decorates** the blocking @c IAddressResolver rather than calling `getaddrinfo` itself,
/// which keeps @c SocketAddress.cpp the single place that issues the lookup and lets every rule
/// below be tested against a scripted inner with no DNS anywhere.
///
/// ## A literal never reaches the pool
///
/// Load-bearing rather than an optimisation. Most dials in the consuming projects are to a
/// literal, and paying a thread hand-off plus two context switches for `inet_pton` would be a
/// real regression on a hot path. It is also what lets the whole connect path be exercised
/// without a thread existing.
///
/// ## The queue is bounded, and a full queue is refused rather than waited on
///
/// An unbounded queue is a memory-exhaustion hole reachable by whatever provokes dials. And
/// blocking the caller to wait for room would reintroduce, on the loop thread, precisely the
/// stall this class exists to remove — so over-depth returns @c NetErrorCode::WouldBlock
/// immediately. `WouldBlock` and not `SystemError`, because a caller can retry the first and can
/// do nothing at all with the second.
class ThreadedAddressResolver final: public IAsyncAddressResolver
{
  public:
    /// @param inner The blocking resolver to delegate to. Must outlive this.
    /// @param options Pool size and queue bound.
    explicit ThreadedAddressResolver(IAddressResolver& inner = defaultAddressResolver(),
                                     ThreadedResolverOptions options = {});

    ThreadedAddressResolver(ThreadedAddressResolver const&) = delete;
    ThreadedAddressResolver& operator=(ThreadedAddressResolver const&) = delete;
    ThreadedAddressResolver(ThreadedAddressResolver&&) = delete;
    ThreadedAddressResolver& operator=(ThreadedAddressResolver&&) = delete;

    /// Stops and joins; see @c stop for what happens to work in flight.
    ~ThreadedAddressResolver() override;

    /// @copydoc IAsyncAddressResolver::resolve
    ///
    /// A stop reaches a lookup that is queued or running alike: the awaiting flow is handed back
    /// to @p loop cancelled at once, and the worker, which cannot be interrupted inside the
    /// blocking resolver, finishes in its own time and publishes into a slot nobody waits on.
    /// The lookup still occupies its thread until then — a stop ends the WAIT, not the lookup.
    [[nodiscard]] async::Task<ResolveResult> resolve(std::string host,
                                                     std::uint16_t port,
                                                     EventLoop* loop) override;

    /// Refuses new work, fails everything queued, and wakes the threads.
    ///
    /// Idempotent. A queued lookup is resumed with @c NetErrorCode::Cancelled, so no coroutine is
    /// stranded. A lookup already **inside** the blocking resolver cannot be interrupted — there
    /// is no portable way — so the join waits for it.
    ///
    /// **Stop this before the loops it hands results back to.** A submit to a loop whose `run()`
    /// has already returned queues a handle nobody will ever resume, which is a leaked frame.
    void stop() noexcept;

    /// @return How many lookups have been refused for want of queue room. For tests and
    ///         diagnostics; an operator seeing this move should raise @c maxQueueDepth or find
    ///         out what is provoking the dials.
    [[nodiscard]] std::size_t refused() const noexcept;

    /// @return How many lookups this resolver has handed to a thread. Zero for a process that
    ///         only ever dials literals, which is the property the fast path exists to give and
    ///         the one a test asserts.
    [[nodiscard]] std::size_t offloaded() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/// @return The process-wide resolver the free @c connect uses.
///
/// Ambient with a name, for the reason @c defaultAddressResolver is: a caller that has its own
/// resolver passes it, and a caller that has not gets a documented default instead of a private
/// static nothing can reach.
///
/// **Its pool starts on first use and never on a literal**, so a process that only dials
/// literals creates no thread at all.
[[nodiscard]] ThreadedAddressResolver& defaultAsyncResolver();

} // namespace core::net
