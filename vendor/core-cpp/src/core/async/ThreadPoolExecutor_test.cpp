// SPDX-License-Identifier: Apache-2.0
#include <core/async/AsyncQueue.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <tuple>
#include <vector>

using core::async::DetachedTask;
using core::async::ResumeOn;
using core::async::Task;
using core::async::ThreadPoolExecutor;
using core::async::whenAll;
using core::async::whenAny;
using Queue = core::async::AsyncQueue<int>;
using namespace std::chrono_literals;

namespace
{

/// How long a case waits for the pool before it calls the machine wedged.
///
/// Generous rather than tuned: what is waited for is a handful of coroutine resumptions on an
/// idle pool, so only a very slow runner can make it long.
constexpr auto Budget = 5s;

/// A latch a case can hold work on, so "these ran at the same time" is a decision rather than a
/// race with the scheduler.
class Latch
{
  public:
    /// Blocks until `release()`, or until the budget runs out. Bounded, because an unbounded wait
    /// in a test that regresses hangs the suite rather than failing it.
    void wait()
    {
        auto guard = std::unique_lock { _mutex };
        std::ignore = _open.wait_for(guard, Budget, [this] { return _released; });
    }

    /// Lets everyone through, now and in future. Idempotent.
    void release()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _released = true;
        }
        _open.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _open;
    bool _released { false };
};

/// Counts arrivals and lets a case wait for a given number of them.
class Arrivals
{
  public:
    void arrive()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            ++_count;
        }
        _changed.notify_all();
    }

    /// @param many How many arrivals to wait for.
    /// @return Whether that many had arrived inside the budget.
    [[nodiscard]] bool waitFor(std::size_t many)
    {
        auto guard = std::unique_lock { _mutex };
        return _changed.wait_for(guard, Budget, [this, many] { return _count >= many; });
    }

    /// @return How many have arrived so far.
    [[nodiscard]] std::size_t count() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _count;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _count { 0 };
};

/// Releases a latch as it dies, so a case that fails on the way out still lets the pool's threads
/// off it. Declared AFTER the pool, so it runs BEFORE the pool joins them.
class LatchRelease
{
  public:
    explicit LatchRelease(Latch* latch) noexcept: _latch(latch) {}

    LatchRelease(LatchRelease const&) = delete;
    LatchRelease(LatchRelease&&) = delete;
    LatchRelease& operator=(LatchRelease const&) = delete;
    LatchRelease& operator=(LatchRelease&&) = delete;

    ~LatchRelease() { _latch->release(); }

  private:
    Latch* _latch;
};

/// What one job does once it lands on the pool.
///
/// POINTERS and values, no references and no captures. A coroutine's reference parameters are not
/// kept alive by its frame, and a capturing lambda coroutine is worse: the closure dies at the end
/// of the full expression that made it, while the frame goes on pointing into it. Both compile,
/// both pass while the stack happens to still hold the values, and both are use-after-free.
struct Job
{
    ThreadPoolExecutor* pool { nullptr }; ///< Where to run.
    Arrivals* arrived { nullptr };        ///< Counted on arrival, when given.
    Latch* hold { nullptr };              ///< Blocked on, when given.
    std::atomic<bool>* moved { nullptr }; ///< Set to "not the caller's thread".
    std::thread::id caller {};            ///< Which thread asked.
};

/// Hops onto the pool and does what the job says.
/// @param job What to do; every target must outlive the pool it runs on.
DetachedTask runOn(Job job)
{
    co_await ResumeOn { *job.pool };
    if (job.moved != nullptr)
        job.moved->store(std::this_thread::get_id() != job.caller, std::memory_order_release);
    if (job.arrived != nullptr)
        job.arrived->arrive();
    if (job.hold != nullptr)
        job.hold->wait();
}

/// One child of a join that hops onto the pool: it therefore FINISHES on a pool thread, which is
/// the thread that then decrements the join's counter.
/// @param pool Where to run.
/// @param arrived Counted once the child is on the pool.
Task<void> joinedJob(ThreadPoolExecutor* pool, Arrivals* arrived)
{
    co_await ResumeOn { *pool };
    arrived->arrive();
}

