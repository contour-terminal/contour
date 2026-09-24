// SPDX-License-Identifier: Apache-2.0
//
// What an executor owes the coroutines it is holding when it goes away
// ([fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
//
// An executor destroyed with work parked resumed none of it and freed none of it: its queue was a
// container of non-owning handles. Every frame in it, and everything reachable from it, leaked --
// reported by LeakSanitizer as an INDIRECT-ONLY set, which is what a `Task` chain looks like when
// it refers to itself through its own continuations.
//
// **These cases do not rely on a sanitizer noticing.** A leak reported only by LSan is a red once
// in N runs and reads as a flake; every case here counts destructions with a frame sentinel and
// asserts the number, so it fails for its own reason on every platform.
//
// **Both directions, and never both at once.** Freeing everything at teardown is as wrong as
// freeing nothing: work whose frame something else owns must be LEFT ALONE, or the executor
// double-frees what a live `Task` is about to destroy. The answer to *destroy or resume* is
// neither on its own: an executor frees exactly the chains `detail::parkedWorkFor` says nothing
// owns.
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

using core::async::DetachedTask;
using core::async::IExecutor;
using core::async::ParkedWork;
using core::async::ResumeOn;
using core::async::Task;
using core::async::whenAll;
using core::async::whenAny;

namespace
{

/// What each case counts.
struct Counters
{
    int parked { 0 };    ///< Coroutines that reached their suspend point.
    int completed { 0 }; ///< Coroutine bodies that ran to their end.
    int destroyed { 0 }; ///< Frames freed, counted by the sentinel each carries.
};

/// A coroutine-frame sentinel: one per frame under test, counted when the frame dies.
///
/// Passed BY VALUE into every coroutine here, which is both this repository's coroutine rule and
/// what puts it in the frame -- a body local would not exist in a lazy `Task` that has never
/// started, and some of these cases are about exactly such a task. Move-aware, so the caller's
/// temporary being destroyed at the end of the call expression does not count as the frame dying.
class FrameSentinel
{
  public:
    /// @param counters Where the destruction is tallied; never null.
    explicit FrameSentinel(Counters* counters) noexcept: _counters(counters) {}

    FrameSentinel(FrameSentinel&& other) noexcept: _counters(std::exchange(other._counters, nullptr)) {}

    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_counters != nullptr)
            ++_counters->destroyed;
    }

  private:
    Counters* _counters;
};

/// An executor that queues and never runs anything by itself, so a case decides when — and
/// whether — parked work is resumed.
///
/// It is the minimum an executor must be to answer the ownership question: it holds its parks in
/// `detail::Parked` entries, which free exactly the chains nobody else owns when the container
/// goes. A deterministic test double for `core::async` alone; `core::net::testing::TestLoop` is
/// the one with timers and a clock.
class QueuedExecutor final: public IExecutor
{
  public:
    using IExecutor::submit;

    void submit(std::coroutine_handle<> handle) override
    {
        _parked.emplace_back(ParkedWork { .resume = handle });
    }

    void submit(ParkedWork work) override { _parked.emplace_back(work); }

    /// @return How many parks are held right now.
    [[nodiscard]] std::size_t pending() const noexcept { return _parked.size(); }

    /// Takes the first entry off, the way a loop's `cancelPending` hands a park back to its owner.
    /// @return What was parked there.
    [[nodiscard]] ParkedWork takeFront()
    {
        auto work = _parked.front().take();
        _parked.erase(_parked.begin());
        return work;
    }

    /// Resumes everything queued, in order, giving each chain back to whoever owns it.
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

/// A type that offers both overloads of `submit`, and one that offers only the borrowing half.
///
/// The second is what a derived class that re-declares `submit(handle)` and inherits its sibling
/// reduces to, by name lookup -- the shape of
/// [fastcached#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041), where every
/// call through the derived type bound to the borrowing overload and nothing diagnosed it.
///
/// Written WITHOUT a base class, because the inheriting form no longer compiles here at all: GCC's
/// `-Woverloaded-virtual` and clang-tidy's `bugprone-derived-method-shadowing-base-method` each
/// refuse it, and both are errors under core-cpp's warning set. That is two lines of defence the
/// check below does not have, and they are worth more than a faithful reproduction of the defect.
struct BothSubmitOverloads
{
    void submit(std::coroutine_handle<> /*handle*/) {}

