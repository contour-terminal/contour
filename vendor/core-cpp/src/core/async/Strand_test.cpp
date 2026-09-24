// SPDX-License-Identifier: Apache-2.0
#include <core/async/AsyncQueue.hpp>
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Strand.hpp>
#include <core/async/StrandTestSupport.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #include <core/async/ThreadPoolExecutor.hpp>

    #include <chrono>
    #include <mutex>
    #include <ranges>
    #include <thread>
#endif

using core::async::currentExecutor;
using core::async::DetachedTask;
using core::async::ExecutorScope;
using core::async::IExecutor;
using core::async::OperationCancelled;
using core::async::ParkedWork;
using core::async::ResumeOn;
using core::async::ResumeTarget;
using core::async::StopSource;
using core::async::Strand;
using core::async::Task;
using core::async::testing::ManualExecutor;

namespace
{

using Queue = core::async::AsyncQueue<int>;

/// Where a coroutine found itself each time it looked: on the strand or not, and which executor
/// was current.
struct Sighting
{
    bool onStrand { false };
    IExecutor* current { nullptr };
};

/// Hops onto @p strand, records where it is, and ends.
Task<void> hopAndLook(Strand* strand, std::vector<Sighting>* out)
{
    out->push_back(Sighting { .onStrand = strand->runningHere(), .current = currentExecutor() });
    co_await ResumeOn { *strand };
    out->push_back(Sighting { .onStrand = strand->runningHere(), .current = currentExecutor() });
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

/// A detached flow that hops onto @p strand and records that it ran there.
DetachedTask detachedOnto(Strand* strand, FrameSentinel sentinel, int* ran)
{
    (void) sentinel;
    co_await ResumeOn { *strand };
    ++*ran;
}

/// The same, owned by the `Task` the caller holds.
Task<void> ownedOnto(Strand* strand, FrameSentinel sentinel, int* ran)
{
    (void) sentinel;
    co_await ResumeOn { *strand };
    ++*ran;
}

#if !defined(_MSC_VER) || defined(__clang__)
/// A coroutine whose `resume()` THROWS: its promise rethrows what escapes the body, which is what
/// a foreign coroutine type may do and none of core-cpp's does. The frame is left suspended at its
/// final point, so the owner destroys it.
class ThrowingResume
{
  public:
    struct promise_type
    {
        [[nodiscard]] ThrowingResume get_return_object() noexcept
        {
            return ThrowingResume { std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        [[noreturn]] void unhandled_exception() const { throw; }
    };

    explicit ThrowingResume(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ThrowingResume(ThrowingResume const&) = delete;
    ThrowingResume(ThrowingResume&&) = delete;
    ThrowingResume& operator=(ThrowingResume const&) = delete;
    ThrowingResume& operator=(ThrowingResume&&) = delete;
    ~ThrowingResume() { _handle.destroy(); }

    [[nodiscard]] std::coroutine_handle<> handle() const noexcept { return _handle; }

  private:
    std::coroutine_handle<promise_type> _handle;
};

/// The body of a @c ThrowingResume: runs once, on the strand, and throws.
ThrowingResume throwOnResume(Strand* strand, std::vector<Sighting>* out, bool really)
{
    out->push_back(Sighting { .onStrand = strand->runningHere(), .current = currentExecutor() });
    if (really)
        throw std::runtime_error { "a task that throws out of resume()" };
    co_return;
}
#endif

/// Records where it ran, on the strand it was submitted to.
Task<void> lookOnce(Strand* strand, std::vector<Sighting>* out)
{
    out->push_back(Sighting { .onStrand = strand->runningHere(), .current = currentExecutor() });
    co_return;
}

/// A task whose BODY throws: its own promise catches it, so the strand never sees it.
Task<void> bodyThrows(bool really)
{
    if (really)
        throw std::runtime_error { "caught by the Task's promise" };
    co_return;
}

/// How a consumer stopped, if it has.
enum class ConsumerEnd : std::uint8_t
{
    Running = 0, ///< Still consuming, or never started.
    Closed,      ///< It saw the queue close and returned.
    Cancelled,   ///< It unwound on OperationCancelled.
};

/// What a strand-bound consumer saw.
struct Consumed
{
    std::vector<int> seen;                    ///< Every value it took, in order.
    std::vector<bool> onStrand;               ///< For each value, whether it was on the strand.
    bool cancelledOnStrand { false };         ///< Whether an unwinding cancel resumed on the strand.
    ConsumerEnd end { ConsumerEnd::Running }; ///< How it stopped.
    std::atomic<bool> finished { false };     ///< Set last, so another thread may read the rest.
};

/// Hops onto @p strand, then pops until the queue closes or the flow is cancelled, recording
/// whether each resumption came back to the strand. The shape morph's handler had.
Task<void> consumeOnStrand(Strand* strand, Queue* queue, Consumed* out)
{
    co_await ResumeOn { *strand };
    try
    {
        while (true)
        {
            auto item = co_await queue->pop();
            if (!item.has_value())
                break;
            out->seen.push_back(*item);
            out->onStrand.push_back(strand->runningHere());
        }
        out->end = ConsumerEnd::Closed;
    }
    catch (OperationCancelled const&)
    {
        out->cancelledOnStrand = strand->runningHere();
        out->end = ConsumerEnd::Cancelled;
    }
    out->finished.store(true);
}

} // namespace

TEST_CASE("Outside every executor's task there is no current executor", "[Strand][context]")
{
    CHECK(currentExecutor() == nullptr);
    CHECK_FALSE(ResumeTarget::current());
    auto fallback = ManualExecutor {};
    CHECK(ResumeTarget::currentOr(fallback).executor() == &fallback);
}

namespace
{

/// Moves out of @p from, which a case then inspects: what a moved-from object promises.
template <typename T>
T moveOutOf(T& from)
{
    return std::move(from);
}

/// Move-assigns @p from to @p to.
template <typename T>
void moveAssign(T& to, T& from)
{
    to = std::move(from);
}

/// Hops back onto @p executor for ever: work that requeues itself on every resumption.
Task<void> requeueForever(ManualExecutor* executor)
{
    while (true)
        co_await ResumeOn { *executor };
}

} // namespace

TEST_CASE("A moved-from ResumeTarget names nothing", "[Strand][context]")
{
    // The implicit move moved the keep-alive out and copied the executor pointer: the source still
    // answered true, and a submit through it reached an executor it no longer kept alive.
    //
    // Reading the source after the move is the point of the case. It is held the way an owner holds
    // one, behind a pointer, which is also what keeps clang-analyzer's use-after-move check -- which
    // objects to any member call on a moved-from LOCAL -- from reading this deliberate read as a
    // mistake.
    auto executor = ManualExecutor {};
    auto const source = std::make_unique<ResumeTarget>(executor);
    auto const moved = moveOutOf(*source);
    CHECK(moved.executor() == &executor);
    CHECK_FALSE(*source);
    CHECK(source->executor() == nullptr);

    auto assigned = ResumeTarget {};
    auto other = ResumeTarget { executor };
    moveAssign(assigned, other);
    CHECK(assigned.executor() == &executor);
    CHECK_FALSE(other);
}

TEST_CASE("ManualExecutor::drain gives up after its bound on work that requeues itself for ever",
          "[ManualExecutor]")
{
    // A regression that requeues work for ever used to hang drain(), and with it the case; now it
    // fails the case, naming the bound.
    auto executor = ManualExecutor {};
    auto spinner = requeueForever(&executor);
    executor.submit(spinner.handle());
    CHECK_THROWS_AS(executor.drain(64), std::length_error);
    CHECK(executor.pending() == 1);
}

TEST_CASE("Nested executor scopes restore their predecessors, also when unwound by a throw",
          "[Strand][context]")
{
    auto outer = ManualExecutor {};
    auto middle = ManualExecutor {};
    auto inner = ManualExecutor {};
    {
        auto const a = ExecutorScope { outer };
        CHECK(currentExecutor() == &outer);
        {
            auto const b = ExecutorScope { middle };
            CHECK(currentExecutor() == &middle);
            try
            {
                auto const c = ExecutorScope { inner };
                CHECK(ExecutorScope::innermost() == &c);
                CHECK(currentExecutor() == &inner);
                CHECK(ResumeTarget::current().executor() == &inner);
                throw std::runtime_error { "unwinds the innermost scope" };
            }
            catch (std::runtime_error const&)
            {
                // The throw left through `c`'s destructor, which put `b` back.
                CHECK(currentExecutor() == &middle);
            }
            CHECK(currentExecutor() == &middle);
        }
        CHECK(currentExecutor() == &outer);
        // The chain is walkable, which is what `runningHere` asks.
        CHECK(ExecutorScope::innermost()->previous() == nullptr);
    }
    CHECK(currentExecutor() == nullptr);
}

TEST_CASE("co_await ResumeOn a strand hops onto it, and the strand is the current executor there", "[Strand]")
{
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto seen = std::vector<Sighting> {};

    auto task = hopAndLook(&strand, &seen);
    task.handle().resume();
    REQUIRE(seen.size() == 1);
    CHECK_FALSE(seen[0].onStrand);
    CHECK(seen[0].current == nullptr);

    // The hop queued the strand's pump on its base, and nothing ran it yet.
    CHECK(base.pending() == 1);
    std::ignore = base.drain();

    REQUIRE(seen.size() == 2);
    CHECK(seen[1].onStrand);
    // Some executor is current, and it is the strand's (its shared state, which outlives the
    // object -- so runningHere(), above, is the question, not a pointer comparison).
    CHECK(seen[1].current != nullptr);
    CHECK(task.done());
    // And the context is gone once the base returns.
    CHECK(currentExecutor() == nullptr);
    CHECK_FALSE(strand.runningHere());
}

TEST_CASE("A strand runs what it is given in FIFO order on its base", "[Strand]")
{
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto order = std::vector<int> {};

    auto tasks = std::vector<Task<void>> {};
    for (auto const index: { 0, 1, 2, 3, 4, 5, 6, 7 })
        tasks.push_back([](Strand* on, std::vector<int>* out, int i) -> Task<void> {
            co_await ResumeOn { *on };
            out->push_back(i);
        }(&strand, &order, index));
    for (auto& task: tasks)
        task.handle().resume();

    // One pump queued on the base for all eight, not one post per task.
    CHECK(base.pending() == 1);
    std::ignore = base.drain();
    CHECK(order == std::vector { 0, 1, 2, 3, 4, 5, 6, 7 });
}

TEST_CASE("A task that throws out of resume() neither wedges the strand nor loses the tasks behind it",
          "[Strand]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    // Under MSVC's cl the strand ends the process here instead (Strand.hpp), which a Catch case
    // cannot observe: core-cpp.strand-throw-canary is the process that asserts it.
    SKIP("under MSVC's cl a throw out of resume() on a strand ends the process; "
         "core-cpp.strand-throw-canary asserts it");
#else
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto seen = std::vector<Sighting> {};

    auto const thrower = throwOnResume(&strand, &seen, true);
    auto first = lookOnce(&strand, &seen);
    auto second = lookOnce(&strand, &seen);
    strand.submit(thrower.handle());
    strand.submit(first.handle());
    strand.submit(second.handle());

    // The exception reaches whoever drove the base -- the strand does not swallow it...
    CHECK_THROWS_AS(base.drain(), std::runtime_error);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].onStrand);
    // ...and the scope it ran under was unwound with it.
    CHECK(currentExecutor() == nullptr);

    // ...and the two tasks behind it still run, on the strand, in order.
    std::ignore = base.drain();
    REQUIRE(seen.size() == 3);
    CHECK(seen[1].onStrand);
    CHECK(seen[2].onStrand);
    CHECK(first.done());
    CHECK(second.done());

    // And the strand still takes work.
    auto third = lookOnce(&strand, &seen);
    strand.submit(third.handle());
    std::ignore = base.drain();
    CHECK(third.done());
    CHECK(seen.size() == 4);
#endif
}

TEST_CASE("A Task whose body throws on a strand hands the exception to its awaiter", "[Strand]")
{
    // The ordinary case, which needs nothing of the strand: the Task's promise catches what its
    // body throws, so resume() returns normally and the strand never sees it.
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto seen = std::vector<Sighting> {};

    auto thrower = bodyThrows(true);
    auto after = lookOnce(&strand, &seen);
    strand.submit(thrower.handle());
    strand.submit(after.handle());
    CHECK_NOTHROW(base.drain());
    CHECK(thrower.done());
    CHECK_THROWS_AS(thrower.result(), std::runtime_error);
    CHECK(after.done());
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].onStrand);
}