/// Joins @p children of them, detached so that nothing owns the chain and every park carries a
/// claim on its root.
/// @param pool Where the children run.
/// @param children How many to join.
/// @param arrived Counted per child.
/// @param joined Counted once the join completes.
DetachedTask joinOnPool(ThreadPoolExecutor* pool, std::size_t children, Arrivals* arrived, Arrivals* joined)
{
    auto tasks = std::vector<Task<void>> {};
    tasks.reserve(children);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, children))
        tasks.push_back(joinedJob(pool, arrived));

    co_await whenAll(std::move(tasks));
    joined->arrive();
}

/// A child that parks TWICE. The first `ResumeOn` suspends on the thread that started the join;
/// the SECOND one suspends on whichever pool thread resumed it -- which is the only way this suite
/// makes two threads take claims on one chain at the same moment. `claimOn` then runs concurrently
/// with a sibling's `claimOn`, with `Parked::resume()`'s `disarm()`, and with the release of a
/// claim whose `ParkedWork` is going out of scope on a third thread.
/// @param pool Where to run.
/// @param arrived Counted once both parks are behind it.
Task<void> twiceParkedJob(ThreadPoolExecutor* pool, Arrivals* arrived)
{
    co_await ResumeOn { *pool };
    co_await ResumeOn { *pool };
    arrived->arrive();
}

/// Joins @p children twice-parking children under one detached root, so one AbandonState sees
/// every claim, re-arm, disarm and release the pool can produce.
/// @param pool Where the children run.
/// @param children How many to join.
/// @param arrived Counted per child.
/// @param joined Counted once the join completes.
DetachedTask joinTwiceParked(ThreadPoolExecutor* pool,
                             std::size_t children,
                             Arrivals* arrived,
                             Arrivals* joined)
{
    auto tasks = std::vector<Task<void>> {};
    tasks.reserve(children);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, children))
        tasks.push_back(twiceParkedJob(pool, arrived));

    co_await whenAll(std::move(tasks));
    joined->arrive();
}

/// A loser: hops onto the pool, then parks on a queue that never delivers, so the only way it ever
/// finishes is the cancellation `whenAny` requests once a sibling wins.
Task<void> queueParkedRacer(ThreadPoolExecutor* pool, Queue* queue, Arrivals* unwound)
{
    co_await ResumeOn { *pool };
    try
    {
        static_cast<void>(co_await queue->pop());
    }
    catch (core::async::OperationCancelled const&)
    {
        unwound->arrive();
        throw;
    }
}

/// The winner: hops onto the pool and returns, which is what requests the stop.
Task<void> poolWinner(ThreadPoolExecutor* pool, Arrivals* arrived)
{
    co_await ResumeOn { *pool };
    arrived->arrive();
}

/// Races one winner against @p losers queue-parked children, all on the pool.
DetachedTask raceOnPool(ThreadPoolExecutor* pool,
                        std::vector<std::unique_ptr<Queue>>* queues,
                        Arrivals* arrived,
                        Arrivals* unwound,
                        Arrivals* raced)
{
    auto tasks = std::vector<Task<void>> {};
    tasks.reserve(queues->size() + 1);
    for (auto& queue: *queues)
        tasks.push_back(queueParkedRacer(pool, queue.get(), unwound));
    tasks.push_back(poolWinner(pool, arrived));

    static_cast<void>(co_await whenAny(std::move(tasks)));
    raced->arrive();
}

/// Structurally identical to joinOnPool, but raced rather than joined: same child count, same
/// single ResumeOn hop, same detached root. The only difference is the combinator, which is what
/// makes it a fair comparison.
DetachedTask raceOnPoolMirror(ThreadPoolExecutor* pool,
                              std::size_t children,
                              Arrivals* arrived,
                              Arrivals* raced)
{
    auto tasks = std::vector<Task<void>> {};
    tasks.reserve(children);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, children))
        tasks.push_back(joinedJob(pool, arrived));

    static_cast<void>(co_await whenAny(std::move(tasks)));
    raced->arrive();
}

} // namespace