    /// By value and consumed, exactly as `IExecutor::submit(ParkedWork)` is declared. The sink is
    /// not decoration: `ParkedWork::abandon` refcounts, so a parameter merely read would be a copy
    /// per call, and clang-tidy's `performance-unnecessary-value-param` would have this control
    /// rewritten to take a const reference -- which is no longer the signature it is a control for.
    void submit(ParkedWork work) { [[maybe_unused]] auto const sunk = std::move(work); }
};

/// What the hiding leaves reachable.
struct OnlyBorrowingSubmit
{
    void submit(std::coroutine_handle<> /*handle*/) {}
};

/// Satisfied where `submit(ParkedWork {})` is a call on @p E: the owning overload is reachable
/// through that type, rather than hidden by a re-declaration of its sibling.
template <typename E>
concept TakesOwnedWork = requires(E& executor) { executor.submit(ParkedWork {}); };

/// Satisfied where `submit(handle)` is a call on @p E.
template <typename E>
concept TakesBorrowedWork = requires(E& executor) { executor.submit(std::coroutine_handle<> {}); };

/// A lazy task that runs to its end the moment it is resumed.
Task<void> immediate(FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    ++counters->completed;
    co_return;
}

/// A detached chain parked on an executor.
DetachedTask parkDetached(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    ++counters->parked;
    co_await ResumeOn { *executor };
    ++counters->completed;
}

/// The innermost frame of a nested chain: it is what the executor actually holds.
Task<void> parkInner(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    ++counters->parked;
    co_await ResumeOn { *executor };
    ++counters->completed;
}

/// The middle frame: it owns `parkInner`'s frame through its awaiter and is itself owned by the
/// root's.
Task<void> parkMiddle(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    co_await parkInner(executor, FrameSentinel { counters }, counters);
}

/// A detached chain three frames deep, the shape #1025 was reported on.
DetachedTask parkNested(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    co_await parkMiddle(executor, FrameSentinel { counters }, counters);
    ++counters->completed;
}

/// A chain whose middle step is a `whenAll`, so the park happens inside a combinator's runner.
DetachedTask parkUnderWhenAll(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    co_await whenAll(parkInner(executor, FrameSentinel { counters }, counters));
    ++counters->completed;
}

/// The same, through `whenAny`.
DetachedTask parkUnderWhenAny(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    std::ignore = co_await whenAny(parkInner(executor, FrameSentinel { counters }, counters));
    ++counters->completed;
}

/// A `whenAll` over TWO children that both park: the shape a fan-out actually has.
DetachedTask parkTwoUnderWhenAll(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    co_await whenAll(parkInner(executor, FrameSentinel { counters }, counters),
                     parkInner(executor, FrameSentinel { counters }, counters));
    ++counters->completed;
}

/// The same, through `whenAny`.
DetachedTask parkTwoUnderWhenAny(IExecutor* executor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    std::ignore = co_await whenAny(parkInner(executor, FrameSentinel { counters }, counters),
                                   parkInner(executor, FrameSentinel { counters }, counters));
    ++counters->completed;
}

} // namespace

// #1041, asserted at compile time rather than left to a call site: a derived interface that
// re-declares one overload of a name hides every other overload of it, and the compiler says
// nothing at the declaration. The negative control is what makes this check mean something --
// without it, an assertion that the owning overload is reachable would hold for a type that
// hides nothing and prove no rule.
static_assert(TakesOwnedWork<IExecutor>, "the interface offers the owning overload");
static_assert(TakesBorrowedWork<IExecutor>, "and the borrowing one");
static_assert(TakesOwnedWork<BothSubmitOverloads>);
static_assert(TakesBorrowedWork<BothSubmitOverloads>);
static_assert(!TakesOwnedWork<OnlyBorrowingSubmit>,
              "a type that offers only submit(handle) does not take owned work: this is what the "
              "hiding in #1041 left every caller with");
static_assert(TakesBorrowedWork<OnlyBorrowingSubmit>, "while the overload it did declare stays reachable");
static_assert(TakesOwnedWork<QueuedExecutor>, "so every executor in this tree takes owned work");
static_assert(TakesBorrowedWork<QueuedExecutor>);

TEST_CASE("A detached chain parked on an executor at teardown is freed exactly once", "[ParkedWork]")
{
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkDetached(&executor, FrameSentinel { &counters }, &counters);

        // Parked, asserted rather than assumed: a DetachedTask runs eagerly to its first
        // suspension, and a body that had NOT suspended would have completed.
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);
        REQUIRE(counters.destroyed == 0);
        REQUIRE(executor.pending() == 1);
    }

    CHECK(counters.destroyed == 1);
    // Freed rather than resumed: nothing ran the rest of the body, which is the whole reason this
    // is safe to do at teardown.
    CHECK(counters.completed == 0);
}