TEST_CASE("A strand's destructor drops queued work: what nobody owns is freed, what a Task owns is not",
          "[Strand][ParkedWork]")
{
    auto base = ManualExecutor {};
    auto detachedDestroyed = 0;
    auto ownedDestroyed = 0;
    auto ran = 0;
    {
        auto owned = std::optional<Task<void>> {};
        {
            auto strand = Strand { base };
            detachedOnto(&strand, FrameSentinel { &detachedDestroyed }, &ran);
            owned.emplace(ownedOnto(&strand, FrameSentinel { &ownedDestroyed }, &ran));
            owned->handle().resume();
            REQUIRE(base.pending() == 1);
            // Destroyed with both queued and its pump waiting on the base.
        }
        CHECK(detachedDestroyed == 1);
        CHECK(ownedDestroyed == 0);

        // The pump still runs when the base gets to it, finds its strand closed, and ends -- it
        // runs nothing it held.
        std::ignore = base.drain();
        CHECK(ran == 0);
        CHECK(base.pending() == 0);
        CHECK_FALSE(owned->done());
    }
    CHECK(ownedDestroyed == 1);
}

TEST_CASE("A strand destroyed before any work reached it, and one destroyed idle, free their pump",
          "[Strand]")
{
    // Nothing to assert beyond not leaking and not crashing, which the sanitizer legs check: the
    // idle pump holds the strand's state, so a destructor that forgot it would leak both.
    auto base = ManualExecutor {};
    {
        auto const unused = Strand { base };
    }
    auto seen = std::vector<Sighting> {};
    {
        auto strand = Strand { base };
        auto task = lookOnce(&strand, &seen);
        strand.submit(task.handle());
        std::ignore = base.drain();
        CHECK(task.done());
    }
    CHECK(base.pending() == 0);
    CHECK(seen.size() == 1);
}

