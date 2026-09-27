// SPDX-License-Identifier: Apache-2.0
#include <core/net/ThreadedAddressResolver.hpp>

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/EventLoop.hpp>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{

    /// One lookup's rendezvous between a worker thread and the parked coroutine.
    ///
    /// Shared, because either side may reach it last: the worker must be able to publish into it
    /// after the task was abandoned, and the task must be able to read it after the worker is
    /// gone.
    ///
    /// **Whoever takes @c waiter first decides the outcome**, and the other finds it empty. The
    /// worker takes it to deliver an answer (@c done), a stop takes it to deliver a cancellation
    /// (@c cancelled), and both do so under @c mutex — so exactly one of them hands the coroutine
    /// back, and the loser's news goes nowhere. That is what makes an answer arriving after a
    /// stop harmless: the frame it would have been delivered into may already be gone.
    struct ResolveSlot
    {
        std::mutex mutex;
        ResolveResult result { std::vector<ResolvedEndpoint> {} };
        async::ParkedWork waiter {};
        bool done = false;
        bool cancelled = false;
    };

    /// Hands @p waiter back to @p loop, or frees it where there is nowhere to hand it.
    ///
    /// An `async::detail::Parked` rather than a bare `ParkedWork`, so the branch that hands
    /// nothing on FREES an unowned chain instead of dropping it. That is `ParkedWork`'s contract —
    /// resumed or freed, never neither. Nothing changes for a BORROWED handle: the claim is empty,
    /// so the guard destroys nothing and the caller's owner still frees it.
    /// @param waiter What was taken out of the slot; may be empty.
    /// @param loop Where to hand it back, or null.
    void handBack(async::detail::Parked waiter, EventLoop* loop)
    {
        if (waiter.handle() && loop != nullptr)
            loop->submit(waiter.take());
    }

    /// Publishes a result and hands the waiter back to its loop.
    ///
    /// **It never calls `resume()`.** The consumer must continue on the loop's thread and not on
    /// whichever worker happened to finish the lookup — that is the entire point of the hand-back,
    /// and resuming here would run the caller's continuation on a resolver thread, which is the
    /// stall this class exists to remove wearing a different hat.
    /// @param slot The rendezvous to fill.
    /// @param loop Where to hand the waiter back, or null when there is nowhere.
    /// @param result What the lookup produced.
    void settle(ResolveSlot& slot, EventLoop* loop, ResolveResult result)
    {
        auto waiter = async::detail::Parked {};
        {
            auto const guard = std::scoped_lock { slot.mutex };
            slot.result = std::move(result);
            slot.done = true;
            waiter = async::detail::Parked { std::exchange(slot.waiter, async::ParkedWork {}) };
        }
        handBack(std::move(waiter), loop);
    }

    /// The stop callback of a parked lookup: takes the waiter back from the worker and hands it
    /// to the loop, cancelled.
    ///
    /// **It may run on any thread** — the loop's, a whenAny sibling's, a watchdog's — so it does
    /// what @c settle does and nothing more: it takes the waiter under the slot's lock and
    /// SUBMITS it. It never resumes, for @c settle's reason: the flow continues on the loop's
    /// thread and on no other.
    ///
    /// A named functor holding two plain pointers rather than a `std::function`, so registering
    /// the callback allocates nothing. The slot outlives it: the awaiter that owns this callback
    /// also holds the slot's `shared_ptr`, and drops the callback first.
    struct CancelLookup
    {
        ResolveSlot* slot = nullptr;
        EventLoop* loop = nullptr;

        void operator()() const
        {
            auto waiter = async::detail::Parked {};
            {
                auto const guard = std::scoped_lock { slot->mutex };
                // An answer that got here first is delivered, and a stop after it changes nothing:
                // the flow is already on its way back with a result it can use.
                if (slot->done)
                    return;
                slot->cancelled = true;
                waiter = async::detail::Parked { std::exchange(slot->waiter, async::ParkedWork {}) };
            }
            handBack(std::move(waiter), loop);
        }
    };

    /// Suspends until a slot is filled, or until the awaiting flow is stopped.
    ///
    /// Race-free the way @c ResultAwaitable is: the completion check and the waiter registration
    /// happen under one lock, so a result landing between them resumes through the normal path
    /// rather than parking on an answer that has already arrived.
    ///
    /// **Stop-aware, because @c IAsyncAddressResolver says a suspending resolver must be.** The
    /// lookup itself cannot be interrupted — `getaddrinfo` has no cancellation — but the WAIT for
    /// it can, and that wait is what a dial's budget, a `whenAny` and a loop's shutdown all need
    /// to end. On a stop the flow is handed back cancelled and throws; the worker finishes in its
    /// own time and publishes into a slot nobody is waiting on.
    struct SlotPark
    {
        std::shared_ptr<ResolveSlot> slot;
        EventLoop* loop = nullptr;
        std::optional<async::StopCallback<CancelLookup>> cancelReg {};

        /// @return False. Whether the answer is already there is @c await_suspend's first question,
        ///         asked under the slot's mutex: taking a lock is a call, and MSVC 19.44's ARM64
        ///         code generator drops the enclosing `try` of a `co_await` on a temporary awaiter
        ///         whose `await_ready` makes one (fastcached#1546, `.agent/rules/async-and-net.md`).
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// Templated on the promise so `parkedWorkFor` can ask the PARKING coroutine's own
        /// promise whether anything else owns its chain: @c settle hands this chain to a loop
        /// that may be destroyed before it runs it, and by then the handle is erased, so this is
        /// the last place the question can be asked. The same promise is where the flow's stop
        /// token is read.
        ///
        /// **An answer already there resumes at once, before the stop callback exists**, which is
        /// what `await_ready` answered when it held that check: a stop that has already fired
        /// cannot then turn a finished lookup into a cancellation.
        ///
        /// **The stop callback is registered BEFORE the waiter is published.** A stop landing in
        /// between then finds no waiter, records @c ResolveSlot::cancelled, and the check below
        /// sees it and does not park; registered after, a stop that had already fired would run
        /// the callback inline, find the waiter, and hand it to the loop while this function is
        /// still deciding whether to suspend — the same frame queued and returned-into at once.
        /// @tparam Promise The suspending coroutine's promise type.
        /// @param handle The suspended lookup.
        /// @return True to stay suspended; false when the answer or the stop arrived first.
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            {
                auto const guard = std::scoped_lock { slot->mutex };
                if (slot->done)
                    return false;
            }
            if constexpr (async::HasStopToken<Promise>)
                cancelReg.emplace(handle.promise().stopToken(),
                                  CancelLookup { .slot = slot.get(), .loop = loop });

            auto const guard = std::scoped_lock { slot->mutex };
            if (slot->done || slot->cancelled)
                return false;
            slot->waiter = async::detail::parkedWorkFor(handle);
            return true;
        }

        /// @return The lookup's answer.
        /// @throws async::OperationCancelled if the flow was stopped before the answer arrived.
        [[nodiscard]] ResolveResult await_resume()
        {
            // Dropped first: it waits for a callback running on another thread to finish, so the
            // slot is not read while that callback is still writing it.
            cancelReg.reset();
            auto const guard = std::scoped_lock { slot->mutex };
            if (slot->cancelled)
                throw async::OperationCancelled {};
            return slot->result;
        }
    };

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
        std::shared_ptr<ResolveSlot> slot;
        EventLoop* loop = nullptr;
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

            auto resolved = inner.resolve(job.host, job.port);
            if (resolved.has_value())
                settle(*job.slot, job.loop, std::move(*resolved));
            else
                settle(*job.slot,
                       job.loop,
                       std::unexpected(resolveFailure(job.host, job.port, resolved.error())));
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
        settle(*job.slot,
               job.loop,
               std::unexpected(
                   makeNetError(NetErrorCode::Cancelled, 0, "the resolver stopped before the lookup ran")));

    _impl->joinWorkers();
}

