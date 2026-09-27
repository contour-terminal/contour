// SPDX-License-Identifier: Apache-2.0
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using core::async::syncRun;
using core::async::syncRunWith;
using core::async::Task;
using core::async::whenAll;

namespace
{

/// A value with no default constructor, and no way to make one by accident.
///
/// `Task<T>` used to answer a null handle with `T {}`, which required every result type to be
/// default-constructible — a requirement no `co_return` needs and that a type like this cannot
/// meet.
class Measurement
{
  public:
    explicit Measurement(int value) noexcept: _value(value) {}

    Measurement() = delete;

    /// @return The value this was made with.
    [[nodiscard]] int value() const noexcept { return _value; }

  private:
    int _value;
};

static_assert(!std::is_default_constructible_v<Measurement>,
              "the point of this type is that Task must not need to default-construct its result");

/// A computational task that completes synchronously when first resumed.
Task<int> answer()
{
    co_return 42;
}

/// Composition: awaiting a child task and using its value.
Task<int> answerPlusOne()
{
    co_return co_await answer() + 1;
}

/// A void task with an observable side effect.
///
/// Counters/buffers are passed by pointer, not reference: clang-tidy's
/// cppcoreguidelines-avoid-reference-coroutine-parameters forbids reference
/// coroutine parameters (they dangle if the referent dies before the coroutine);
/// the pointee here is a test local that outlives the coroutine.
Task<void> increment(int* counter)
{
    ++*counter;
    co_return;
}

/// Deeply recursive task used to prove symmetric transfer keeps the stack O(1).
Task<int> sumDown(int n)
{
    if (n == 0)
        co_return 0;
    co_return 1 + co_await sumDown(n - 1);
}

/// A coroutine-frame sentinel: one per frame under test, counted when that frame dies.
///
/// Passed BY VALUE into a coroutine, which is both this repository's coroutine rule and what puts
/// it in the frame — a body local would not exist in a lazy `Task` that has never started. Move
/// aware, so the caller's temporary being destroyed at the end of the call expression is not
/// counted as the frame dying.
class FrameSentinel
{
  public:
    /// @param destroyed Incremented once when the frame holding this dies; never null.
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

/// A value task whose frame is counted when it dies.
Task<int> answerWithSentinel(FrameSentinel sentinel)
{
    (void) sentinel;
    co_return 42;
}

/// Awaits a task the CALLER still names, so a case can ask what became of that name.
///
/// `operator co_await` is rvalue-qualified, so awaiting a named local is spelled `std::move` —
/// and the whole question is whether that move is real.
Task<void> awaitNamedLocal(Task<int>* named, int* value, bool* stillOwnsAfter)
{
    *value = co_await std::move(*named);
    *stillOwnsAfter = static_cast<bool>(named->handle());
}

/// Produces a value of a type that cannot be default-constructed.
Task<Measurement> measure(int value)
{
    co_return Measurement { value };
}

/// Awaits one, so the awaiter is held to the same requirement as the task.
Task<Measurement> measureThenAdd(int value)
{
    auto const measured = co_await measure(value);
    co_return Measurement { measured.value() + 1 };
}

/// An awaitable that suspends and is never resumed by anybody — the shape of a socket read with
/// no data buffered and no closed peer to report EOF.
struct NeverCompletes
{
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    // The handle is deliberately dropped: nothing ever resumes this awaitable, which is the whole
    // point of the fixture.
    void await_suspend(std::coroutine_handle<> /*awaiting*/) const noexcept {}

    void await_resume() const noexcept {}
};

/// An awaitable that parks and remembers who, so a case can play the owner that takes the park
/// back — `ISocket::cancelRead()` in miniature.
struct Retrievable
{
    std::coroutine_handle<> parked {}; ///< Who parked here, until the owner takes it back.

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> awaiting) noexcept { parked = awaiting; }

    void await_resume() const noexcept {}
};

/// A value-returning task that parks before ever producing a result.
Task<int> parksForever()
{
    co_await NeverCompletes {};
    co_return 1;
}

/// A void task that parks before its side effect, so a case can assert the body past the suspend
/// point did not run.
Task<void> parksForeverVoid(int* sideEffect)
{
    co_await NeverCompletes {};
    *sideEffect = 1;
}

/// Parks on @p on and records reaching its end, so a case can tell a frame that finished from one
/// that was freed where it parked.
Task<int> parksOn(Retrievable* on, bool* finished)
{
    co_await *on;
    *finished = true;
    co_return 1;
}

/// An awaitable the test can complete by hand, simulating asynchronous I/O so the
/// `whenAll` join path (last child resumes the parent) is exercised under ASAN.
struct ManualEvent
{
    std::vector<std::coroutine_handle<>>* waiters;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> waiting) const { waiters->push_back(waiting); }