TEST_CASE("A detached chain that IS dequeued is resumed rather than freed", "[ParkedWork]")
{
    // Without this half, an executor could free everything it holds and every other case here
    // would still pass.
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkDetached(&executor, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);

        CHECK(executor.drain() == 1);

        // Resumed, ran to its end, and a DetachedTask frees its own frame there.
        CHECK(counters.completed == 1);
        CHECK(counters.destroyed == 1);
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("An abandoned chain is freed from its root rather than the frame that parked", "[ParkedWork]")
{
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkNested(&executor, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.destroyed == 0);
    }

    // Three frames -- the detached root, the task it awaits, the task that parks -- each freed
    // once. Freeing the frame the executor HOLDS would give one: the two above it are reachable
    // only through each other, which is exactly the indirect-only leak set #1025 was reported
    // with.
    CHECK(counters.destroyed == 3);
    CHECK(counters.completed == 0);
}

TEST_CASE("An executor leaves parked work whose frame something else owns alone", "[ParkedWork]")
{
    // Mandatory rather than decoration: freeing everything at teardown is as wrong as freeing
    // nothing. This frame is owned by the `Task` below, so an executor that freed what it merely
    // borrows would double-free here -- and this case is what would catch it.
    auto counters = Counters {};
    {
        // A lazy task, never started: its frame and its by-value parameters exist, and the `Task`
        // object is what frees them.
        auto owned = immediate(FrameSentinel { &counters }, &counters);
        {
            auto executor = QueuedExecutor {};
            executor.submit(owned.handle());
            REQUIRE(executor.pending() == 1);
        }

        // The executor was destroyed holding this handle and did not touch it. A teardown that
        // freed what it merely borrows would have freed it here, and the line below would then be
        // a use-after-free rather than a failed check.
        CHECK(counters.destroyed == 0);
        CHECK(counters.completed == 0);
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("unownedRoot reaches a coroutine parked underneath a whenAll runner", "[ParkedWork][WhenAll]")
{
    // The runner is a coroutine type of its own between the detached root and the task that
    // parks. If it does not carry the chain's ownership down, the park it holds says "somebody
    // owns this", the executor leaves it alone, and the whole chain leaks -- four frames, none of
    // them reachable.
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkUnderWhenAll(&executor, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(executor.pending() == 1);
        REQUIRE(counters.destroyed == 0);
    }

    // Two sentinels: the detached root and the task that parked. The runner frame between them
    // carries none, and it is freed with them -- the awaiter that owns it lives in the root.
    CHECK(counters.destroyed == 2);
    CHECK(counters.completed == 0);
}

TEST_CASE("unownedRoot reaches a coroutine parked underneath a whenAny runner", "[ParkedWork][WhenAny]")
{
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkUnderWhenAny(&executor, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(executor.pending() == 1);
        REQUIRE(counters.destroyed == 0);
    }

    CHECK(counters.destroyed == 2);
    CHECK(counters.completed == 0);
}

TEST_CASE("Two parks of one chain free it once between them", "[ParkedWork][WhenAll]")
{
    // A fan-out hands the SAME chain root to every child, so an executor ends up holding N entries
    // that name one root. An entry that frees it unconditionally frees it N times: the second
    // destroy runs over a frame the first one released, which is a heap-use-after-free rather than
    // a leak, and at -O2 a crash.
    //
    // The invariant the single-child cases above cannot state is **at most one live park per
    // unowned root**. It was true of a linear chain -- one frame parks, one park names the root --
    // and stopped being true the moment `unownedRoot` reached a combinator's children.
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkTwoUnderWhenAll(&executor, FrameSentinel { &counters }, &counters);

        // Both children parked, so the executor holds two entries naming one root.
        REQUIRE(counters.parked == 2);
        REQUIRE(executor.pending() == 2);
        REQUIRE(counters.destroyed == 0);
    }

    // The detached root and the two children that parked, each freed exactly once.
    CHECK(counters.destroyed == 3);
    CHECK(counters.completed == 0);
}

TEST_CASE("Two parks of one raced chain free it once between them", "[ParkedWork][WhenAny]")
{
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkTwoUnderWhenAny(&executor, FrameSentinel { &counters }, &counters);

        REQUIRE(counters.parked == 2);
        REQUIRE(executor.pending() == 2);
        REQUIRE(counters.destroyed == 0);
    }

    CHECK(counters.destroyed == 3);
    CHECK(counters.completed == 0);
}