TEST_CASE("A coroutine on a strand that awaits AsyncQueue::pop resumes on the strand, not on the "
          "queue's executor",
          "[Strand][AsyncQueue][context]")
{
    // morph's finding: the queue hands its consumer to the executor it was built over, so a
    // consumer that parked while on a strand came back on that one instead -- off the strand,
    // racing everything the strand serialises.
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto strand = Strand { base };
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto out = Consumed {};

    auto consumer = consumeOnStrand(&strand, &queue, &out);
    consumer.handle().resume();
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());

    // Pushed from outside every executor, so nothing but the park itself says where to go back.
    std::ignore = queue.push(7);
    CHECK(foreign.pending() == 0);
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    REQUIRE(out.seen == std::vector { 7 });
    CHECK(out.onStrand == std::vector { true });

    queue.close();
    std::ignore = base.drain();
    std::ignore = foreign.drain();
    CHECK(out.end == ConsumerEnd::Closed);
}

TEST_CASE("A coroutine on a strand parked on AsyncQueue::pop unwinds on the strand when its flow is "
          "stopped",
          "[Strand][AsyncQueue][context]")
{
    // The stop path posts from the stop callback, on whatever thread requested the stop: it has to
    // find the same way back as a push.
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto strand = Strand { base };
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto out = Consumed {};
    auto source = StopSource {};

    auto consumer = consumeOnStrand(&strand, &queue, &out);
    consumer.handle().promise().setStopToken(source.get_token());
    consumer.handle().resume();
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());

    source.request_stop();
    CHECK(foreign.pending() == 0);
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    CHECK(out.end == ConsumerEnd::Cancelled);
    CHECK(out.cancelledOnStrand);
    CHECK_FALSE(queue.hasWaiter());
}

namespace
{

/// An object that owns a strand, the way morph's backend owned its StrandExecutor: whatever holds
/// the last reference to it decides where the strand's destructor runs.
class StrandOwner
{
  public:
    explicit StrandOwner(IExecutor& base): _strand(base) {}

    /// @return The strand this owner's work runs on.
    [[nodiscard]] Strand& strand() noexcept { return _strand; }

  private:
    Strand _strand;
};

/// Hops onto the owner's strand and, from inside that task, releases the last reference to the
/// owner -- so `~Strand` runs inside one of its own tasks. morph's CI hit exactly this twice: a
/// completion's frame held the owner and was destroyed on the strand.
Task<void> releaseOwnerFromItsStrand(std::shared_ptr<StrandOwner>* holder, std::atomic<bool>* released)
{
    co_await ResumeOn { (*holder)->strand() };
    holder->reset();
    // Reached only if the destructor did not wait for the task it was called from.
    released->store(true);
}

/// Hops onto @p strand and records that it ran.
Task<void> runOn(Strand* strand, int* ran)
{
    co_await ResumeOn { *strand };
    ++*ran;
}

} // namespace

TEST_CASE("A strand destroyed from inside one of its own tasks neither waits for itself nor runs what "
          "is queued behind",
          "[Strand][lifetime]")
{
    auto base = ManualExecutor {};
    auto released = std::atomic<bool> { false };
    auto ranBehind = 0;
    auto holder = std::make_shared<StrandOwner>(base);

    auto releaser = releaseOwnerFromItsStrand(&holder, &released);
    auto behind = runOn(&holder->strand(), &ranBehind);
    releaser.handle().resume();
    behind.handle().resume();
    std::ignore = base.drain();

    // The task that destroyed its strand ran to its end: the destructor did not wait for it.
    CHECK(released.load());
    CHECK(releaser.done());
    CHECK(holder == nullptr);
    // What was queued behind it was dropped with the strand -- left to the Task that owns it --
    // and the pump, which outlived the strand, ended without running anything more.
    CHECK(ranBehind == 0);
    CHECK_FALSE(behind.done());
    CHECK(base.pending() == 0);
    CHECK(currentExecutor() == nullptr);
}

namespace
{

/// A base that resumes what it is given inside `submit`, with itself current: the executor on which
/// a hand-off and everything it starts run on the submitter's own stack.
class InlineExecutor final: public IExecutor
{
  public:
    using IExecutor::submit;

    void submit(std::coroutine_handle<> handle) override { submit(ParkedWork { .resume = handle }); }

    void submit(ParkedWork work) override
    {
        auto entry = core::async::detail::Parked { std::move(work) };
        auto const scope = ExecutorScope { *this };
        entry.resume();
    }
};

} // namespace

TEST_CASE("A strand whose inline base runs a task that destroys the strand's owner is not touched after",
          "[Strand][lifetime]")
{
    // The hand-off that queued the pump runs it inside the base's submit; the task releases the last
    // reference to the owner, the pump ends, and the strand's state goes with it -- all before that
    // submit returns to the hand-off, which used to lock the freed state to end the hand-off
    // (ASan: heap-use-after-free).
    auto base = InlineExecutor {};
    auto released = std::atomic<bool> { false };
    auto holder = std::make_shared<StrandOwner>(base);

    auto releaser = releaseOwnerFromItsStrand(&holder, &released);
    releaser.handle().resume();

    CHECK(released.load());
    CHECK(holder == nullptr);
    CHECK(releaser.done());
    CHECK(currentExecutor() == nullptr);
}

namespace
{

/// Hops onto the owner's strand, releases the last reference to the owner from inside that task,
/// and THEN parks on @p queue: the resume target it takes names a strand that no longer exists.
DetachedTask releaseOwnerThenPop(std::shared_ptr<StrandOwner>* holder,
                                 Queue* queue,
                                 FrameSentinel sentinel,
                                 bool* parked)
{
    (void) sentinel;
    co_await ResumeOn { (*holder)->strand() };
    holder->reset();
    *parked = true;
    std::ignore = co_await queue->pop();
    *parked = false;
}

} // namespace

TEST_CASE("A consumer parked while on a strand does not reach that strand once it is destroyed",
          "[Strand][AsyncQueue][lifetime]")
{
    // The owner `{ AsyncQueue queue; Strand strand; }` destroys the strand first: a push, a close or
    // a stop that lands between the two -- or from another thread, at any time after -- hands the
    // consumer to the strand it parked on. It must find the strand's state, closed, not freed
    // storage (ASan: heap-use-after-free in Strand::submit, before the fix).
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto out = Consumed {};
    auto consumer = std::optional<Task<void>> {};
    {
        auto strand = Strand { base };
        consumer.emplace(consumeOnStrand(&strand, &queue, &out));
        consumer->handle().resume();
        std::ignore = base.drain();
        REQUIRE(queue.hasWaiter());
    }

    std::ignore = queue.push(7);
    // Dropped by the closed strand: never resumed, on the strand's base or the queue's.
    CHECK_FALSE(queue.hasWaiter());
    CHECK(base.pending() == 0);
    CHECK(foreign.pending() == 0);
    CHECK(out.seen.empty());
    CHECK(out.end == ConsumerEnd::Running);
}

TEST_CASE("A task that destroys its strand's owner and then parks is freed, not resumed on freed storage",
          "[Strand][AsyncQueue][lifetime]")
{
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto destroyed = 0;
    auto parked = false;
    auto holder = std::make_shared<StrandOwner>(base);

    releaseOwnerThenPop(&holder, &queue, FrameSentinel { &destroyed }, &parked);
    std::ignore = base.drain();
    REQUIRE(holder == nullptr);
    REQUIRE(parked);
    REQUIRE(queue.hasWaiter());

    // The push hands the detached consumer to the strand it parked on, which is closed: the chain
    // nobody owns is freed there rather than resumed.
    std::ignore = queue.push(1);
    CHECK(destroyed == 1);
    CHECK(parked);
    CHECK(base.pending() == 0);
    CHECK(foreign.pending() == 0);
}