    void await_resume() const noexcept {}
};

/// Suspends on a ManualEvent, then increments a counter once resumed.
Task<void> waitThenIncrement(std::vector<std::coroutine_handle<>>* waiters, int* counter)
{
    co_await ManualEvent { waiters };
    ++*counter;
}

// The whenAll drivers below are free functions, not immediately-invoked lambdas:
// a lambda-coroutine's closure is a temporary destroyed once the Task is created,
// so a body that resumes later would read captures through a dangling `this`.
// Pointer parameters keep the (longer-lived) test locals reachable from the frame.

/// Runs two increments concurrently via whenAll.
Task<void> incrementTwiceConcurrently(int* counter)
{
    co_await whenAll(increment(counter), increment(counter));
}

/// Joins two manually-completed waiters, then marks completion.
Task<void> joinTwoWaiters(std::vector<std::coroutine_handle<>>* waiters, int* counter)
{
    co_await whenAll(waitThenIncrement(waiters, counter), waitThenIncrement(waiters, counter));
    *counter += 100; // marker proving the join resumed the parent exactly once
}

/// Awaits an empty whenAll, then records that it completed immediately.
Task<void> awaitEmptyWhenAll(bool* reached)
{
    co_await whenAll(std::vector<Task<void>> {});
    *reached = true;
}

/// Awaits a task owning no frame, of each kind, and counts the awaits that refused it by name.
Task<void> awaitEmptyTasks(int* refused)
{
    try
    {
        std::ignore = co_await Task<int> {};
    }
    catch (std::logic_error const&)
    {
        ++*refused;
    }
    try
    {
        co_await Task<void> {};
    }
    catch (std::logic_error const&)
    {
        ++*refused;
    }
}

} // namespace

TEST_CASE("Task produces a value when driven to completion", "[Task]")
{
    auto task = answer();
    REQUIRE_FALSE(task.done());

    task.handle().resume();

    REQUIRE(task.done());
    REQUIRE(task.result() == 42);
}

TEST_CASE("Task<void> runs its body to completion", "[Task]")
{
    auto counter = 0;
    auto task = increment(&counter);
    REQUIRE(counter == 0); // lazy: nothing runs before resume

    task.handle().resume();

    REQUIRE(task.done());
    REQUIRE(counter == 1);
}

TEST_CASE("Task composes via co_await", "[Task]")
{
    auto task = answerPlusOne();
    task.handle().resume();

    REQUIRE(task.done());
    REQUIRE(task.result() == 43);
}

TEST_CASE("Task is move-only and the moved-from frame is not double-freed", "[Task]")
{
    auto task = answer();
    auto moved = std::move(task);

    moved.handle().resume();

    REQUIRE(moved.done());
    REQUIRE(moved.result() == 42);
}

TEST_CASE("Awaiting a task takes its frame, leaving the name that held it empty", "[Task]")
{
    // The awaiter OWNS the task it was handed, rather than borrowing it from a `Task` value that
    // outlives the suspension. It is what lets a coroutine park inside the awaited task while the
    // caller that named it is somewhere the frame is not reachable from, and it is why
    // `operator co_await` is rvalue-qualified: the value is moved out, not read through.
    auto destroyed = 0;
    auto value = 0;
    auto stillOwnsAfter = true;
    {
        auto named = answerWithSentinel(FrameSentinel { &destroyed });
        auto root = awaitNamedLocal(&named, &value, &stillOwnsAfter);

        root.handle().resume();

        REQUIRE(root.done());
        CHECK(value == 42);
        // Emptied by the await, so the local cannot free it a second time...
        CHECK_FALSE(stillOwnsAfter);
        // ... and the awaiter already did, at the end of the `co_await` expression.
        CHECK(destroyed == 1);
    }
    // Freed exactly once: the local going out of scope here added nothing.
    CHECK(destroyed == 1);
}

TEST_CASE("Awaiting a task that already finished takes its value without resuming it again", "[Task]")
{
    // Decided when the `co_await` makes its awaiter, and read by `await_ready`, so the awaiting
    // coroutine never suspends (fastcached#1546). A finished frame resumed a second time is
    // undefined behaviour, so what this holds is that the child is never resumed, only read.
    auto destroyed = 0;
    auto value = 0;
    auto stillOwnsAfter = true;
    {
        auto named = answerWithSentinel(FrameSentinel { &destroyed });
        named.handle().resume();
        REQUIRE(named.done());

        auto root = awaitNamedLocal(&named, &value, &stillOwnsAfter);
        root.handle().resume();

        REQUIRE(root.done());
        CHECK(value == 42);
        CHECK_FALSE(stillOwnsAfter);
    }
    CHECK(destroyed == 1);
}

