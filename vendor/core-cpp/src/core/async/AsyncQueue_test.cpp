// SPDX-License-Identifier: Apache-2.0
#include <core/async/AsyncQueue.hpp>
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using core::async::AsyncQueue;
using core::async::AsyncQueueAdmission;
using core::async::AsyncQueueOptions;
using core::async::AsyncQueueOverflow;
using core::async::DetachedTask;
using core::async::IExecutor;
using core::async::OperationCancelled;
using core::async::ParkedWork;
using core::async::StopSource;
using core::async::Task;

namespace
{

using Queue = AsyncQueue<int>;

// fastcached#1546: MSVC 19.44's ARM64 code generator drops the enclosing `try` of a `co_await` on
// a temporary awaiter whose `await_ready` makes a call, so an `OperationCancelled` from
// `await_resume` passes every handler. An `await_ready` here answers a constant and the decision
// is `await_suspend`'s (.agent/rules/async-and-net.md); a call put back fails to compile wherever
// the question can be asked at compile time (core::async::awaitReadyIsConstantFalse).
static_assert(core::async::awaitReadyIsConstantFalse<Queue::PopAwaiter>());

/// An executor that queues and never runs anything by itself, so a case decides when — and
/// whether — parked work is resumed. `ParkedWork_test.cpp` has the same double for the same
/// reason; `core::async` has no shared one, and the loop that will be it (`core::net`'s
/// `testing::TestLoop`) is a layer above.
class QueuedExecutor final: public IExecutor
{
  public:
    using IExecutor::submit;

    void submit(std::coroutine_handle<> handle) override
    {
        _parked.emplace_back(ParkedWork { .resume = handle });
    }

    void submit(ParkedWork work) override { _parked.emplace_back(work); }

    /// @return How many posts are held right now.
    [[nodiscard]] std::size_t pending() const noexcept { return _parked.size(); }

    /// Resumes everything queued, in order.
    /// @return How many entries were run.
    std::size_t drain()
    {
        auto ran = std::size_t { 0 };
        // Taken wholesale, because resuming a coroutine can park again.
        auto batch = std::exchange(_parked, {});
        for (auto& entry: batch)
        {
            entry.resume();
            ++ran;
        }
        return ran;
    }

  private:
    std::vector<core::async::detail::Parked> _parked;
};

/// How a consumer stopped, if it has.
enum class ConsumerEnd : std::uint8_t
{
    Running = 0, ///< Still consuming, or never started.
    Closed,      ///< It saw the queue close and returned.
    Cancelled,   ///< It unwound on OperationCancelled.
};

/// What a case counts about its consumer.
struct Consumed
{
    std::vector<int> seen;                    ///< Every value the consumer took, in order.
    ConsumerEnd end { ConsumerEnd::Running }; ///< How it stopped, if it has.
};

/// Consumes until the queue closes or the flow is cancelled, recording what it saw. The shape
/// every real consumer has, so the cases exercise the path production does.
Task<void> consume(Queue* queue, Consumed* out)
{
    try
    {
        while (true)
        {
            auto item = co_await queue->pop();
            if (!item.has_value())
                break;
            out->seen.push_back(*item);
        }
        out->end = ConsumerEnd::Closed;
    }
    catch (OperationCancelled const&)
    {
        out->end = ConsumerEnd::Cancelled;
    }
}

/// A frame sentinel, counted when the frame carrying it dies.
class FrameSentinel
{
  public:
    explicit FrameSentinel(int* destroyed) noexcept: _destroyed(destroyed) {}

    FrameSentinel(FrameSentinel&& other) noexcept: _destroyed(std::exchange(other._destroyed, nullptr)) {}

    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_destroyed != nullptr)
            ++*_destroyed;
    }

  private:
    int* _destroyed;
};

/// A consumer chain owned by NOBODY, parked on an empty queue.
DetachedTask consumeDetached(Queue* queue, FrameSentinel sentinel, int* parked)
{
    (void) sentinel;
    ++*parked;
    std::ignore = co_await queue->pop();
}

/// The same shape, owned by the `Task` the caller holds. The control.
Task<void> consumeOwned(Queue* queue, FrameSentinel sentinel, int* parked)
{
    (void) sentinel;
    ++*parked;
    std::ignore = co_await queue->pop();
}

} // namespace