namespace
{

/// A base executor that refuses the next @c refuse submits by throwing, and otherwise queues like
/// @c ManualExecutor -- an executor whose own queue could not grow, or that is shutting down.
class RefusingExecutor final: public IExecutor
{
  public:
    using IExecutor::submit;

    /// Refuses the next @p count submits.
    /// @param count How many.
    void refuse(int count) noexcept { _refuse = count; }

    void submit(std::coroutine_handle<> handle) override { submit(ParkedWork { .resume = handle }); }

    void submit(ParkedWork work) override
    {
        if (_refuse > 0)
        {
            --_refuse;
            throw std::runtime_error { "the base executor refuses" };
        }
        _inner.submit(std::move(work));
    }

    /// @return How many entries were resumed.
    std::size_t drain() { return _inner.drain(); }

    /// @return How many entries are queued.
    [[nodiscard]] std::size_t pending() const { return _inner.pending(); }

  private:
    ManualExecutor _inner;
    int _refuse { 0 };
};

} // namespace

TEST_CASE("A base that refuses the strand's pump leaves the strand usable, and the refused task unqueued",
          "[Strand][exceptions]")
{
    // Before the fix the pump stayed Scheduled with nothing queued anywhere: every later submit only
    // queued behind it, and the refused task ran later although its submitter had been told no.
    auto base = RefusingExecutor {};
    auto strand = Strand { base };
    auto seen = std::vector<Sighting> {};

    auto refused = lookOnce(&strand, &seen);
    base.refuse(1);
    CHECK_THROWS_AS(strand.submit(refused.handle()), std::runtime_error);
    CHECK(strand.queued() == 0);

    auto accepted = lookOnce(&strand, &seen);
    strand.submit(accepted.handle());
    std::ignore = base.drain();
    CHECK(accepted.done());
    CHECK_FALSE(refused.done());
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].onStrand);
}

#if !defined(_MSC_VER) || defined(__clang__)
namespace
{

/// What a @c ResubmitWhenFreed saw.
struct Resubmits
{
    int freed { 0 };      ///< Times a frame carrying one was freed.
    bool threw { false }; ///< Whether the submit from its destructor threw.
};

/// Submits to a strand from its destructor: a frame that, as it is freed, hands work back to the
/// strand that frees it. What that submit throws is recorded, not let out of the destructor.
class ResubmitWhenFreed
{
  public:
    ResubmitWhenFreed(Strand* strand, Resubmits* out) noexcept: _strand(strand), _out(out) {}
    ResubmitWhenFreed(ResubmitWhenFreed&& other) noexcept:
        _strand(std::exchange(other._strand, nullptr)), _out(other._out)
    {
    }
    ResubmitWhenFreed(ResubmitWhenFreed const&) = delete;
    ResubmitWhenFreed& operator=(ResubmitWhenFreed const&) = delete;
    ResubmitWhenFreed& operator=(ResubmitWhenFreed&&) = delete;

    ~ResubmitWhenFreed()
    {
        if (_strand == nullptr)
            return;
        ++_out->freed;
        try
        {
            _strand->submit(std::noop_coroutine());
        }
        catch (...)
        {
            _out->threw = true;
        }
    }

  private:
    Strand* _strand;
    Resubmits* _out;
};

/// A detached flow queued on @p strand, carrying a @c ResubmitWhenFreed.
DetachedTask resubmitsWhenFreed(Strand* strand, ResubmitWhenFreed guard, int* ran)
{
    (void) guard;
    co_await ResumeOn { *strand };
    ++*ran;
}

} // namespace
#endif

TEST_CASE("Work a refused hand-off frees is dropped, not refused, when it submits to the strand again",
          "[Strand][exceptions]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    SKIP("under MSVC's cl a throw out of resume() on a strand terminates the process "
         "(core-cpp.strand-throw-canary)");
#else
    // The task's throw ends the pump and the base refuses its replacement, which abandons the queue.
    // Before the fix the abandoned frame was freed with the strand idle and open: its destructor's
    // submit made a new pump, handed it to the same refusing base, and threw out of a destructor.
    auto base = RefusingExecutor {};
    auto strand = Strand { base };
    auto seen = std::vector<Sighting> {};
    auto resubmits = Resubmits {};
    auto ran = 0;

    auto const thrower = throwOnResume(&strand, &seen, true);
    strand.submit(thrower.handle());
    resubmitsWhenFreed(&strand, ResubmitWhenFreed { &strand, &resubmits }, &ran);
    REQUIRE(strand.queued() == 2);

    base.refuse(2);
    // What leaves is the refusal of the replacement pump, not the task's own exception.
    auto thrown = std::string {};
    try
    {
        std::ignore = base.drain();
    }
    catch (std::exception const& error)
    {
        thrown = error.what();
    }
    CHECK(thrown == "the base executor refuses");
    base.refuse(0);
    CHECK(resubmits.freed == 1);
    CHECK_FALSE(resubmits.threw);
    CHECK(ran == 0);
    CHECK(strand.queued() == 0);
    CHECK(base.pending() == 0);

    // The strand still takes work.
    auto after = lookOnce(&strand, &seen);
    strand.submit(after.handle());
    std::ignore = base.drain();
    CHECK(after.done());
#endif
}

#if !defined(_MSC_VER) || defined(__clang__)
namespace
{

/// Submits a coroutine to a strand from its destructor, recording what that submit throws.
class StartWhenFreed
{
  public:
    StartWhenFreed(Strand* strand, std::coroutine_handle<> handle, bool* threw) noexcept:
        _strand(strand), _handle(handle), _threw(threw)
    {
    }
    StartWhenFreed(StartWhenFreed&& other) noexcept:
        _strand(std::exchange(other._strand, nullptr)), _handle(other._handle), _threw(other._threw)
    {
    }
    StartWhenFreed(StartWhenFreed const&) = delete;
    StartWhenFreed& operator=(StartWhenFreed const&) = delete;
    StartWhenFreed& operator=(StartWhenFreed&&) = delete;

    ~StartWhenFreed()
    {
        if (_strand == nullptr)
            return;
        try
        {
            _strand->submit(_handle);
        }
        catch (...)
        {
            *_threw = true;
        }
    }

  private:
    Strand* _strand;
    std::coroutine_handle<> _handle;
    bool* _threw;
};

/// A detached flow queued on @p strand, carrying a @c StartWhenFreed.
DetachedTask startsWhenFreed(Strand* strand, StartWhenFreed guard, int* ran)
{
    (void) guard;
    co_await ResumeOn { *strand };
    ++*ran;
}

} // namespace
#endif

TEST_CASE("A task on another strand, started by abandoned work as it is freed, can still submit to the "
          "refusing strand",
          "[Strand][exceptions]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    SKIP("under MSVC's cl a throw out of resume() on a strand terminates the process "
         "(core-cpp.strand-throw-canary)");
