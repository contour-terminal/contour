// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ExecutorAddressResolver` — an @c IAsyncAddressResolver that owns no thread: every lookup runs
/// on a blocking executor its owner injects.

#include <core/async/IExecutor.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/NetExport.hpp>
#include <core/net/SocketAddress.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace core::net
{

/// How many lookups may be outstanding at once.
///
/// At namespace scope rather than nested in @c ExecutorAddressResolver, for the reason
/// @c ThreadedResolverOptions is: a defaulted parameter of a nested type whose own default member
/// initializers are not yet complete is rejected outright.
struct ExecutorResolverOptions
{
    /// Largest number of lookups holding a slot: handed to the executor and not yet returned from
    /// the blocking resolver. A lookup past it is refused with @c NetErrorCode::ResourceExhausted.
    std::size_t maxInFlight { 256 };
};

/// Name resolution on an injected executor, with no thread of its own.
///
/// `getaddrinfo` blocks and takes no timeout, so somebody's thread has to wait for it; where
/// @c ThreadedAddressResolver brings a pool of its own, this one runs every lookup on the
/// @c async::IExecutor it is given — the process's blocking pool, a host's worker, a test's
/// @c async::testing::ManualExecutor. Each lookup is a detached flow that hops onto that executor,
/// calls @c IAddressResolver::resolveClassified there, and hands the waiting flow back to its loop.
///
/// - **A literal never reaches the executor**: it is parsed in place by
///   @c detail::endpointOfLiteral, and the inner resolver is not asked.
/// - **A null loop resolves inline** through the inner resolver, because with nowhere to hand a
///   result back to, offloading would park a coroutine nothing could resume.
/// - **The budget is bounded and a full one is refused**, as @c ThreadedAddressResolver's queue
///   is: over @c ExecutorResolverOptions::maxInFlight a lookup answers
///   @c NetErrorCode::ResourceExhausted at once rather than waiting for room on the loop thread.
/// - **A stop ends the wait, not the lookup.** The awaiting flow is handed back to its loop and
///   throws @c async::OperationCancelled; the lookup keeps its slot until the blocking call has
///   returned, because until then it still occupies the executor, and its late answer goes
///   nowhere.
/// - **A lookup the executor drops unrun** (an executor destroyed with it queued) releases its slot
///   and answers its waiter @c NetErrorCode::Cancelled, a cancellation by the resource and so a
///   value rather than a throw.
///
/// - **A flow already stopped is not offloaded**: it throws @c async::OperationCancelled before it
///   takes a slot.
///
/// **Teardown order.** A lookup outlives this resolver safely — it holds the inner resolver and
/// the slot count by share. What it does not outlive is the loop of a flow still parked on it,
/// which its answer is handed to: destroy a loop only once no flow of it is parked on a lookup
/// (each finished, or stopped -- a stopped flow's late answer goes nowhere). The executor, too, must
/// outlive every lookup queued on it, or drop them (which answers @c NetErrorCode::Cancelled).
///
/// **A stopped @c async::ThreadPoolExecutor runs what is submitted inline**, on the submitting
/// thread: a lookup started after its `stop()` runs `getaddrinfo` on the LOOP thread, which is the
/// stall this class exists to remove. Stop resolving through this resolver before stopping its pool.
class CORE_NET_API ExecutorAddressResolver final: public IAsyncAddressResolver
{
  public:
    /// @param blocking Where lookups run; must outlive, or drop, every lookup queued on it.
    /// @param inner Shared, because a job may outlive the resolver.
    /// @param options The lookup budget.
    ExecutorAddressResolver(async::IExecutor& blocking,
                            std::shared_ptr<IAddressResolver> inner,
                            ExecutorResolverOptions options = {});

    ExecutorAddressResolver(ExecutorAddressResolver const&) = delete;
    ExecutorAddressResolver& operator=(ExecutorAddressResolver const&) = delete;
    ExecutorAddressResolver(ExecutorAddressResolver&&) = delete;
    ExecutorAddressResolver& operator=(ExecutorAddressResolver&&) = delete;
    ~ExecutorAddressResolver() override = default;

    /// @copydoc IAsyncAddressResolver::resolve
    ///
    /// A failure carries the code @c IAddressResolver::resolveClassified gave it.
    /// Host text @c detail::checkLookupHost refuses (empty, an embedded NUL, more than 253 bytes) is
    /// @c NetErrorCode::AddressError before any lookup starts, whatever @p loop is.
    [[nodiscard]] async::Task<ResolveResult> resolve(std::string host,
                                                     std::uint16_t port,
                                                     EventLoop* loop) override;

    /// @return How many lookups have been refused for want of a slot.
    [[nodiscard]] std::size_t refused() const noexcept;

    /// @return How many lookups this resolver has handed to its executor. Zero for a process that
    ///         only resolves literals.
    [[nodiscard]] std::size_t offloaded() const noexcept;

    /// @return How many lookups hold a slot right now: handed to the executor and not yet returned
    ///         from the blocking resolver, a cancelled one included.
    [[nodiscard]] std::size_t inFlight() const noexcept;

  private:
    async::IExecutor& _blocking;
    std::shared_ptr<IAddressResolver> _inner;
    ExecutorResolverOptions _options;
    /// The slot count, shared with every lookup in flight, which releases its slot itself.
    std::shared_ptr<std::atomic<std::size_t>> _inFlight;
    std::atomic<std::size_t> _refused { 0 };
    std::atomic<std::size_t> _offloaded { 0 };
};

} // namespace core::net
