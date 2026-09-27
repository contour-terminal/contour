// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `AsyncQueue<T>` — a queue a coroutine parks on and any thread pushes to.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace core::async
{

/// What a full @c AsyncQueue gives up to make room.
///
/// At namespace scope rather than nested in the template, so a defaulted parameter can name it
/// without the enclosing type being complete.
enum class AsyncQueueOverflow : std::uint8_t
{
    /// Discard the front. Right where the newest item subsumes the older ones — a status update
    /// supersedes every earlier one for that peer, so what is queued behind a dead connection is
    /// stale by definition.
    DropOldest = 0,

    /// Discard the item being pushed. Right where the queue holds a sequence whose prefix is what
    /// matters.
    DropNewest,
};

/// Whether an item entered the queue.
enum class AsyncQueueAdmission : std::uint8_t
{
    Refused = 0, ///< The queue is closed, or it was full and drops the newest.
    Accepted,    ///< The item is in the queue.
};

/// How much an @c AsyncQueue holds and what it does when it is full.
struct AsyncQueueOptions
{
    /// Largest number of items held before @c overflow applies; 0 is unbounded.
    ///
    /// A bound, not a target. Unbounded is offered because some producers are self-limiting, but
    /// it is not the default: a peer that is down for an hour, accumulating an hour of
    /// heartbeats, is the shape of leak a bound exists to make impossible to write by omission.
    std::size_t capacity { 0 };

    /// Which end is sacrificed when @c capacity is reached.
    AsyncQueueOverflow overflow { AsyncQueueOverflow::DropOldest };
};

/// What one `push()` did.
struct AsyncQueuePush
{
    /// How many items this push cost the queue; 0 in the ordinary case.
    ///
    /// Under @ref AsyncQueueOverflow::DropOldest those are the items evicted to make room, so the
    /// count is what the queue gave up and @ref admission is @c Accepted. Under
    /// @ref AsyncQueueOverflow::DropNewest nothing is evicted and the item lost is @b this one, so
    /// the count is 1 against a @c Refused admission. Both are one item dropped, which is what a
    /// caller adding this to a loss counter is asking about; the admission says which end went.
    ///
    /// Returned rather than only counted internally, because the caller is the one holding the
    /// counter an operator reads and the context to name what was lost. A silent drop is
    /// invisible: a protocol that recovers from loss looks healthy while running slower than it
    /// should.
    std::size_t displaced { 0 };

    /// Whether the value entered the queue.
    AsyncQueueAdmission admission { AsyncQueueAdmission::Refused };
};

/// A queue a coroutine parks on and any thread pushes to.
///
/// What replaces `std::mutex` + `std::condition_variable` + `std::deque` at the boundary between
/// a thread that produces and a coroutine that consumes. A condition variable parks a *thread*;
/// once the consumer lives on an event loop there is no thread of its own to park, and blocking
/// one would stall every other coroutine sharing that loop.
///
/// ## The one invariant everything else rests on
///
/// **`push()` and `close()` never resume the consumer. They hand its handle to
/// @c IExecutor::submit and return.** Not an optimisation — it is what makes the type usable at
/// all. A producer commonly pushes while holding a lock of its own, and a queue that resumed
/// inline would run the consumer's next step inside that lock, on the producer's thread, at a
/// point where the consumer may call back into the producer's object.
///
/// ## Where the consumer resumes
///
/// **On the executor it was running on when it parked**, read from the current-executor context
/// (`ExecutorContext.hpp`) in `pop()`'s `await_suspend`: a consumer on an `EventLoop` comes back
/// to that loop, one on a `Strand` to that strand. Only a consumer that parked outside every
/// executor's task -- driven by hand, or by an executor that does not state itself -- comes back
/// on the executor this queue was constructed over. The push, the close and the stop callback all
/// resume it the same way. Before 0.4.0 it was always this queue's executor, which sent a consumer
/// that parked on a strand back off it (found by morph, PR #806).
///
/// ## Single consumer, many producers
///
/// One waiter slot. A second concurrent consumer is a programmer error rather than a runtime
/// condition, so it is asserted rather than handled. Many producers, because an item can be
/// handed over from a timer, from another coroutine on the same loop, and from whatever else
/// holds a reference.
///
/// ## Lifetime: the queue owns no coroutine frame, and cannot
///
/// It never destroys a frame and never keeps one alive. `close()` hands the parked consumer back
/// to the executor; the executor resumes it; the consumer observes `std::nullopt` and returns;
/// only then is its frame gone. So an owner must **observe** its consumer finishing before
/// destroying the queue — a counter, not an assumption. `~AsyncQueue` asserts against a waiter
/// still being registered, and `hasWaiter()` lets a test assert it in a release build too.
///
/// What the queue does carry is the park's ownership: the handle it hands to the executor is a
/// @c ParkedWork built by `detail::parkedWorkFor`, so an executor destroyed before it dequeues
/// the post frees a detached consumer's chain rather than leaking it, and leaves a consumer some
/// `Task` owns alone
/// ([fastcached#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041)).
///
/// ## Closing discards
///
/// One behaviour, not a policy. A consumer that had to drain N items during teardown would make
/// shutdown depend on the queue depth and on its own backpressure, which is the one thing
/// shutdown must not depend on. A caller that wants everything delivered stops pushing and lets
/// the consumer see an empty queue.
///
/// @tparam T Item type. Moved in and out; required nothrow-move-constructible so the critical
///         section cannot throw with the waiter half-extracted.
template <typename T>
    requires std::is_nothrow_move_constructible_v<T>
class AsyncQueue final
{
  public:
    /// Constructs over the executor a consumer is resumed on when it parked outside every
    /// executor's task.
    /// @param executor Where a woken consumer is posted when no executor was current as it parked.
    ///        Must outlive this queue and every producer that can reach it.
    /// @param options Capacity and overflow policy.
    AsyncQueue(IExecutor& executor, AsyncQueueOptions options) noexcept:
        _executor(executor), _options(options)
    {
    }

    AsyncQueue(AsyncQueue const&) = delete;
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue const&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    /// Asserts that no consumer is parked. See the lifetime section.
    ~AsyncQueue() { assert(!_waiter.resume && "AsyncQueue destroyed with a consumer still parked on it"); }

    /// Offers an item, from any thread.
    ///
    /// Never blocks beyond this queue's own mutex, never waits for the consumer, and never fails
    /// for want of space — it displaces instead, and says how much. A closed queue refuses.
    /// @param value The item; moved.
    /// @return Whether it was accepted, and what it cost.
    ///
    /// `[[nodiscard]]`, because the result is the only report of a drop: this class exists to make
    /// loss visible, and a discarded return is exactly the silent drop it was written against.
    [[nodiscard]] AsyncQueuePush push(T value)
    {
        auto waiter = ParkedWork {};
        auto target = ResumeTarget {};
        auto outcome = AsyncQueuePush {};
        {
            auto const guard = std::scoped_lock { _mutex };
            if (_closed.load(std::memory_order_relaxed))
                return outcome;

            if (_options.capacity != 0 && _items.size() >= _options.capacity)
            {
                if (_options.overflow == AsyncQueueOverflow::DropNewest)
                {
                    _displaced.fetch_add(1, std::memory_order_relaxed);
                    return AsyncQueuePush { .displaced = 1, .admission = AsyncQueueAdmission::Refused };
                }
                while (_items.size() >= _options.capacity)
                {
                    _items.pop_front();
                    ++outcome.displaced;
                }
                _displaced.fetch_add(outcome.displaced, std::memory_order_relaxed);
            }

            _items.push_back(std::move(value));
            outcome.admission = AsyncQueueAdmission::Accepted;
            // Exactly one of a push, a close or a cancellation can ever obtain a given handle,
            // which is what makes a double resume inexpressible.
            waiter = std::exchange(_waiter, {});
            target = std::exchange(_waiterTarget, {});
        }

        // Outside the lock: the handle is already ours and nobody else can see it, and an
        // executor's submit may perform a syscall. A producer holding a lock of its own across
        // that would serialise its own hot path behind it.
        if (waiter.resume)
            target.submit(std::move(waiter));
        return outcome;
    }

    /// Wakes the consumer with "no more items, ever", and discards what is held.
    ///
    /// Idempotent, safe from any thread, and safe before a consumer exists. This is the stop path:
    /// a consumer parked on `pop()` is resumed at once rather than after a timeout, which is the
    /// property a bounded wait cannot give.
    void close() noexcept
    {
        auto waiter = ParkedWork {};
        auto target = ResumeTarget {};
        {
            auto const guard = std::scoped_lock { _mutex };
            _closed.store(true, std::memory_order_release);
            _items.clear();
            waiter = std::exchange(_waiter, {});
            target = std::exchange(_waiterTarget, {});
        }
        if (waiter.resume)
            target.submit(std::move(waiter));
    }

    /// @return Whether `close()` has been called.
    [[nodiscard]] bool isClosed() const noexcept { return _closed.load(std::memory_order_acquire); }

    /// @return How many items are held right now. For tests and metrics; racy by nature and never
    ///         a basis for a decision.
    [[nodiscard]] std::size_t size() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _items.size();
    }

    /// @return Whether a consumer is currently parked on this queue.
    ///
    /// Exposed for one purpose: a teardown test asserting that no coroutine frame was left
    /// suspended. A production caller reading this is asking a question whose answer is stale
    /// before it returns.
    [[nodiscard]] bool hasWaiter() const noexcept
    {
        auto const guard = std::scoped_lock { _mutex };
        return static_cast<bool>(_waiter.resume);
    }

    /// @return Cumulative items lost to overflow across this queue's life, evicted or refused.
    [[nodiscard]] std::uint64_t displaced() const noexcept
    {
        return _displaced.load(std::memory_order_relaxed);
    }

    class PopAwaiter;

    /// Takes the next item, suspending until one arrives, the queue closes, or the awaiting
    /// coroutine's own stop token is requested.
    ///
    /// `co_await queue.pop()` yields `std::optional<T>`: a value, or `std::nullopt` meaning the
    /// queue closed and the consumer should stop. Exactly one consumer may have an outstanding
    /// `pop()` at a time.
    /// @return An awaitable resolving to the next item or `std::nullopt`.
    /// @throws OperationCancelled if the awaiting coroutine's stop token is requested while it
    ///         waits, and neither an item nor a close arrived first.
    [[nodiscard]] PopAwaiter pop() noexcept { return PopAwaiter { this }; }

    /// Awaiter produced by `pop()`. Not constructed directly.
    class PopAwaiter final
    {
      public:
        /// @param queue The queue to take from; never null.
        explicit PopAwaiter(AsyncQueue* queue) noexcept: _queue(queue) {}

        PopAwaiter(PopAwaiter const&) = delete;
        PopAwaiter(PopAwaiter&&) = delete;
        PopAwaiter& operator=(PopAwaiter const&) = delete;
        PopAwaiter& operator=(PopAwaiter&&) = delete;
        ~PopAwaiter() = default;

        /// @return False. Whether an item or a close is already there is @c await_suspend's
        ///         question, asked under the mutex: taking a lock is a call, and MSVC 19.44's ARM64
        ///         code generator drops the enclosing `try` of a `co_await` on a temporary awaiter
        ///         whose `await_ready` makes one
        ///         ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)).
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// Registers @p awaiting as this queue's single waiter, and its stop token as what can
        /// take the park back.
        ///
        /// The first block is what `await_ready` used to ask: an item or a close already there
        /// resumes at once, before the token is read or a callback registered, so a stopped flow
        /// still takes an item that was waiting for it. Returns `bool` rather than `void` because
        /// that check and the park are two separate acquisitions of the mutex, so a push can land
        /// between them. Re-checking under the lock and answering false resumes through the normal
        /// path instead of parking on an item that is already there — a park nothing would ever
        /// wake, because the push that would have woken it has already happened.
        ///
        /// Where the consumer will be resumed is read here, once, from the current-executor context
        /// -- the executor running the consumer now -- with the queue's own executor as the
        /// fallback, and it travels with the park to whichever of a push, a close or the stop
        /// callback takes it.
        ///
        /// The stop callback is registered BEFORE the park is published, and outside the queue's
        /// mutex. A token that is already stopped runs the callback in its constructor, on this
        /// thread, and that callback takes the mutex; registering first means such an inline run
        /// finds no waiter to hand back and only records the cancellation, which the re-check
        /// below reads. The alternative — publishing the park first — would let the callback
        /// submit the handle to an executor that may resume it before `await_suspend` has
        /// returned.
        /// @tparam Promise The awaiting coroutine's promise type.
        /// @param awaiting The suspended consumer.
        /// @return True to stay suspended; false where an item, a close or a cancellation got
        ///         there first.
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
        {
            {
                auto const guard = std::scoped_lock { _queue->_mutex };
                if (!_queue->_items.empty() || _queue->_closed.load(std::memory_order_relaxed))
                    return false;
            }

            if constexpr (HasStopToken<Promise>)
                _token = awaiting.promise().stopToken();
            if (_token.stop_possible())
                _stopReg.emplace(_token, CancelPop { this });

            // Outside the lock: copying it may count a reference, and the lock is the producers'.
            auto target = ResumeTarget::currentOr(_queue->_executor);
            auto const guard = std::scoped_lock { _queue->_mutex };
            if (!_queue->_items.empty() || _queue->_closed.load(std::memory_order_relaxed) || _cancelled)
                return false;
            assert(!_queue->_waiter.resume
                   && "AsyncQueue supports one consumer; a second is a programmer error");
            _queue->_waiter = detail::parkedWorkFor(awaiting);
            _queue->_waiterTarget = std::move(target);
            return true;
        }

        /// @return The next item, or `std::nullopt` where the queue closed and the consumer
        ///         should end.
        /// @throws OperationCancelled where the awaiting coroutine's own token was requested and
        ///         neither an item nor a close answered first.
        ///
        /// The order is data, then close, then cancellation. Data wins because an item taken out
        /// of the queue has nowhere to be put back — the same reason a receive that already got
        /// bytes wins over a cancel (`.agent/rules/async-and-net.md`). Close beats cancellation
        /// because it is the resource saying "no more items, ever", which a consumer answers by
        /// returning; unwinding with an exception there would add nothing and would make an
        /// ordinary shutdown throw.
        [[nodiscard]] std::optional<T> await_resume()
        {
            auto const guard = std::scoped_lock { _queue->_mutex };
            if (!_queue->_items.empty())
            {
                auto value = std::move(_queue->_items.front());
                _queue->_items.pop_front();
                return value;
            }
            if (_queue->_closed.load(std::memory_order_relaxed))
                return std::nullopt;
            if (_cancelled || _token.stop_requested())
                throw OperationCancelled {};
            return std::nullopt;
        }

      private:
        /// The stop callback: records the cancellation and hands the parked consumer back to the
        /// executor it parked on, without closing the queue.
        ///
        /// A named functor rather than a lambda in a `StopCallback<std::function<void()>>`: one
        /// pointer of state needs neither an allocation nor an indirect call.
        class CancelPop
        {
          public:
            /// @param awaiter The awaiter whose park this takes back; never null.
            explicit CancelPop(PopAwaiter* awaiter) noexcept: _awaiter(awaiter) {}

            void operator()() const noexcept
            {
                auto waiter = ParkedWork {};
                auto target = ResumeTarget {};
                {
                    auto const guard = std::scoped_lock { _awaiter->_queue->_mutex };
                    _awaiter->_cancelled = true;
                    waiter = std::exchange(_awaiter->_queue->_waiter, {});
                    target = std::exchange(_awaiter->_queue->_waiterTarget, {});
                }
                // Outside the lock, for `push()`'s reason: the executor's own lock must never
                // nest under this queue's.
                if (waiter.resume)
                    target.submit(std::move(waiter));
            }

          private:
            PopAwaiter* _awaiter;
        };

        AsyncQueue* _queue;
        StopToken _token;          ///< The awaiting flow's own token (empty where it has none).
        bool _cancelled { false }; ///< Set by CancelPop under the queue's mutex; read under it too.

        /// Declared LAST, so it is destroyed FIRST: ~StopCallback waits for a callback running on
        /// another thread, and that callback reads the members above.
        std::optional<StopCallback<CancelPop>> _stopReg;
    };

  private:
    IExecutor& _executor;
    AsyncQueueOptions _options;

    /// Guards @c _items and @c _waiter, and orders the store to @c _closed. Never held across
    /// `IExecutor::submit`, so the executor's own lock is never nested under this one and no
    /// lock-order inversion is expressible.
    mutable std::mutex _mutex;

    std::deque<T> _items;
    ParkedWork _waiter {};

    /// Where @c _waiter is resumed: the executor it was running on as it parked, or @c _executor.
    /// Taken together with @c _waiter, always.
    ResumeTarget _waiterTarget {};

    /// Stored under @c _mutex with release and read outside it with acquire, so a loop condition
    /// can ask without taking the lock. The pairing means a reader that sees `true` also sees the
    /// cleared @c _items.
    std::atomic<bool> _closed { false };

    /// Relaxed: a monotone diagnostic, counted and never compared.
    std::atomic<std::uint64_t> _displaced { 0 };
};

} // namespace core::async