#else
    // The abandoned frame's destructor starts a task on a second strand, whose base runs it inline --
    // still inside the first strand's drop. That task's own hop back to the first strand is not a
    // resubmit from the abandoned work, and must reach the first strand's base. Before the fix the
    // drop's marker covered everything that ran synchronously inside it, and the hop was dropped.
    auto refusing = RefusingExecutor {};
    auto inlineBase = InlineExecutor {};
    auto first = Strand { refusing };
    auto second = Strand { inlineBase };
    auto seen = std::vector<Sighting> {};
    auto threw = false;
    auto ran = 0;

    auto hopper = hopAndLook(&first, &seen);
    auto const thrower = throwOnResume(&first, &seen, true);
    first.submit(thrower.handle());
    startsWhenFreed(&first, StartWhenFreed { &second, hopper.handle(), &threw }, &ran);

    refusing.refuse(1);
    CHECK_THROWS_AS(refusing.drain(), std::runtime_error);
    CHECK_FALSE(threw);
    CHECK(ran == 0);
    // The hopper ran on the second strand and queued itself on the first, whose base now has it.
    CHECK(refusing.pending() == 1);
    std::ignore = refusing.drain();
    CHECK(hopper.done());
    REQUIRE(seen.size() == 3); // the thrower's sighting, then the hopper's two
    CHECK(seen[2].onStrand);
#endif
}

TEST_CASE("A base that refuses the pump's hand-back between turns does not wedge the strand",
          "[Strand][exceptions]")
{
    // With a batch of one, the pump hands the base back after every task and queues itself again.
    // A refusal there cannot be reported to anyone -- the pump is not inside a caller's submit -- so
    // the pump keeps the base it is running on and goes on with the next turn inline.
    auto base = RefusingExecutor {};
    auto strand = Strand { base, core::async::StrandOptions { .batch = 1 } };
    auto seen = std::vector<Sighting> {};

    auto first = lookOnce(&strand, &seen);
    auto second = lookOnce(&strand, &seen);
    strand.submit(first.handle());
    strand.submit(second.handle());
    REQUIRE(base.pending() == 1);

    base.refuse(1);
    CHECK_NOTHROW(base.drain());
    CHECK(first.done());
    CHECK(second.done());
    CHECK(seen.size() == 2);
    CHECK(strand.queued() == 0);

    // And it still takes work afterwards.
    auto third = lookOnce(&strand, &seen);
    strand.submit(third.handle());
    std::ignore = base.drain();
    CHECK(third.done());
}

namespace
{

/// A callable that owns something, so a test can see whether it was moved from.
struct OwningCall
{
    std::unique_ptr<int> payload;
    std::vector<int>* out;

    void operator()() const { out->push_back(*payload); }
};

/// The session an around-task hook installs, as morph's action session is installed: per thread,
/// for the length of one task.
thread_local int ambientSession = 0;

/// An around-task hook that installs @c session for the length of every task, and counts them.
struct SessionHook
{
    int session { 0 };
    int calls { 0 };

    void operator()(core::async::RunTask run)
    {
        ++calls;
        auto const previous = std::exchange(ambientSession, session);
        run();
        ambientSession = previous;
    }
};

/// Hops onto @p strand, then pops twice, recording the ambient session at every resumption.
Task<void> sessionConsumer(Strand* strand, Queue* queue, std::vector<int>* sessions)
{
    co_await ResumeOn { *strand };
    sessions->push_back(ambientSession);
    for ([[maybe_unused]] auto const index: { 0, 1 })
    {
        std::ignore = co_await queue->pop();
        sessions->push_back(ambientSession);
    }
}

} // namespace

TEST_CASE("Strand::post runs a callable as one task, in FIFO order with submitted coroutines",
          "[Strand][post]")
{
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto order = std::vector<int> {};
    auto onStrand = std::vector<bool> {};
    auto seen = std::vector<Sighting> {};

    strand.post([&order, &onStrand, &strand] {
        order.push_back(1);
        onStrand.push_back(strand.runningHere());
    });
    auto coroutine = lookOnce(&strand, &seen);
    strand.submit(coroutine.handle());
    strand.post([&order, &onStrand, &strand, &seen] {
        order.push_back(seen.empty() ? -1 : 3);
        onStrand.push_back(strand.runningHere() && currentExecutor() != nullptr);
    });
    CHECK(strand.queued() == 3);
    CHECK(base.pending() == 1);

    std::ignore = base.drain();
    CHECK(order == std::vector { 1, 3 });
    CHECK(onStrand == std::vector { true, true });
    CHECK(coroutine.done());
    CHECK(strand.queued() == 0);
}

TEST_CASE("A callable that throws out of a strand propagates as a task's throw, and the strand goes on",
          "[Strand][post][exceptions]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    SKIP("under MSVC's cl a throw out of a strand task ends the process; "
         "core-cpp.strand-throw-canary asserts it");
#else
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto order = std::vector<int> {};

    strand.post([] { throw std::runtime_error { "a posted call that throws" }; });
    strand.post([&order] { order.push_back(2); });
    CHECK_THROWS_AS(base.drain(), std::runtime_error);
    CHECK(currentExecutor() == nullptr);
    std::ignore = base.drain();
    CHECK(order == std::vector { 2 });
#endif
}

TEST_CASE("tryPost and trySubmit queue on an open strand, and leave the work with the caller once it is "
          "closed",
          "[Strand][post]")
{
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto order = std::vector<int> {};
    auto seen = std::vector<Sighting> {};

    auto first = OwningCall { .payload = std::make_unique<int>(1), .out = &order };
    CHECK(strand.tryPost(first));
    auto queuedTask = lookOnce(&strand, &seen);
    CHECK(strand.trySubmit(queuedTask.handle()));
    std::ignore = base.drain();
    CHECK(order == std::vector { 1 });
    CHECK(queuedTask.done());

    strand.close();
    strand.close(); // idempotent

    auto call = OwningCall { .payload = std::make_unique<int>(2), .out = &order };
    CHECK_FALSE(strand.tryPost(call));
    CHECK(call.payload != nullptr);

    auto refused = lookOnce(&strand, &seen);
    auto work = ParkedWork { .resume = refused.handle() };
    CHECK_FALSE(strand.trySubmit(work));
    CHECK(work.resume == refused.handle());
    CHECK_FALSE(strand.trySubmit(refused.handle()));

    // post and submit drop instead.
    strand.post([&order] { order.push_back(3); });
    strand.submit(refused.handle());
    CHECK(base.pending() == 0);
    CHECK(order == std::vector { 1 });
    CHECK_FALSE(refused.done());
}

TEST_CASE("Strand::idle is true once nothing is queued or running", "[Strand][idle]")
{
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    CHECK(strand.idle());

    auto inside = std::optional<bool> {};
    strand.post([&inside, &strand] { inside = strand.idle(); });
    CHECK_FALSE(strand.idle());
    std::ignore = base.drain();
    CHECK(strand.idle());
    REQUIRE(inside.has_value());
    CHECK_FALSE(*inside); // running is not idle
}

TEST_CASE("An around-task hook runs around every task, including a resumption that came back through the "
          "strand from another executor",
          "[Strand][aroundTask]")
{
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto hook = SessionHook { .session = 42 };
    auto strand =
        Strand { base, core::async::StrandOptions { .aroundTask = core::async::AroundTask::of(hook) } };
    auto sessions = std::vector<int> {};

    auto consumer = sessionConsumer(&strand, &queue, &sessions);
    consumer.handle().resume(); // hops onto the strand
    std::ignore = base.drain(); // first task: runs to the first pop, and parks
    REQUIRE(queue.hasWaiter());
    std::ignore = queue.push(1); // hands the consumer back to the strand it parked on
    std::ignore = base.drain();
    std::ignore = queue.push(2);
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    CHECK(consumer.done());
    CHECK(sessions == std::vector { 42, 42, 42 });
    CHECK(hook.calls == 3);
    CHECK(ambientSession == 0);

    strand.post([] { CHECK(ambientSession == 42); });
    std::ignore = base.drain();
    CHECK(hook.calls == 4);
}