// #1041 over the real pool. What genuinely closes the hazard is stronger than this line: both
// `IExecutor::submit` overloads are PURE virtual (`IExecutor.hpp:43,56`), so a concrete executor
// that declared only the borrowing half would hide the owning one, fail to override it, and stay
// abstract -- unusable rather than silently leaky. The residual shape is an intermediate abstract
// class declaring one half, which `-Woverloaded-virtual` (on GCC *and* clang, gated by
// `CORE_CPP_GCC_OR_CLANG` at `cmake/CoreCppToolchain.cmake:84`) and clang-tidy's
// `bugprone-derived-method-shadowing-base-method` both refuse. So this assertion is a reachability
// check over the real pool, not the guard; `ParkedWork_test.cpp` proves the concept discriminates,
// with a plain non-inheriting type offering only `submit(handle)` as its negative control.
static_assert(
    requires(ThreadPoolExecutor& pool) { pool.submit(core::async::ParkedWork {}); },
    "ThreadPoolExecutor::submit(ParkedWork) must be reachable through the derived type");

TEST_CASE("Work awaited onto a pool runs on its threads, not the caller's", "[ThreadPoolExecutor]")
{
    auto elsewhere = std::atomic<bool> { false };
    auto done = Arrivals {};
    // Declared LAST so it is joined FIRST: everything a job touches has to outlive the pool, and
    // locals are destroyed in reverse.
    auto pool = ThreadPoolExecutor { 2 };

    runOn(Job { .pool = &pool, .arrived = &done, .moved = &elsewhere, .caller = std::this_thread::get_id() });

    auto const settled = done.waitFor(1);
    INFO("arrivals: " << done.count() << " of 1");
    REQUIRE(settled);
    CHECK(elsewhere.load(std::memory_order_acquire));
}

TEST_CASE("A pool of N runs N pieces of blocking work at once", "[ThreadPoolExecutor]")
{
    // The whole reason a pool exists rather than an event loop: this work BLOCKS, and on one
    // thread the second piece could not start until the first finished.
    constexpr auto Threads = std::size_t { 3 };

    auto hold = Latch {};
    auto started = Arrivals {};
    auto pool = ThreadPoolExecutor { Threads };
    auto const releaseOnExit = LatchRelease { &hold };

    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Threads))
        runOn(Job { .pool = &pool, .arrived = &started, .hold = &hold });

    // All three are inside the blocking section together, which is the claim.
    auto const settled = started.waitFor(Threads);
    INFO("started: " << started.count() << " of " << Threads);
    CHECK(settled);
}

TEST_CASE("A pool never abandons a coroutine it was handed", "[ThreadPoolExecutor]")
{
    // An unresumed coroutine never runs its destructors and never frees its frame, so a queue
    // dropped at shutdown leaks every job in it along with whatever it holds. Both routes out are
    // checked: drained at stop, and resumed inline when the pool is already stopped.
    auto ran = Arrivals {};

    SECTION("queued when it stops")
    {
        {
            auto pool = ThreadPoolExecutor { 1 };
            for ([[maybe_unused]] auto const index: std::views::iota(0, 4))
                runOn(Job { .pool = &pool, .arrived = &ran });
        } // joins, draining what is left
        CHECK(ran.count() == 4);
    }

    SECTION("submitted after it stopped")
    {
        auto pool = ThreadPoolExecutor { 1 };
        pool.stop();
        runOn(Job { .pool = &pool, .arrived = &ran });
        // Resumed on this thread rather than dropped, so it still completed.
        CHECK(ran.count() == 1);
    }
}

TEST_CASE("A pool asked for no threads still runs its work", "[ThreadPoolExecutor]")
{
    // Zero would be a pool that accepts handles and resumes none of them, which is a leak wearing
    // the shape of an idle pool.
    auto ran = Arrivals {};
    auto pool = ThreadPoolExecutor { 0 };
    CHECK(pool.threads() == 1);

    runOn(Job { .pool = &pool, .arrived = &ran });

    auto const settled = ran.waitFor(1);
    INFO("arrivals: " << ran.count() << " of 1");
    CHECK(settled);
}

