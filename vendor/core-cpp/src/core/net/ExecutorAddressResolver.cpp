// SPDX-License-Identifier: Apache-2.0
#include <core/net/ExecutorAddressResolver.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/detail/InFlightBudget.hpp>
#include <core/net/detail/ParkedCall.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{

    /// One lookup's rendezvous between the executor that runs it and the parked flow.
    using ResolveCall = detail::ParkedCall<ResolveResult>;

    /// One lookup's slot and its answer, owned by the detached flow that runs it.
    ///
    /// **Its destructor is the guard of the slot rule.** A lookup that finishes gives the slot back
    /// and settles its call through @c finish. One that never gets there — its executor freed it
    /// unrun, or its frame could not even be allocated — reaches the destructor with both still
    /// held, and the destructor frees the slot and answers the waiter @c NetErrorCode::Cancelled, so
    /// neither a coroutine nor a slot is stranded. Nothing on that path throws.
    class LookupLease
    {
      public:
        LookupLease(std::shared_ptr<std::atomic<std::size_t>> inFlight,
                    std::shared_ptr<ResolveCall> call) noexcept:
            _inFlight(std::move(inFlight)), _call(std::move(call))
        {
        }

        LookupLease(LookupLease&&) noexcept = default;
        LookupLease(LookupLease const&) = delete;
        LookupLease& operator=(LookupLease const&) = delete;
        LookupLease& operator=(LookupLease&&) = delete;

        ~LookupLease() { fail(NetErrorCode::Cancelled); }

        /// Gives the slot back, THEN answers the waiter.
        ///
        /// In that order so that a flow resumed with its answer never finds its own slot still
        /// taken -- at the cap, a lookup it starts next would otherwise be refused for nothing. The
        /// slot still stands for the executor's thread: it is given back only once the blocking
        /// call has returned, a stopped lookup's included. At most once; a later call does nothing.
        /// @param answer What the lookup came to.
        void finish(ResolveResult answer) noexcept
        {
            auto const call = std::exchange(_call, nullptr);
            auto const inFlight = std::exchange(_inFlight, nullptr);
            if (!call)
                return;
            inFlight->fetch_sub(1, std::memory_order_acq_rel);
            call->settle(std::move(answer));
        }

        /// Finishes with a failure that allocates nothing, for the paths that cannot take a throw.
        /// @param code What the waiter is told.
        void fail(NetErrorCode code) noexcept
        {
            if (!_call)
                return;
            // No context: a destructor and a catch-all have no message worth an allocation.
            finish(std::unexpected(makeNetError(code)));
        }

      private:
        std::shared_ptr<std::atomic<std::size_t>> _inFlight;
        std::shared_ptr<ResolveCall> _call;
    };

    /// Runs one lookup on @p blocking and answers its call.
    ///
    /// Detached, because nobody may own it: the waiting flow can be stopped and gone long before
    /// `getaddrinfo` returns. Everything it touches it holds by value or by share — never the
    /// resolver that started it — so it outlives that resolver safely. Whatever is thrown on the
    /// way -- an executor that cannot queue it, an allocation inside the inner resolver -- is caught
    /// and answered @c NetErrorCode::SystemError: an exception escaping a detached flow ends the
    /// process, and a waiter not answered would be stranded.
    /// @param blocking Where the lookup runs.
    /// @param inner The blocking resolver.
    /// @param host The name to look up.
    /// @param port The port to pair it with.
    /// @param lease The lookup's slot and call.
    async::DetachedTask runLookup(async::IExecutor* blocking,
                                  std::shared_ptr<IAddressResolver> inner,
                                  std::string host,
                                  std::uint16_t port,
                                  LookupLease lease)
    {
        try
        {
            co_await async::ResumeOn { *blocking };
            auto resolved = inner->resolveClassified(host, port);
            if (resolved.has_value())
                lease.finish(std::move(*resolved));
            else
                lease.finish(std::unexpected(resolveFailure(host, port, std::move(resolved.error()))));
        }
        catch (...)
        {
            lease.fail(NetErrorCode::SystemError);
        }
    }

} // namespace

ExecutorAddressResolver::ExecutorAddressResolver(async::IExecutor& blocking,
                                                 std::shared_ptr<IAddressResolver> inner,
                                                 ExecutorResolverOptions options):
    _blocking(blocking),
    _inner(std::move(inner)),
    _options(options),
    _inFlight(std::make_shared<std::atomic<std::size_t>>(0))
{
}

std::size_t ExecutorAddressResolver::refused() const noexcept
{
    return _refused.load(std::memory_order_relaxed);
}

std::size_t ExecutorAddressResolver::offloaded() const noexcept
{
    return _offloaded.load(std::memory_order_relaxed);
}

std::size_t ExecutorAddressResolver::inFlight() const noexcept
{
    return _inFlight->load(std::memory_order_acquire);
}

async::Task<ResolveResult> ExecutorAddressResolver::resolve(std::string host,
                                                            std::uint16_t port,
                                                            EventLoop* loop)
{
    if (auto checked = detail::checkLookupHost(host); !checked.has_value())
        co_return std::unexpected(std::move(checked.error()));

    // A literal needs no lookup, so it needs neither the executor nor the inner resolver.
    if (auto literal = detail::endpointOfLiteral(host, port))
        co_return std::vector<ResolvedEndpoint> { *literal };

    // With nowhere to submit a result back to, offloading would park a coroutine nothing could
    // ever resume.
    if (loop == nullptr)
    {
        auto resolved = _inner->resolveClassified(host, port);
        if (!resolved.has_value())
            co_return std::unexpected(resolveFailure(host, port, std::move(resolved.error())));
        co_return std::move(*resolved);
    }

    // A flow already stopped is not handed to the executor at all: its lookup would take a slot
    // and a thread for an answer nobody can receive.
    if ((co_await async::thisCoroStopToken()).stop_requested())
        throw async::OperationCancelled {};

    // Allocated before the slot is taken, so a failed allocation has no slot to give back.
    auto call = std::make_shared<ResolveCall>(loop);
    if (detail::reserveSlot(*_inFlight, _options.maxInFlight) == detail::SlotReservation::Refused)
    {
        _refused.fetch_add(1, std::memory_order_relaxed);
        co_return std::unexpected(
            makeNetError(NetErrorCode::ResourceExhausted,
                         0,
                         std::format("the resolver budget is full ({} in flight); not resolving {}:{}",
                                     _options.maxInFlight,
                                     host,
                                     port)));
    }

    _offloaded.fetch_add(1, std::memory_order_relaxed);
    runLookup(&_blocking, _inner, std::move(host), port, LookupLease { _inFlight, call });
    co_return co_await ResolveCall::wait(std::move(call));
}

} // namespace core::net