TEST_CASE("A parked entry that cannot resume frees the chain it owns", "[ParkedWork]")
{
    // `Parked::resume()` takes the work out before it decides whether it can resume, so a handle
    // it declines to resume would have its claim DROPPED rather than released -- a silent leak on
    // the one path this type exists to close. The contract is *resumed or freed, never neither*.
    //
    // **Driven at the primitive, because no executor reaches this branch today.** A chain whose
    // claim is set is owned by nobody, so nothing else can have resumed it to completion while the
    // executor held it; the branch is written for the contract rather than for a caller that
    // exists.
    auto spent = Counters {};
    auto owned = Counters {};

    // Run to its final suspension, so it is a handle `resume()` will decline.
    auto done = immediate(FrameSentinel { &spent }, &spent);
    done.handle().resume();
    REQUIRE(done.done());

    auto executor = QueuedExecutor {};
    parkDetached(&executor, FrameSentinel { &owned }, &owned);
    REQUIRE(executor.pending() == 1);

    // A real claim on a real detached chain: the only way to have one, and the only way this case
    // exercises what an executor actually holds.
    auto taken = executor.takeFront();
    {
        auto parked = core::async::detail::Parked { ParkedWork { .resume = done.handle(),
                                                                 .abandon = std::move(taken.abandon) } };
        REQUIRE(owned.destroyed == 0);

        parked.resume();
        CHECK(owned.destroyed == 1);
    }

    // Freed once, by the resume that declined -- not again when the entry died, and not by having
    // been run.
    CHECK(owned.destroyed == 1);
    CHECK(owned.completed == 0);
}

TEST_CASE("Two claims on one chain free it once between them", "[ParkedWork]")
{
    // R97 at the primitive: the invariant is one FREE per root, not one park per root. Two entries
    // naming one chain release in turn, and only the second one frees.
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkTwoUnderWhenAll(&executor, FrameSentinel { &counters }, &counters);
        REQUIRE(executor.pending() == 2);

        auto first = executor.takeFront();
        auto second = executor.takeFront();
        CHECK(first.abandon.sameChainAs(second.abandon));
        CHECK(first.abandon.armed());

        first = ParkedWork {};
        // One claim gone, one left: the chain is still there, which is the half a single-park case
        // cannot state.
        CHECK(counters.destroyed == 0);

        second = ParkedWork {};
        CHECK(counters.destroyed == 3);
    }
    CHECK(counters.destroyed == 3);
}

TEST_CASE("Resuming one park of a chain disarms every claim on it", "[ParkedWork]")
{
    // The other half of R97, and the one a refcount alone does not give: a chain that is running
    // again belongs to whoever owns it, so the sibling park still queued must free nothing. Were
    // it otherwise, the fix would trade a double free for a use-after-free on the resumed chain.
    auto counters = Counters {};
    {
        auto executor = QueuedExecutor {};
        parkTwoUnderWhenAll(&executor, FrameSentinel { &counters }, &counters);
        REQUIRE(executor.pending() == 2);

        CHECK(executor.drain() == 2);

        // Both children ran on, the join completed, and the detached root freed its own frame --
        // so the claims have nothing left to free and must know it.
        CHECK(counters.completed == 3);
        CHECK(counters.destroyed == 3);
    }
    CHECK(counters.destroyed == 3);
}

TEST_CASE("Parked::take() hands the work over, and the entry then frees nothing", "[ParkedWork]")
{
    auto counters = Counters {};
    auto taken = ParkedWork {};
    {
        auto executor = QueuedExecutor {};
        parkDetached(&executor, FrameSentinel { &counters }, &counters);

        auto parked = core::async::detail::Parked { executor.takeFront() };
        taken = parked.take();
        CHECK_FALSE(static_cast<bool>(parked));
    }

    // The entry died holding nothing, so the caller that took it is still the only claim.
    CHECK(counters.destroyed == 0);
    taken = ParkedWork {};
    CHECK(counters.destroyed == 1);
}

TEST_CASE("unownedRootOf answers for each shape of chain", "[ParkedWork]")
{
    auto counters = Counters {};

    // A type this module does not know -- including the type-erased handle -- says "not mine".
    CHECK_FALSE(static_cast<bool>(core::async::detail::unownedRootOf(std::noop_coroutine())));

    // A Task nobody has awaited carries nothing: the `Task` object is its owner.
    auto owned = immediate(FrameSentinel { &counters }, &counters);
    CHECK_FALSE(static_cast<bool>(core::async::detail::unownedRootOf(owned.handle())));
    CHECK_FALSE(static_cast<bool>(core::async::detail::parkedWorkFor(owned.handle()).abandon));
    CHECK(core::async::detail::parkedWorkFor(owned.handle()).resume.address() == owned.handle().address());
}