async::Task<ResolveResult> ThreadedAddressResolver::resolve(std::string host,
                                                            std::uint16_t port,
                                                            EventLoop* loop)
{
    // A literal needs no lookup, so it needs no thread. See the class comment: here that is the
    // common case, not the exotic one.
    //
    // The null-loop arm is a correctness requirement rather than a second optimisation: with
    // nowhere to submit a result back to, offloading would park a coroutine nothing could ever
    // resume.
    if (loop == nullptr || detail::isNumericHost(host))
    {
        auto resolved = _impl->inner.resolve(host, port);
        if (!resolved.has_value())
            co_return std::unexpected(resolveFailure(host, port, resolved.error()));
        co_return std::move(*resolved);
    }

    auto slot = std::make_shared<ResolveSlot>();
    {
        auto const guard = std::scoped_lock { _impl->mutex };
        if (_impl->stopping)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "the resolver is stopping"));

        if (_impl->queue.size() >= _impl->options.maxQueueDepth)
        {
            _impl->refusedCount.fetch_add(1, std::memory_order_relaxed);
            co_return std::unexpected(
                makeNetError(NetErrorCode::WouldBlock,
                             0,
                             std::format("the resolver queue is full ({} waiting); not resolving {}:{}",
                                         _impl->queue.size(),
                                         host,
                                         port)));
        }

        _impl->ensureStarted();
        _impl->queue.push_back(
            Impl::Job { .slot = slot, .loop = loop, .host = std::move(host), .port = port });
        _impl->offloadedCount.fetch_add(1, std::memory_order_relaxed);
    }
    _impl->wake.notify_one();

    co_return co_await SlotPark { .slot = std::move(slot), .loop = loop };
}

ThreadedAddressResolver& defaultAsyncResolver()
{
    static ThreadedAddressResolver resolver;
    return resolver;
}

} // namespace core::net