TEST_CASE("A push while the consumer is parked does not resume it inline", "[AsyncQueue]")
{
    // THE invariant. A producer commonly pushes while holding a lock of its own, so a queue that
    // resumed the consumer from inside push() would run the consumer's next step under that lock,
    // on the producer's thread, and deadlock the moment the consumer touched the producer back.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    executor.submit(consumer.handle());
    std::ignore = executor.drain();
    REQUIRE(queue.hasWaiter());

    auto const pushed = queue.push(7);
    CHECK(pushed.admission == AsyncQueueAdmission::Accepted);
    CHECK(pushed.displaced == 0);

    // Not yet: the handle went to the executor, not to a resume().
    CHECK(out.seen.empty());
    CHECK(executor.pending() == 1);

    std::ignore = executor.drain();
    REQUIRE(out.seen == std::vector { 7 });

    queue.close();
    std::ignore = executor.drain();
    CHECK(out.end == ConsumerEnd::Closed);
    CHECK_FALSE(queue.hasWaiter());
}

TEST_CASE("A pop with an item already queued does not suspend", "[AsyncQueue]")
{
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };

    CHECK(queue.push(1).admission == AsyncQueueAdmission::Accepted);
    CHECK(queue.push(2).admission == AsyncQueueAdmission::Accepted);

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    executor.submit(consumer.handle());
    std::ignore = executor.drain();

    // Both taken without ever parking, and then parked on the empty queue.
    CHECK(out.seen == std::vector { 1, 2 });
    CHECK(queue.hasWaiter());

    queue.close();
    std::ignore = executor.drain();
    CHECK(out.end == ConsumerEnd::Closed);
}

TEST_CASE("close() wakes a parked consumer at once and discards what is held", "[AsyncQueue]")
{
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    executor.submit(consumer.handle());
    std::ignore = executor.drain();

    CHECK(queue.push(1).admission == AsyncQueueAdmission::Accepted);
    CHECK(queue.push(2).admission == AsyncQueueAdmission::Accepted);
    // Closed before the consumer runs: what is queued is dropped rather than drained, so teardown
    // does not depend on the depth or on the consumer's backpressure.
    queue.close();
    std::ignore = executor.drain();

    CHECK(out.seen.empty());
    CHECK(out.end == ConsumerEnd::Closed);
    CHECK(queue.size() == 0);
    CHECK(queue.isClosed());
}

TEST_CASE("A push after close is refused and is not counted as displaced", "[AsyncQueue]")
{
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };

    queue.close();
    auto const pushed = queue.push(1);
    CHECK(pushed.admission == AsyncQueueAdmission::Refused);
    // Refused is not the same fact as displaced: one says the queue is gone, the other that it
    // was full, and an operator does something different about each.
    CHECK(pushed.displaced == 0);
    CHECK(queue.displaced() == 0);
}

TEST_CASE("DropOldest keeps the newest and reports what it displaced", "[AsyncQueue]")
{
    auto executor = QueuedExecutor {};
    auto queue =
        Queue { executor, AsyncQueueOptions { .capacity = 2, .overflow = AsyncQueueOverflow::DropOldest } };

    CHECK(queue.push(1).displaced == 0);
    CHECK(queue.push(2).displaced == 0);
    auto const third = queue.push(3);
    CHECK(third.admission == AsyncQueueAdmission::Accepted);
    CHECK(third.displaced == 1);
    CHECK(queue.size() == 2);
    CHECK(queue.displaced() == 1);

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    executor.submit(consumer.handle());
    std::ignore = executor.drain();
    // The oldest went, and order is otherwise preserved.
    CHECK(out.seen == std::vector { 2, 3 });

    queue.close();
    std::ignore = executor.drain();
}

TEST_CASE("DropNewest refuses the push instead", "[AsyncQueue]")
{
    // The other row of the table, so the enum is not one used value and one decorative one.
    auto executor = QueuedExecutor {};
    auto queue =
        Queue { executor, AsyncQueueOptions { .capacity = 2, .overflow = AsyncQueueOverflow::DropNewest } };

    CHECK(queue.push(1).admission == AsyncQueueAdmission::Accepted);
    CHECK(queue.push(2).admission == AsyncQueueAdmission::Accepted);
    auto const third = queue.push(3);
    CHECK(third.admission == AsyncQueueAdmission::Refused);
    CHECK(third.displaced == 1);
    CHECK(queue.size() == 2);

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    executor.submit(consumer.handle());
    std::ignore = executor.drain();
    CHECK(out.seen == std::vector { 1, 2 });

    queue.close();
    std::ignore = executor.drain();
}