TEST_CASE("Awaiting a task owning no frame is refused by name, inside the awaiting coroutine", "[Task]")
{
    // The other half of what the awaiter's constructor decides. `await_resume` refuses the missing
    // frame, and the refusal is caught here by the awaiting coroutine's own `try` -- the handler
    // fastcached#1546 lost, twice: first with the decision in `await_ready` as a call, then, on the
    // `windows (cl-release-arm64)` leg, with it in an `await_suspend` that transferred straight back
    // to the awaiting coroutine (refused == 0: neither refusal was caught).
    auto refused = 0;
    auto root = awaitEmptyTasks(&refused);

    root.handle().resume();

    REQUIRE(root.done());
    CHECK(refused == 2);
}

TEST_CASE("A task owning no frame has no result to give", "[Task]")
{
    // `done()` is true for a task owning NO frame — default-constructed, moved from, released —
    // so `if (t.done()) t.result();` reaches this. It used to answer with a default-constructed
    // `T`, which invents a value the coroutine never produced and forces every `T` to be
    // default-constructible. There is no value that would be true, so it is refused by name.
    auto empty = Task<int> {};
    REQUIRE(empty.done());
    CHECK_THROWS_AS(empty.result(), std::logic_error);

    auto emptyVoid = Task<void> {};
    REQUIRE(emptyVoid.done());
    CHECK_THROWS_AS(emptyVoid.result(), std::logic_error);
}

TEST_CASE("A task produces a value whose type cannot be default-constructed", "[Task]")
{
    auto task = measureThenAdd(41);

    task.handle().resume();

    REQUIRE(task.done());
    CHECK(task.result().value() == 42);
}

TEST_CASE("release() hands the frame to the caller, and the task keeps nothing", "[Task]")
{
    auto destroyed = 0;
    auto handle = Task<int>::HandleType {};
    {
        auto task = answerWithSentinel(FrameSentinel { &destroyed });
        handle = task.release();
        REQUIRE(handle);
        CHECK_FALSE(task.handle());
    }

    // The task went out of scope empty, so its destructor freed nothing...
    CHECK(destroyed == 0);

    // ... and freeing it is now the caller's job, which is the whole of what release() means.
    handle.destroy();
    CHECK(destroyed == 1);
}

TEST_CASE("syncRun drives a task to its end and answers with its result", "[Task][syncRun]")
{
    CHECK(syncRun(answer()) == 42);
    CHECK(syncRun(answerPlusOne()) == 43);

    auto counter = 0;
    syncRun(increment(&counter));
    CHECK(counter == 1);
}

TEST_CASE("syncRun refuses a task that is still suspended", "[Task][syncRun]")
{
    // It used to read the promise's result unconditionally. For a task still parked after
    // resume() that names storage which was never engaged, and ~Task() then tears the frame down
    // while whatever parked the coroutine still points into it — which surfaced as a SIGSEGV, a
    // heap corruption or an abort rather than as a named failure.
    CHECK_THROWS_AS(syncRun(parksForever()), std::logic_error);

    auto sideEffect = 0;
    CHECK_THROWS_AS(syncRun(parksForeverVoid(&sideEffect)), std::logic_error);
    CHECK(sideEffect == 0);
}

TEST_CASE("syncRunWith takes the park back before refusing, so nothing points into a freed frame",
          "[Task][syncRun]")
{
    // The plain refusal frees a frame the parked read still points into, and the resource's next
    // completion then writes into freed memory. Taking the park back first is what makes the throw
    // the whole of the failure: the frame runs to its end, THEN is freed.
    auto park = Retrievable {};
    auto finished = false;
    auto retrievals = 0;

    CHECK_THROWS_AS(syncRunWith(parksOn(&park, &finished),
                                [&park, &retrievals] {
                                    ++retrievals;
                                    std::exchange(park.parked, {}).resume();
                                }),
                    std::logic_error);

    CHECK(retrievals == 1);
    CHECK(finished);
    CHECK_FALSE(park.parked);
}

TEST_CASE("syncRunWith asks nothing of the retriever when the task never parks", "[Task][syncRun]")
{
    auto retrievals = 0;
    auto const result = syncRunWith(answer(), [&retrievals] { ++retrievals; });

    CHECK(result == 42);
    CHECK(retrievals == 0);
}

TEST_CASE("syncRun refuses a task owning no frame rather than resuming a null handle", "[Task][syncRun]")
{
    CHECK_THROWS_AS(syncRun(Task<int> {}), std::logic_error);
    CHECK_THROWS_AS(syncRunWith(Task<int> {}, [] {}), std::logic_error);
}

