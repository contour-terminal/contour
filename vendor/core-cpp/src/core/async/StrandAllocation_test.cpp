// SPDX-License-Identifier: Apache-2.0
//
// What a strand does when an allocation it needs fails.
//
// A strand allocates its pump's frame lazily, on the submit that finds it without one, and again
// when a task's throw out of `resume()` kills the pump. Either allocation can fail, and a strand
// that had already published "a pump is scheduled" when it did would never schedule one again --
// every later submit would queue behind it, and `~Strand` would wait for a pump that is not
// running. This binary replaces the global allocation functions so a case can fail exactly the
// next one, which is why it is a binary of its own: a replacement reaches every test linked beside
// it.
#include <core/async/KeyedStrands.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Strand.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace
{

/// How many allocations to serve before failing one; negative serves them all. Per thread, so
/// Catch2's own bookkeeping on another thread is never the one that fails.
thread_local int allocationsBeforeFailure = -1;

/// How many allocations this thread has been served.
thread_local std::size_t allocationsServed = 0;

/// Serves an allocation from `malloc`, or fails it when the countdown reaches zero.
/// @param size The size asked for.
/// @return The storage.
/// @throws std::bad_alloc When the countdown says so, or when `malloc` refuses.
void* countedAllocation(std::size_t size)
{
    if (allocationsBeforeFailure == 0)
    {
        allocationsBeforeFailure = -1;
        throw std::bad_alloc {};
    }
    if (allocationsBeforeFailure > 0)
        --allocationsBeforeFailure;
    ++allocationsServed;
    if (auto* const storage = std::malloc(size == 0 ? 1 : size))
        return storage;
    throw std::bad_alloc {};
}

/// A base that queues in fixed storage, so what a census counts is the strand's allocations alone
/// -- a `std::deque` allocates a block every so many entries.
class FixedExecutor final: public core::async::IExecutor
{
  public:
    using IExecutor::submit;

    void submit(std::coroutine_handle<> handle) override
    {
        assert(_tail - _head < _queued.size());
        _queued.at(_tail++ % _queued.size()) = handle;
    }

    void submit(core::async::ParkedWork work) override
    {
        work.abandon.disarm(); // only pumps and borrowed handles come here
        submit(work.resume);
    }

    /// Resumes everything queued, including what that queues.
    void drain()
    {
        while (_head != _tail)
            _queued.at(_head++ % _queued.size()).resume();
    }

  private:
    std::array<std::coroutine_handle<>, 8> _queued {};
    std::size_t _head { 0 };
    std::size_t _tail { 0 };
};

/// Records that it ran, then ends.
core::async::Task<void> record(std::vector<int>* out, int value)
{
    out->push_back(value);
    co_return;
}

#if !defined(_MSC_VER) || defined(__clang__)
/// A coroutine whose `resume()` throws -- see `Strand_test.cpp` -- and which arms the allocation
/// failure on its way out, so the allocation that fails is the replacement pump's.
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

/// What the task throws: a type that allocates nothing, so the allocation the countdown fails is
/// the replacement pump's and not the exception's own message.
struct ThrownOutOfResume
{
};

/// Throws out of `resume()`, having armed the next allocation to fail.
ThrowingResume throwAndFailNextAllocation(bool really)
{
    if (really)
    {
        allocationsBeforeFailure = 0;
        throw ThrownOutOfResume {};
    }
    co_return;
}
#endif

} // namespace

// The replaced global allocation functions are in ReplacedGlobalAllocation.cpp, which forwards
// them here (its header says why they are not in this file).
void* core::testing::replacedAllocate(std::size_t size)
{
    return countedAllocation(size);
}

void core::testing::replacedRelease(void* storage) noexcept
{
    std::free(storage);
}