TEST_CASE("A join whose children finish on a pool completes exactly once", "[ThreadPoolExecutor][WhenAll]")
{
    // The join's counter is decremented by whichever thread resumed each child, and this module is
    // the one that makes such a child expressible: `ResumeOn` and this pool are its own vocabulary.
    // A plain `--remaining` here is a data race on the join state -- ThreadSanitizer says so, and a
    // variant of this case hung with every child finished and nobody resumed. The suite passed
    // before only by never joining anything on a pool.
    constexpr auto Children = std::size_t { 8 };

    auto arrived = Arrivals {};
    auto joined = Arrivals {};
    auto pool = ThreadPoolExecutor { 4 }; // last, so it is joined first -- see the case above

    joinOnPool(&pool, Children, &arrived, &joined);

    auto const settled = joined.waitFor(1);
    INFO("children on the pool: " << arrived.count() << " of " << Children
                                  << "; joins completed: " << joined.count());
    REQUIRE(settled);
    CHECK(arrived.count() == Children);
    // Exactly once: a lost decrement completes the join twice and resumes the root twice.
    CHECK(joined.count() == 1);
}

TEST_CASE("Claims on one chain are taken and given back from several threads at once",
          "[ThreadPoolExecutor][WhenAll]")
{
    // PROBE for the residual risk in R97's fix: `AbandonState::claim()` increments relaxed, and
    // `Parked::resume()` disarms before resuming while its own claim is released afterwards. Every
    // other case in the suite takes its claims on one thread, so neither had been exercised.
    // Sixteen children each parking twice on four threads means `claimOn` -- `call_once`, `rearm`,
    // `claim` -- runs concurrently with a sibling's `disarm` and a third claim's `release`, all on
    // one AbandonState.
    constexpr auto Children = std::size_t { 16 };

    auto arrived = Arrivals {};
    auto joined = Arrivals {};
    auto pool = ThreadPoolExecutor { 4 }; // last, so it is joined first

    joinTwiceParked(&pool, Children, &arrived, &joined);

    auto const settled = joined.waitFor(1);
    INFO("children past both parks: " << arrived.count() << " of " << Children
                                      << "; joins completed: " << joined.count());
    REQUIRE(settled);
    CHECK(arrived.count() == Children);
    CHECK(joined.count() == 1);
}

TEST_CASE("A race whose children park on a pool completes exactly once", "[ThreadPoolExecutor][WhenAny]")
{
    // The whenAny half of the cross-thread join, and the only case anywhere that covers a
    // cancellation whose losers are parked ON A POOL: they sit on a stop-aware `pop` at the moment
    // the winner requests the stop.
    //
    // WHAT IT CANNOT CATCH, measured rather than assumed. This case does NOT reproduce the
    // `claimOn` use-after-free that `ParkedWork.hpp` documents -- 0 crashes in 1920 runs where the
    // mirror case below gives 14 and the whenAll join gives 15. Its parks are created one at a
    // time on pool threads; the precondition for that defect is a BURST of parks from the start
    // loop while workers release. It is the richer test of what `whenAny` is for and the blind one
    // for how its ownership breaks, which is why the mirror below exists beside it: a case that
    // mirrors the SUBJECT can be clean while a case that mirrors the STRUCTURE reproduces.
    constexpr auto Losers = std::size_t { 4 };

    auto arrived = Arrivals {};
    auto unwound = Arrivals {};
    auto raced = Arrivals {};
    auto queues = std::vector<std::unique_ptr<Queue>> {};
    auto pool = ThreadPoolExecutor { 4 }; // last, so it is joined first

    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Losers))
        queues.push_back(std::make_unique<Queue>(pool, core::async::AsyncQueueOptions {}));

    raceOnPool(&pool, &queues, &arrived, &unwound, &raced);

    auto const settled = raced.waitFor(1);
    INFO("winner arrived: " << arrived.count() << "; losers unwound: " << unwound.count() << " of " << Losers
                            << "; races completed: " << raced.count());
    REQUIRE(settled);
    CHECK(raced.count() == 1);
}

TEST_CASE("A race of pool-hopping children, mirroring the join", "[ThreadPoolExecutor][WhenAny]")
{
    constexpr auto Children = std::size_t { 8 };

    auto arrived = Arrivals {};
    auto raced = Arrivals {};
    auto pool = ThreadPoolExecutor { 4 }; // last, so it is joined first

    raceOnPoolMirror(&pool, Children, &arrived, &raced);

    auto const settled = raced.waitFor(1);
    INFO("children on the pool: " << arrived.count() << " of " << Children
                                  << "; races completed: " << raced.count());
    REQUIRE(settled);
    CHECK(raced.count() == 1);
}
