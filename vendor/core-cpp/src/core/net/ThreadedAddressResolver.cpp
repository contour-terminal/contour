// SPDX-License-Identifier: Apache-2.0
#include <core/net/ThreadedAddressResolver.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/detail/ParkedCall.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{

    /// One lookup's rendezvous between a worker thread and the parked coroutine.
    using ResolveCall = detail::ParkedCall<ResolveResult>;

} // namespace

/// Pool, queue, and the state @c stop has to reach.
struct ThreadedAddressResolver::Impl
{
    IAddressResolver& inner;
    ThreadedResolverOptions options;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;

    /// One queued lookup.
    struct Job
    {
        std::shared_ptr<ResolveCall> call;
        std::string host;
        std::uint16_t port = 0;
    };

    std::deque<Job> queue;
    std::vector<std::thread> threads;

    std::atomic<std::size_t> refusedCount { 0 };
    std::atomic<std::size_t> offloadedCount { 0 };

    Impl(IAddressResolver& resolver, ThreadedResolverOptions opts) noexcept: inner(resolver), options(opts) {}

    /// One worker's whole life: take a job, resolve it, publish it.
    void runWorker()
    {
        while (true)
        {
            auto job = Job {};
            {
                auto lock = std::unique_lock { mutex };
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }

            auto resolved = inner.resolveClassified(job.host, job.port);
            if (resolved.has_value())
                job.call->settle(std::move(*resolved));
            else
                job.call->settle(
                    std::unexpected(resolveFailure(job.host, job.port, std::move(resolved.error()))));
        }
    }

    /// Starts the pool on first use.
    ///
    /// Lazily, because a process whose dials are all literals — which is most of them — must not
    /// pay for threads it never uses. The caller holds @c mutex.
    void ensureStarted()
    {
        if (!threads.empty() || stopping)
            return;
        threads.reserve(options.threads);
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, options.threads))
            threads.emplace_back([this] { runWorker(); });
    }

    /// Joins every worker. Called with @c mutex NOT held, after @c stopping is set and the
    /// condition variable has been notified.
    void joinWorkers() noexcept
    {
        for (auto& worker: threads)
        {
            if (worker.joinable())
                worker.join();
        }
        threads.clear();
    }
};

ThreadedAddressResolver::ThreadedAddressResolver(IAddressResolver& inner, ThreadedResolverOptions options):
    _impl(std::make_unique<Impl>(inner, options))
{
}

ThreadedAddressResolver::~ThreadedAddressResolver()
{
    stop();
}

std::size_t ThreadedAddressResolver::refused() const noexcept
{
    return _impl->refusedCount.load(std::memory_order_relaxed);
}

std::size_t ThreadedAddressResolver::offloaded() const noexcept
{
    return _impl->offloadedCount.load(std::memory_order_relaxed);
}

void ThreadedAddressResolver::stop() noexcept
{
    auto abandoned = std::deque<Impl::Job> {};
    {
        auto const guard = std::scoped_lock { _impl->mutex };
        if (_impl->stopping)
            return;
        _impl->stopping = true;
        abandoned.swap(_impl->queue);
    }
    _impl->wake.notify_all();

    // Every queued lookup is resumed rather than dropped, so no coroutine is left parked on an
    // answer that will never come.
    for (auto& job: abandoned)
        job.call->settle(std::unexpected(
            makeNetError(NetErrorCode::Cancelled, 0, "the resolver stopped before the lookup ran")));

    _impl->joinWorkers();
}

async::Task<ResolveResult> ThreadedAddressResolver::resolve(std::string host,
                                                            std::uint16_t port,
                                                            EventLoop* loop)
{
    if (auto checked = detail::checkLookupHost(host); !checked.has_value())
        co_return std::unexpected(std::move(checked.error()));

    // A literal needs no lookup, so it needs no thread. See the class comment: here that is the
    // common case, not the exotic one.
    //
    // The null-loop arm is a correctness requirement rather than a second optimisation: with
    // nowhere to submit a result back to, offloading would park a coroutine nothing could ever
    // resume.
    if (loop == nullptr || detail::isNumericHost(host))
    {
        auto resolved = _impl->inner.resolveClassified(host, port);
        if (!resolved.has_value())
            co_return std::unexpected(resolveFailure(host, port, std::move(resolved.error())));
        co_return std::move(*resolved);
    }

    auto call = std::make_shared<ResolveCall>(loop);
    {
        auto const guard = std::scoped_lock { _impl->mutex };
        if (_impl->stopping)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "the resolver is stopping"));

        if (_impl->queue.size() >= _impl->options.maxQueueDepth)
        {
            _impl->refusedCount.fetch_add(1, std::memory_order_relaxed);
            co_return std::unexpected(
                makeNetError(NetErrorCode::ResourceExhausted,
                             0,
                             std::format("the resolver queue is full ({} waiting); not resolving {}:{}",
                                         _impl->queue.size(),
                                         host,
                                         port)));
        }

        _impl->ensureStarted();
        _impl->queue.push_back(Impl::Job { .call = call, .host = std::move(host), .port = port });
        _impl->offloadedCount.fetch_add(1, std::memory_order_relaxed);
    }
    _impl->wake.notify_one();

    co_return co_await ResolveCall::wait(std::move(call));
}

ThreadedAddressResolver& defaultAsyncResolver()
{
    static ThreadedAddressResolver resolver;
    return resolver;
}

} // namespace core::net