TEST_CASE("A submit whose allocation fails changes nothing, whichever allocation it is",
          "[Strand][exceptions]")
{
    // A strand's first submit allocates the pump's frame and room in its queue, in an order that is
    // the implementation's business. Each allocation is failed in turn, until a submit needs no
    // more than the ones that were let through: every refused submit must leave nothing queued and
    // nothing scheduled -- a pump published as scheduled that nobody will run is a strand no later
    // submit can restart, and a queued task whose submitter was told no runs anyway.
    auto failures = 0;
    auto reachedSuccess = false;
    for (auto const failAt: { 0, 1, 2, 3, 4, 5, 6, 7 })
    {
        auto base = core::async::testing::ManualExecutor {};
        auto strand = core::async::Strand { base };
        auto order = std::vector<int> {};
        order.reserve(4);
        auto refused = record(&order, 1);
        auto accepted = record(&order, 2);

        allocationsBeforeFailure = failAt;
        auto threw = false;
        try
        {
            strand.submit(refused.handle());
        }
        catch (std::bad_alloc const&)
        {
            threw = true;
        }
        allocationsBeforeFailure = -1;
        if (!threw)
        {
            INFO("the first submit needed " << failAt << " allocation(s)");
            CHECK(failures > 0);
            reachedSuccess = true;
            break;
        }
        ++failures;
        INFO("allocation " << failAt << " failed");
        CHECK(strand.queued() == 0);
        CHECK(base.pending() == 0);

        strand.submit(accepted.handle());
        std::ignore = base.drain();
        CHECK(order == std::vector { 2 });
        CHECK_FALSE(refused.done());
    }
    // Every allocation a first submit makes was failed once: the loop ended on a submit that needed
    // no more, not by running out of positions to try.
    CHECK(reachedSuccess);
}

TEST_CASE("A strand whose replacement pump cannot be allocated keeps its queue and restarts on the "
          "next submit",
          "[Strand][exceptions]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    SKIP("under MSVC's cl a throw out of resume() on a strand terminates the process "
         "(core-cpp.strand-throw-canary)");
#else
    auto base = core::async::testing::ManualExecutor {};
    auto strand = core::async::Strand { base };
    auto order = std::vector<int> {};
    order.reserve(4);

    auto const thrower = throwAndFailNextAllocation(true);
    auto behind = record(&order, 1);
    strand.submit(thrower.handle());
    strand.submit(behind.handle());

    // The task's exception was replaced by the allocation failure of the pump meant to take over.
    CHECK_THROWS_AS(base.drain(), std::bad_alloc);
    allocationsBeforeFailure = -1;
    CHECK(order.empty());
    CHECK(strand.queued() == 1);

    // Nothing is scheduled, but nothing is wedged: the next submit makes a pump, and the task that
    // was waiting runs first.
    auto next = record(&order, 2);
    strand.submit(next.handle());
    std::ignore = base.drain();
    CHECK(order == std::vector { 1, 2 });
#endif
}

TEST_CASE("A key whose replacement pump cannot be allocated leaves no strand behind", "[Strand][exceptions]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    SKIP("under MSVC's cl a throw out of resume() on a strand terminates the process "
         "(core-cpp.strand-throw-canary)");
#else
    // The key's strand ran dry with its pump dead and no replacement: before the fix it stayed in
    // the registry, idle, with nothing left to retire it -- size() 1, and waitIdle() for ever.
    auto base = core::async::testing::ManualExecutor {};
    auto strands = core::async::KeyedStrands<int> { base };

    auto const thrower = throwAndFailNextAllocation(true);
    strands.submit(3, thrower.handle());
    CHECK_THROWS_AS(base.drain(), std::bad_alloc);
    allocationsBeforeFailure = -1;
    CHECK(strands.size() == 0);
    #if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    if (strands.size() == 0)
        strands.waitIdle();
    #endif
#endif
}

TEST_CASE("A post costs one allocation, the call; a submit none, once the strand is warm",
          "[Strand][post][allocation]")
{
    auto base = FixedExecutor {};
    auto strand = core::async::Strand { base };
    auto count = 0;

    // Warm: the pump's frame and the queue's room are made by the first work, once.
    strand.post([&count] { ++count; });
    base.drain();

    auto before = allocationsServed;
    strand.post([&count] { ++count; });
    auto const posted = allocationsServed - before;
    before = allocationsServed;
    base.drain();
    auto const ran = allocationsServed - before;
    CHECK(posted == 1);
    CHECK(ran == 0);
    CHECK(count == 2);

    auto order = std::vector<int> {};
    order.reserve(4);
    auto task = record(&order, 1);
    before = allocationsServed;
    strand.submit(task.handle());
    base.drain();
    CHECK(allocationsServed - before == 0);
    CHECK(order == std::vector { 1 });
}