namespace
{

using core::async::RunTask;
using core::async::TaskKind;

static_assert(noexcept(std::declval<RunTask const&>().kind()), "kind() is noexcept");
static_assert(std::is_same_v<decltype(std::declval<RunTask const&>().kind()), TaskKind>,
              "kind() returns the TaskKind");

/// A hook that records what kind of task it runs around, and runs it.
struct KindHook
{
    std::vector<TaskKind> kinds;

    void operator()(RunTask run)
    {
        kinds.push_back(run.kind());
        run();
    }
};

/// Hops onto @p strand with `ResumeOn`, then counts itself run.
Task<void> hopOnto(Strand* strand, int* ran)
{
    co_await ResumeOn { *strand };
    ++*ran;
}

} // namespace

TEST_CASE("An around-task hook is told whether it runs a posted callable or a coroutine resumption",
          "[Strand][aroundTask][kind]")
{
    // A hook that installs per-resumption context -- a request's session -- can apply it to the
    // resumptions alone (core-cpp#53): every way a coroutine reaches the strand is a resumption, and
    // every callable posted to it is a callable.
    auto base = ManualExecutor {};
    auto hook = KindHook {};
    auto strand =
        Strand { base, core::async::StrandOptions { .aroundTask = core::async::AroundTask::of(hook) } };
    auto order = std::vector<int> {};

    strand.post([&order] { order.push_back(1); });
    auto call = OwningCall { .payload = std::make_unique<int>(2), .out = &order };
    CHECK(strand.tryPost(call));

    auto seen = std::vector<Sighting> {};
    auto byHandle = lookOnce(&strand, &seen);
    strand.submit(byHandle.handle());

    auto destroyed = 0;
    auto root = std::coroutine_handle<> {};
    core::async::test::parkDetached(&root, FrameSentinel { &destroyed });
    REQUIRE(root);
    strand.submit(ParkedWork { .resume = root, .abandon = core::async::detail::claimOn(root) });

    auto hopped = 0;
    auto hop = hopOnto(&strand, &hopped);
    hop.handle().resume(); // a ResumeOn hop: the strand is handed the coroutine

    std::ignore = base.drain();
    CHECK(hook.kinds
          == std::vector { TaskKind::Callable,
                           TaskKind::Callable,
                           TaskKind::Resumption,
                           TaskKind::Resumption,
                           TaskKind::Resumption });
    CHECK(order == std::vector { 1, 2 });
    CHECK(byHandle.done());
    CHECK(destroyed == 1);
    CHECK(hopped == 1);
    CHECK(hop.done());
}

TEST_CASE("seal() closes the offer door, and runs what is queued and what comes back", "[Strand][seal]")
{
    // After seal() the try members hand work back; post and submit -- how work the strand already
    // admitted comes back to it -- are still admitted until close(), and what was queued runs.
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto order = std::vector<int> {};
    auto seen = std::vector<Sighting> {};

    strand.post([&order] { order.push_back(1); });
    auto queued = lookOnce(&strand, &seen);
    strand.submit(queued.handle());
    strand.seal();
    strand.seal(); // idempotent
    CHECK_FALSE(strand.idle());

    auto refusedCall = OwningCall { .payload = std::make_unique<int>(2), .out = &order };
    CHECK_FALSE(strand.tryPost(refusedCall));
    CHECK(refusedCall.payload != nullptr);
    auto refused = lookOnce(&strand, &seen);
    auto work = ParkedWork { .resume = refused.handle() };
    CHECK_FALSE(strand.trySubmit(work));
    CHECK(work.resume == refused.handle());
    CHECK_FALSE(strand.trySubmit(refused.handle()));

    strand.post([&order] { order.push_back(3); });
    auto cameBack = lookOnce(&strand, &seen);
    strand.submit(cameBack.handle());

    // A host with one thread pumps its base until the strand is idle: nothing queued or running.
    while (!strand.idle() && base.runOne())
    {
    }
    CHECK(strand.idle());
    CHECK(order == std::vector { 1, 3 });
    CHECK(queued.done());
    CHECK(cameBack.done());
    CHECK_FALSE(refused.done());
    CHECK(base.pending() == 0);
    strand.close();
    CHECK(strand.idle());
}

TEST_CASE("A coroutine parked off the strand when it is sealed comes back to it, and finishes before close",
          "[Strand][seal][AsyncQueue]")
{
    // The teardown morph runs: its handler is admitted, runs on the strand and parks on an
    // AsyncQueue pop -- off the strand -- when the seal lands. idle() cannot see it; the push that
    // hands it back goes through the strand's plain submit, which after the seal dropped it
    // (3549c34): a detached chain freed without its finish, an owned one never resumed.
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto strand = Strand { base };
    auto out = Consumed {};
    auto consumer = consumeOnStrand(&strand, &queue, &out);
    consumer.handle().resume();
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());

    strand.seal();
    CHECK(strand.idle()); // what the strand can see: nothing queued, nothing running

    std::ignore = queue.push(7);
    std::ignore = base.drain();
    queue.close();
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    CHECK(out.seen == std::vector { 7 });
    CHECK(out.onStrand == std::vector { true });
    CHECK(out.end == ConsumerEnd::Closed);
    CHECK(consumer.done());
    CHECK(strand.idle());
    strand.close();
}

TEST_CASE("A trySubmit a sealed strand refuses leaves the caller an armed claim", "[Strand][seal]")
{
    auto base = ManualExecutor {};
    auto strand = Strand { base };
    auto destroyed = 0;
    auto root = std::coroutine_handle<> {};
    core::async::test::parkDetached(&root, FrameSentinel { &destroyed });
    REQUIRE(root);
    auto work = ParkedWork { .resume = root, .abandon = core::async::detail::claimOn(root) };
    REQUIRE(work.abandon.armed());

    strand.seal();
    CHECK_FALSE(strand.trySubmit(work));
    CHECK(work.resume == root);
    CHECK(work.abandon.armed());
    CHECK(destroyed == 0);
    // Still the caller's to give up: the last claim on an armed chain frees it.
    work.abandon.reset();
    CHECK(destroyed == 1);
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)

namespace
{

using namespace std::chrono_literals;

/// How long a case waits for other threads before it calls the machine wedged: generous, because
/// what is waited for is at most a few thousand resumptions on a pool, and a cold two-core runner
/// under a sanitizer is what it has to hold for.
constexpr auto Budget = 60s;

/// Polls @p done on a monotonic clock until it holds or the budget runs out.
/// @return Whether it held.
template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate done)
{
    auto const deadline = std::chrono::steady_clock::now() + Budget;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// What the overlap case counts, from every pool thread at once.
struct Overlap
{
    std::atomic<int> inside { 0 };    ///< Tasks inside their critical section right now.
    std::atomic<int> overlaps { 0 };  ///< Times a task found another inside.
    std::atomic<int> offStrand { 0 }; ///< Times a task found itself not on the strand.
    std::atomic<int> finished { 0 };  ///< Tasks that have ended.
    std::mutex orderMutex;            ///< Guards @c order, so a broken strand is a red, not a crash.
    std::vector<int> order;           ///< The order tasks ran in.
};

/// Hops onto @p strand, then stays inside a critical section for a while, counting company.
DetachedTask overlapProbe(Strand* strand, Overlap* probe, int index)
{
    co_await ResumeOn { *strand };
    if (!strand->runningHere() || currentExecutor() == nullptr)
        probe->offStrand.fetch_add(1);
    if (probe->inside.fetch_add(1) != 0)
        probe->overlaps.fetch_add(1);
    {
        auto const guard = std::scoped_lock { probe->orderMutex };
        probe->order.push_back(index);
    }
    for ([[maybe_unused]] auto const spin: std::views::iota(0, 50))
        std::this_thread::yield();
    probe->inside.fetch_sub(1);
    probe->finished.fetch_add(1);
}

} // namespace