TEST_CASE("A push landing after the awaiter is made and before it suspends does not park", "[AsyncQueue]")
{
    // A producer can slip in between the moment a `pop()` awaiter exists and the moment it asks
    // whether to park. Parking then would be a park nothing ever wakes: the push that would have
    // woken it has already happened and already found no waiter. The awaiter is driven directly
    // here because there is no other way to be inside that window. `await_ready` is not asked: it
    // is a constant `false` (fastcached#1546), and the question is `await_suspend`'s.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };

    auto awaiter = queue.pop();

    std::ignore = queue.push(42);

    // false == "do not suspend, resume through the normal path".
    CHECK_FALSE(awaiter.await_suspend(std::noop_coroutine()));
    CHECK_FALSE(queue.hasWaiter());

    auto const item = awaiter.await_resume();
    REQUIRE(item.has_value());
    CHECK(*item == 42);
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
// The cross-thread case is compiled where there is a second thread to run it on. Under
// single-threaded Emscripten there is none, and the rest of this file still covers the queue --
// `std::thread` rather than `std::jthread`, because `__cpp_lib_jthread` is exactly what
// `StopToken.hpp` has a fallback for.
TEST_CASE("A producer on another thread reaches a consumer on the executor", "[AsyncQueue]")
{
    // The headline property: a hand-off from a thread that is not the consumer's is the whole
    // point of this type.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    executor.submit(consumer.handle());
    std::ignore = executor.drain();
    REQUIRE(queue.hasWaiter());

    constexpr auto Count = std::size_t { 256 };
    {
        auto producer = std::thread { [&queue, Count] {
            for (auto const i: std::views::iota(std::size_t { 0 }, Count))
                std::ignore = queue.push(static_cast<int>(i));
        } };
        producer.join();
    }

    std::ignore = executor.drain();

    // FIFO across the hand-off, asserted as the whole sequence: a queue that lost or reordered
    // under contention would show up here rather than as a count that happens to match.
    auto expected = std::vector<int> {};
    expected.reserve(Count);
    for (auto const i: std::views::iota(std::size_t { 0 }, Count))
        expected.push_back(static_cast<int>(i));
    REQUIRE(out.seen == expected);

    queue.close();
    std::ignore = executor.drain();
    CHECK(out.end == ConsumerEnd::Closed);
    CHECK_FALSE(queue.hasWaiter());
}
#endif

TEST_CASE("A cancelled consumer unwinds out of a pop that has nothing to give", "[AsyncQueue]")
{
    // The stop-aware half. A consumer parked on a live queue whose own flow is cancelled has to
    // come back -- and it comes back by unwinding, because the queue said nothing: it is the
    // flow's own token, which `.agent/rules/async-and-net.md` says throws.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };
    auto source = StopSource {};

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    consumer.handle().promise().setStopToken(source.get_token());
    executor.submit(consumer.handle());
    std::ignore = executor.drain();
    REQUIRE(queue.hasWaiter());

    source.request_stop();

    // The stop callback handed the park back to the executor rather than resuming it there: a
    // callback runs on whatever thread requested the stop, and this type resumes nobody inline.
    CHECK(executor.pending() == 1);
    CHECK_FALSE(queue.hasWaiter());
    CHECK(out.end != ConsumerEnd::Cancelled);

    std::ignore = executor.drain();
    CHECK(out.end == ConsumerEnd::Cancelled);
    CHECK(out.end != ConsumerEnd::Closed);
    CHECK(out.seen.empty());
    // Still usable by its owner: cancelling a consumer is not closing the queue.
    CHECK_FALSE(queue.isClosed());
    CHECK_FALSE(queue.hasWaiter());
}

TEST_CASE("A pop on an already cancelled flow never parks", "[AsyncQueue]")
{
    // The other half of the window: a token stopped BEFORE the pop runs its callback inside the
    // registration, on this thread, so a park published first would be handed to the executor
    // from inside await_suspend -- which may resume a coroutine that has not suspended yet.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };
    auto source = StopSource {};
    source.request_stop();

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    consumer.handle().promise().setStopToken(source.get_token());

    consumer.handle().resume();

    CHECK(out.end == ConsumerEnd::Cancelled);
    CHECK_FALSE(queue.hasWaiter());
    CHECK(executor.pending() == 0);
}

TEST_CASE("An item already queued beats a cancellation", "[AsyncQueue]")
{
    // Data wins, for the reason a receive that already took bytes does: there is nowhere to put
    // the item back. The consumer takes it and only then sees the stop, on its next pop.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };
    auto source = StopSource {};

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    consumer.handle().promise().setStopToken(source.get_token());

    std::ignore = queue.push(7);
    source.request_stop();
    consumer.handle().resume();

    CHECK(out.seen == std::vector { 7 });
    CHECK(out.end == ConsumerEnd::Cancelled);
    CHECK(out.end != ConsumerEnd::Closed);
}

TEST_CASE("A closed queue answers a cancelled consumer with nullopt rather than a throw", "[AsyncQueue]")
{
    // close() is the resource saying "no more items, ever", which a consumer answers by
    // returning. Throwing there would make an ordinary shutdown -- close the queue, stop the
    // flow -- unwind, and a detached consumer's unhandled OperationCancelled ends the process.
    auto executor = QueuedExecutor {};
    auto queue = Queue { executor, AsyncQueueOptions {} };
    auto source = StopSource {};

    auto out = Consumed {};
    auto consumer = consume(&queue, &out);
    consumer.handle().promise().setStopToken(source.get_token());
    executor.submit(consumer.handle());
    std::ignore = executor.drain();
    REQUIRE(queue.hasWaiter());

    queue.close();
    source.request_stop();
    std::ignore = executor.drain();

    CHECK(out.end == ConsumerEnd::Closed);
    CHECK(out.end != ConsumerEnd::Cancelled);
}

TEST_CASE("A detached consumer posted by push and never dequeued is freed at teardown",
          "[AsyncQueue][ParkedWork]")
{
    // What the queue owes a chain it hands to an executor that nobody drains. `push()` and
    // `close()` post a handle, and an executor destroyed before the post is dequeued frees
    // nothing unless the post says what may be freed — which is correct for a consumer some
    // `Task` owns and a leak for a detached one, and the two are indistinguishable at the call
    // site unless the handle carries the answer
    // ([fastcached#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041)).
    auto destroyed = 0;
    auto parked = 0;
    {
        auto executor = QueuedExecutor {};
        auto queue = Queue { executor, AsyncQueueOptions {} };

        consumeDetached(&queue, FrameSentinel { &destroyed }, &parked);
        REQUIRE(parked == 1);
        REQUIRE(queue.hasWaiter());

        auto const pushed = queue.push(7);
        REQUIRE(pushed.admission == AsyncQueueAdmission::Accepted);
        REQUIRE_FALSE(queue.hasWaiter());
        // The executor is destroyed WITHOUT draining, so the post is never dequeued.
    }
    CHECK(destroyed == 1);
}

TEST_CASE("A detached consumer posted by close and never dequeued is freed at teardown",
          "[AsyncQueue][ParkedWork]")
{
    auto destroyed = 0;
    auto parked = 0;
    {
        auto executor = QueuedExecutor {};
        auto queue = Queue { executor, AsyncQueueOptions {} };

        consumeDetached(&queue, FrameSentinel { &destroyed }, &parked);
        REQUIRE(parked == 1);

        // The other site, and the stop path rather than the hot one: a queue closed while its
        // consumer is parked is the ordinary way a sender ends.
        queue.close();
        REQUIRE_FALSE(queue.hasWaiter());
    }
    CHECK(destroyed == 1);
}

TEST_CASE("A consumer some Task owns is left alone by the executor", "[AsyncQueue][ParkedWork]")
{
    // The control, and mandatory rather than decoration: freeing everything at teardown is as
    // wrong as freeing nothing. This frame is owned by the `Task` below, so an executor that
    // freed what it merely borrows would double-free here.
    auto destroyed = 0;
    auto parked = 0;
    {
        auto executor = QueuedExecutor {};
        auto queue = Queue { executor, AsyncQueueOptions {} };

        auto consumer = consumeOwned(&queue, FrameSentinel { &destroyed }, &parked);
        executor.submit(consumer.handle());
        std::ignore = executor.drain();
        REQUIRE(parked == 1);
        REQUIRE(queue.hasWaiter());

        auto const pushed = queue.push(7);
        REQUIRE(pushed.admission == AsyncQueueAdmission::Accepted);
        // The executor is destroyed with the post undrained, exactly as above -- but this frame
        // has an owner, so it must survive until `consumer` goes out of scope.
        CHECK(destroyed == 0);
    }
    // Freed exactly once, by the Task that owns it.
    CHECK(destroyed == 1);
}