TEST_CASE("A post to a busy key costs one allocation, the call", "[KeyedStrands][post][allocation]")
{
    auto base = FixedExecutor {};
    auto strands = core::async::KeyedStrands<int> { base };
    auto count = 0;

    strands.post(1, [&count] { ++count; }); // makes key 1's strand, and keeps it busy until drained
    auto const before = allocationsServed;
    strands.post(1, [&count] { ++count; });
    CHECK(allocationsServed - before == 1);
    base.drain();
    CHECK(count == 2);
    CHECK(strands.size() == 0);
}

TEST_CASE("A key whose first submit cannot allocate leaves no strand behind, whichever allocation it is",
          "[KeyedStrands][exceptions]")
{
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
    SKIP("MSVC's checked iterators allocate a container proxy inside std::vector's noexcept default "
         "constructor, so failing that allocation ends the process in the standard library");
#endif
    // The key's strand is made, registered, and then fails to take the work: before the fix it stayed
    // registered with nothing queued and nothing to retire it -- size() 1, and waitIdle() for ever.
    auto failures = 0;
    auto reachedSuccess = false;
    for (auto const failAt: { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 })
    {
        auto base = core::async::testing::ManualExecutor {};
        auto strands = core::async::KeyedStrands<int> { base };
        auto order = std::vector<int> {};
        order.reserve(4);
        auto refused = record(&order, 1);
        auto accepted = record(&order, 2);

        allocationsBeforeFailure = failAt;
        auto threw = false;
        try
        {
            strands.submit(7, refused.handle());
        }
        catch (std::bad_alloc const&)
        {
            threw = true;
        }
        allocationsBeforeFailure = -1;
        if (!threw)
        {
            INFO("the first submit needed " << failAt << " allocation(s)");
            CHECK(failures > 0);
            reachedSuccess = true;
            break;
        }
        ++failures;
        INFO("allocation " << failAt << " failed");
        CHECK(strands.size() == 0);
        CHECK(base.pending() == 0);

        strands.submit(7, accepted.handle());
        std::ignore = base.drain();
        CHECK(order == std::vector { 2 });
        CHECK_FALSE(refused.done());
        CHECK(strands.size() == 0);
    }
    CHECK(reachedSuccess);
}

TEST_CASE("A post to an idle key costs one allocation, the call, and a submit none, once warm",
          "[KeyedStrands][post][allocation]")
{
    // A key with no work has no strand; the strand its last work retired is kept -- pump frame,
    // queue room and map node -- and given to the next key that needs one. So in the steady state a
    // post to an idle key allocates the call and nothing else, as a post to a busy key does.
    auto base = FixedExecutor {};
    auto strands = core::async::KeyedStrands<int> { base };
    auto count = 0;
    auto order = std::vector<int> {};
    order.reserve(8);
    strands.post(1, [&count] { ++count; }); // warms: a strand, its pump, its room, a node, the buckets
    base.drain();
    REQUIRE(strands.size() == 0);

    auto before = allocationsServed;
    strands.post(1, [&count] { ++count; });
    auto const idleSameKey = allocationsServed - before;
    base.drain();

    before = allocationsServed;
    strands.post(2, [&count] { ++count; }); // another key: the kept strand is given to it
    auto const idleOtherKey = allocationsServed - before;
    base.drain();

    auto task = record(&order, 1);
    before = allocationsServed;
    strands.submit(3, task.handle());
    auto const idleSubmit = allocationsServed - before;
    base.drain();

    CHECK(idleSameKey == 1);
    CHECK(idleOtherKey == 1);
    CHECK(idleSubmit == 0);
    CHECK(count == 3);
    CHECK(order == std::vector { 1 });
    CHECK(strands.size() == 0);
}