TEST_CASE("Deep co_await chains keep the stack bounded (symmetric transfer)", "[Task]")
{
    // Without symmetric transfer this recursion would overflow the stack; with it
    // both the descent and the unwind are tail calls, where the compiler makes them so. Where it
    // does not, the case is skipped: a limit decided in 0.5.0 (core-cpp#15, docs/modules/async.md).
    //
    // The teardown is not covered by that reasoning and does not need to be: each level destroys
    // the child it awaited at the end of its own `co_return` expression, by which time that child
    // has already destroyed ITS child, so the chain is released one frame at a time as it unwinds.
    // A chain destroyed BEFORE it completes is the recursive case -- one `~Task` per level, all
    // nested -- and this case never has one.
#if defined(__EMSCRIPTEN__) && !defined(__wasm_tail_call__)
    // Measured with emsdk 3.1.56 under node: "RangeError: Maximum call stack size exceeded". Its
    // WebAssembly has no tail calls unless built with -mtail-call, so each transfer nests a call.
    SKIP("WebAssembly without -mtail-call has no tail call for symmetric transfer (core-cpp#15)");
#elif defined(__GNUC__) && !defined(__clang__) && !defined(CORE_ASYNC_SYMMETRIC_TRANSFER_IS_TAIL_CALL)
    // Measured with GCC 14.3 and 15: the recursion overflows an 8 MiB stack at -O0, -Og and -O1,
    // and passes at -O2 and -O3. GCC makes the transfer a tail call only when it optimises sibling
    // calls, and it exposes no macro for the level -- __OPTIMIZE__ is 1 at -Og and -O1 too, so
    // keying on it left this case running there, where it crashes the process and takes every
    // other case in the binary with it. Whoever builds decides: src/core/async/CMakeLists.txt
    // reads the level off the build's own flags and defines the macro below only at -O2 or better,
    // so a build outside core-cpp's presets -- a distro package, a bisect, -Og for a debugger --
    // skips this case instead of losing the binary (core-cpp#15).
    SKIP("this GCC build does not optimise sibling calls, so symmetric transfer is not a tail call "
         "(core-cpp#15)");
#endif
    constexpr auto Depth = 100000;
    auto task = sumDown(Depth);

    task.handle().resume();

    REQUIRE(task.done());
    REQUIRE(task.result() == Depth);
}

TEST_CASE("whenAll completes synchronously when every child does", "[Task][whenAll]")
{
    auto counter = 0;
    auto root = incrementTwiceConcurrently(&counter);

    root.handle().resume();

    REQUIRE(root.done());
    REQUIRE(counter == 2);
}

TEST_CASE("whenAll resumes the awaiting coroutine only after the last child finishes", "[Task][whenAll]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto counter = 0;
    auto root = joinTwoWaiters(&waiters, &counter);

    root.handle().resume();
    REQUIRE(waiters.size() == 2);
    REQUIRE(counter == 0);
    REQUIRE_FALSE(root.done());

    waiters[0].resume(); // first child finishes; not the last
    REQUIRE(counter == 1);
    REQUIRE_FALSE(root.done());

    waiters[1].resume(); // last child finishes; tail-transfers back to the parent
    REQUIRE(counter == 102);
    REQUIRE(root.done());
}

TEST_CASE("whenAll over no tasks completes immediately", "[Task][whenAll]")
{
    auto reached = false;
    auto root = awaitEmptyWhenAll(&reached);

    root.handle().resume();

    REQUIRE(root.done());
    REQUIRE(reached);
}

#ifndef _WIN32
// Exception propagation through a coroutine frame crashes the Catch2 harness on
// Windows (an MSVC coroutine-unwind interaction that also affects std::generator).
// The behavior is correct on every platform; we simply cannot assert it through
// the harness on Windows.
namespace
{
Task<int> throwsViaTask()
{
    throw std::runtime_error("boom");
    co_return 0; // unreachable; makes this a coroutine
}
} // namespace

TEST_CASE("Task captures and rethrows a body exception", "[Task]")
{
    auto task = throwsViaTask();
    task.handle().resume();

    REQUIRE(task.done());
    REQUIRE_THROWS_AS(task.result(), std::runtime_error);
}
#else
// The case above cannot be compiled here, so it SAYS so rather than vanishing. A compiled-out
// case reports nothing at all: the Windows binary would list three fewer cases than the Linux one
// with nothing to explain the gap, and `.agent/rules/testing.md` asks for a `SKIP` precisely so
// that a case which could not run names itself and why. The `#ifndef` stays -- the reason for it
// is sound -- and only the silence goes.
TEST_CASE("Task captures and rethrows a body exception", "[Task]")
{
    SKIP("exception propagation through a coroutine frame crashes the Catch2 harness on MSVC");
}
#endif