TEST_CASE("A strand over a four-thread pool runs its tasks in FIFO order and never two at once",
          "[Strand][threads]")
{
    constexpr auto Count = 2000;
    auto probe = Overlap {};
    auto pool = core::async::ThreadPoolExecutor { 4 };
    // Declared after the pool: destroyed first, while the pool still runs what it queued.
    auto strand = Strand { pool };

    for (auto const index: std::views::iota(0, Count))
        overlapProbe(&strand, &probe, index);

    auto const done = waitUntil([&probe] { return probe.finished.load() == Count; });
    INFO("finished " << probe.finished.load() << " of " << Count << " inside the budget");
    REQUIRE(done);
    CHECK(probe.overlaps.load() == 0);
    CHECK(probe.offStrand.load() == 0);
    auto expected = std::vector<int> {};
    for (auto const index: std::views::iota(0, Count))
        expected.push_back(index);
    auto const guard = std::scoped_lock { probe.orderMutex };
    CHECK(probe.order == expected);
}

TEST_CASE("ThreadPoolExecutor states itself as the current executor on its threads",
          "[Strand][context][threads]")
{
    auto seen = std::atomic<IExecutor*> { nullptr };
    auto done = std::atomic<bool> { false };
    auto pool = core::async::ThreadPoolExecutor { 2 };
    [](core::async::ThreadPoolExecutor* on,
       std::atomic<IExecutor*>* out,
       std::atomic<bool>* flag) -> DetachedTask {
        co_await ResumeOn { *on };
        out->store(currentExecutor());
        flag->store(true);
    }(&pool, &seen, &done);
    REQUIRE(waitUntil([&done] { return done.load(); }));
    CHECK(seen.load() == &pool);
}

TEST_CASE("A strand destroyed while its task runs on another thread waits for that task", "[Strand][threads]")
{
    auto started = std::atomic<bool> { false };
    auto finished = std::atomic<bool> { false };
    auto pool = core::async::ThreadPoolExecutor { 1 };
    {
        auto strand = std::optional<Strand> {};
        strand.emplace(pool);
        [](Strand* on, std::atomic<bool>* begun, std::atomic<bool>* ended) -> DetachedTask {
            co_await ResumeOn { *on };
            begun->store(true);
            std::this_thread::sleep_for(200ms);
            ended->store(true);
        }(&*strand, &started, &finished);
        REQUIRE(waitUntil([&started] { return started.load(); }));
        strand.reset();
        // The destructor returned, so the task it was running had ended.
        CHECK(finished.load());
    }
}

TEST_CASE("A coroutine on a strand fed by a producer thread through AsyncQueue always resumes on the "
          "strand",
          "[Strand][AsyncQueue][context][threads]")
{
    static constexpr auto Count = 500;
    auto foreign = core::async::ThreadPoolExecutor { 1 };
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto out = Consumed {};
    auto consumer = std::optional<Task<void>> {};
    {
        auto pool = core::async::ThreadPoolExecutor { 4 };
        auto strand = Strand { pool };
        consumer.emplace(consumeOnStrand(&strand, &queue, &out));
        consumer->handle().resume();

        auto drained = std::atomic<bool> { false };
        auto producer = std::thread { [&queue, &drained] {
            for (auto const index: std::views::iota(0, Count))
            {
                std::ignore = queue.push(index);
                if (index % 16 == 0)
                    std::this_thread::sleep_for(100us);
            }
            // Closing discards what is held, so it waits for the consumer to have taken everything:
            // an item a pop took is delivered whatever happens after.
            drained.store(waitUntil([&queue] { return queue.size() == 0; }));
            queue.close();
        } };
        producer.join();
        // Asserted here, on the case's thread: Catch2's assertions belong to it.
        CHECK(drained.load());
        auto const done = waitUntil([&out] { return out.finished.load(); });
        REQUIRE(done);
        // The strand and then the pool go here, and the pool's join is what ends every thread
        // that touched the consumer's frame -- so it is destroyed below with nobody inside it.
    }
    CHECK(out.end == ConsumerEnd::Closed);
    CHECK(out.seen.size() == Count);
    CHECK(out.onStrand.size() == out.seen.size());
    CHECK(std::ranges::all_of(out.onStrand, [](bool on) { return on; }));
}

TEST_CASE("A coroutine on a strand parked on AsyncQueue::pop unwinds on the strand when another thread "
          "stops it",
          "[Strand][AsyncQueue][context][threads]")
{
    auto foreign = core::async::ThreadPoolExecutor { 1 };
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto out = Consumed {};
    auto source = StopSource {};
    auto consumer = std::optional<Task<void>> {};
    {
        auto pool = core::async::ThreadPoolExecutor { 4 };
        auto strand = Strand { pool };
        consumer.emplace(consumeOnStrand(&strand, &queue, &out));
        // Set on the root, which nothing awaits: an awaiting coroutine would hand down its own.
        consumer->handle().promise().setStopToken(source.get_token());
        consumer->handle().resume();

        REQUIRE(waitUntil([&queue] { return queue.hasWaiter(); }));
        auto stopper = std::thread { [&source] { source.request_stop(); } };
        stopper.join();
        auto const done = waitUntil([&out] { return out.finished.load(); });
        REQUIRE(done);
    }
    CHECK(out.end == ConsumerEnd::Cancelled);
    CHECK(out.cancelledOnStrand);
}

TEST_CASE("A strand destroyed from inside one of its own tasks on a pool thread does not deadlock",
          "[Strand][lifetime][threads]")
{
    // The same, on a pool thread, where the wait the destructor skips would otherwise block that
    // thread for ever: the shape morph's CI deadlocked on.
    auto released = std::atomic<bool> { false };
    auto holder = std::shared_ptr<StrandOwner> {};
    auto releaser = std::optional<Task<void>> {};
    {
        auto pool = core::async::ThreadPoolExecutor { 2 };
        holder = std::make_shared<StrandOwner>(pool);
        releaser.emplace(releaseOwnerFromItsStrand(&holder, &released));
        releaser->handle().resume();
        auto const done = waitUntil([&released] { return released.load(); });
        INFO("the task that released the strand's owner " << (done ? "finished" : "never finished"));
        REQUIRE(done);
    }
    CHECK(holder == nullptr);
    CHECK(releaser->done());
}

namespace
{

/// A base that holds its first submit until the case says so, then refuses it: the window in which
/// a second submitter finds the pump already scheduled, and a close can start.
class HoldingRefusingExecutor final: public IExecutor
{
  public:
    using IExecutor::submit;

    void submit(std::coroutine_handle<> handle) override { submit(ParkedWork { .resume = handle }); }

    void submit(ParkedWork work) override
    {
        if (!_refused.exchange(true))
        {
            _entered.store(true);
            std::ignore = waitUntil([this] { return _release.load(); });
            // Long enough for a close started just before the release to be inside its wait.
            std::this_thread::sleep_for(20ms);
            throw std::runtime_error { "the base executor refuses" };
        }
        _inner.submit(std::move(work));
    }

    /// @return Whether the held submit has been entered.
    [[nodiscard]] bool entered() const noexcept { return _entered.load(); }

    /// Lets the held submit go on, to refuse.
    void release() noexcept { _release.store(true); }

    /// @return How many entries were resumed.
    std::size_t drain() { return _inner.drain(); }

  private:
    ManualExecutor _inner;
    std::atomic<bool> _refused { false };
    std::atomic<bool> _entered { false };
    std::atomic<bool> _release { false };
};

/// Hops onto @p strand, recording whether the hop threw and whether it arrived.
DetachedTask hopOrRecordRefusal(Strand* strand,
                                FrameSentinel sentinel,
                                std::atomic<int>* refused,
                                std::atomic<int>* arrived)
{
    (void) sentinel;
    try
    {
        co_await ResumeOn { *strand };
        arrived->fetch_add(1);
    }
    catch (std::runtime_error const&)
    {
        refused->fetch_add(1);
    }
}

} // namespace

TEST_CASE("A refused hand-off abandons the work another thread queued behind it, and the strand restarts",
          "[Strand][exceptions][threads]")
{
    // Submitter A publishes the pump as scheduled and is held inside the base's submit; submitter B
    // finds it scheduled, queues and returns. The base then refuses A. Before the fix B's work stayed
    // queued with no pump anywhere, and ran -- late, out of nobody's order -- on the next submit.
    // Now a refusal means the base is not running this strand, and what is queued is dropped, as
    // close() drops it: B's detached frame is freed, never run.
    auto refused = std::atomic<int> { 0 };
    auto arrived = std::atomic<int> { 0 };
    auto destroyedA = 0;
    auto destroyedB = 0;
    auto base = HoldingRefusingExecutor {};
    auto strand = Strand { base };

    auto submitterA = std::thread { [&strand, &refused, &arrived, &destroyedA] {
        hopOrRecordRefusal(&strand, FrameSentinel { &destroyedA }, &refused, &arrived);
    } };
    REQUIRE(waitUntil([&base] { return base.entered(); }));
    hopOrRecordRefusal(&strand, FrameSentinel { &destroyedB }, &refused, &arrived);
    base.release();
    submitterA.join();

    CHECK(refused.load() == 1); // A was told, and ran its catch on its own thread.
    CHECK(destroyedA == 1);     // ...and ended there.
    CHECK(destroyedB == 1);     // B was dropped with the queue,
    CHECK(strand.queued() == 0);
    std::ignore = base.drain();
    CHECK(arrived.load() == 0); // and never ran.

    // The strand is idle, not wedged: the next submit makes it run again.
    auto seen = std::vector<Sighting> {};
    auto next = lookOnce(&strand, &seen);
    strand.submit(next.handle());
    std::ignore = base.drain();
    CHECK(next.done());
    CHECK(seen.size() == 1);
}

TEST_CASE("A strand closed while a refused hand-off is still inside the base's submit does not free the "
          "work the refusal returns",
          "[Strand][exceptions][threads][lifetime]")
{
    // close() used to drop the queue -- the refused submitter's own entry included, freeing its
    // detached frame -- while the submitter was still inside the base's submit; the refusal then
    // resumed the freed frame with the exception (ASan: heap-use-after-free). close() now waits for
    // hand-offs in flight, as it waits for a running task.
    auto refused = std::atomic<int> { 0 };
    auto arrived = std::atomic<int> { 0 };
    auto destroyed = 0;
    auto base = HoldingRefusingExecutor {};
    auto strand = std::optional<Strand> {};
    strand.emplace(base);

    auto submitter = std::thread { [&strand, &refused, &arrived, &destroyed] {
        hopOrRecordRefusal(&*strand, FrameSentinel { &destroyed }, &refused, &arrived);
    } };
    REQUIRE(waitUntil([&base] { return base.entered(); }));
    base.release();
    strand.reset();
    submitter.join();

    CHECK(refused.load() == 1);
    CHECK(arrived.load() == 0);
    CHECK(destroyed == 1);
}

namespace
{

/// Increments @p ran once resumed; for a submitter that resumes it itself where it is refused.
Task<void> countWhenRun(std::atomic<int>* ran)
{
    ran->fetch_add(1);
    co_return;
}

} // namespace

TEST_CASE("Work offered while another thread seals a strand either runs or is handed back, never dropped",
          "[Strand][seal][threads]")
{
    // Producers offer callables and coroutines through the try members, and run what is refused
    // themselves, until each has been refused often enough to know the seal landed in the middle of
    // its stream; a third thread seals the strand. Every piece of work must have run exactly once,
    // on the strand or on its producer: a piece accepted and then dropped is the window seal()
    // exists to close.
    static constexpr auto RefusalsEach = 64;
    static constexpr auto OffersEachSide = 1 << 14;
    // Declared before the pool and the strand, so it outlives them: a case that times out with
    // tasks still queued then reads red, rather than having the pool resume freed frames.
    auto tasks = std::array<std::vector<Task<void>>, 2> {};
    auto ran = std::atomic<int> { 0 };
    auto pool = core::async::ThreadPoolExecutor { 4 };
    auto strand = Strand { pool };
    auto offered = std::atomic<int> { 0 };
    auto refusedTotal = std::atomic<int> { 0 };
    auto producersDone = std::atomic<int> { 0 };
    auto sealIssued = std::atomic<bool> { false };
    auto const deadline = std::chrono::steady_clock::now() + Budget;

    auto producer = [&](std::size_t which) {
        auto& mine = tasks.at(which);
        mine.reserve(2 * static_cast<std::size_t>(OffersEachSide)); // never reallocated
        auto refused = 0;
        auto beforeSeal = 0;
        auto afterSeal = 0;
        while (refused < RefusalsEach && std::chrono::steady_clock::now() < deadline)
        {
            // Bounded on both sides of the seal: a producer that got through its share before the
            // seal waits for it rather than running out of offers, and a seal that refuses nothing
            // fails the case once the share after it is spent, not on the budget.
            if (!sealIssued.load())
            {
                if (beforeSeal == OffersEachSide)
                {
                    std::this_thread::yield();
                    continue;
                }
                ++beforeSeal;
            }
            else if (afterSeal++ == OffersEachSide)
                break;
            auto call = [&ran] {
                ran.fetch_add(1);
            };
            if (!strand.tryPost(call))
            {
                ++refused;
                call();
            }
            mine.push_back(countWhenRun(&ran));
            if (auto const handle = mine.back().handle(); !strand.trySubmit(handle))
            {
                ++refused;
                handle.resume();
            }
            offered.fetch_add(2);
        }
        refusedTotal.fetch_add(refused);
        producersDone.fetch_add(1);
    };
    auto first = std::thread { producer, std::size_t { 0 } };
    auto second = std::thread { producer, std::size_t { 1 } };
    // Sealed as soon as the stream is going -- polled without a sleep, so the seal lands in it.
    while (offered.load() < 2000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    strand.seal();
    sealIssued.store(true);
    CHECK(waitUntil([&producersDone] { return producersDone.load() == 2; }));
    first.join();
    second.join();
    CHECK(waitUntil([&strand] { return strand.idle(); }));

    // Both producers saw the seal; and nothing offered was lost on either side of it.
    CHECK(refusedTotal.load() >= 2 * RefusalsEach);
    CHECK(ran.load() == offered.load());
    for (auto const& mine: tasks)
        CHECK(std::ranges::all_of(mine, [](Task<void> const& task) { return task.done(); }));
}

#endif
