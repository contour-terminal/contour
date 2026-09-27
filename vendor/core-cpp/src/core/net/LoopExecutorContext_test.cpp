// SPDX-License-Identifier: Apache-2.0
//
// `EventLoop` states itself as the current executor for the whole of a turn, so a coroutine it
// resumes -- and a strand it drives -- can find its way back to it from an awaitable that another
// thread completes (`core::async::ExecutorScope`).
#include <core/async/AsyncQueue.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Strand.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <tuple>
#include <vector>

using core::async::currentExecutor;
using core::async::ExecutorScope;
using core::async::IExecutor;
using core::async::ResumeOn;
using core::async::Strand;
using core::async::Task;
using core::net::testing::TestLoop;
using core::platform::ManualClock;

namespace
{

/// Records the current executor each time it runs, then ends.
Task<void> recordCurrent(std::vector<IExecutor*>* out)
{
    out->push_back(currentExecutor());
    co_return;
}

/// What a coroutine saw after hopping onto a strand the loop drives.
struct OnStrand
{
    bool onStrand { false };           ///< `Strand::runningHere()`.
    IExecutor* current { nullptr };    ///< The innermost executor.
    IExecutor* underneath { nullptr }; ///< The executor whose scope the strand's is nested in.
};

/// Hops onto @p strand and records where it is.
Task<void> hopOnto(Strand* strand, std::optional<OnStrand>* out)
{
    co_await ResumeOn { *strand };
    auto const* const scope = ExecutorScope::innermost();
    out->emplace(OnStrand { .onStrand = strand->runningHere(),
                            .current = currentExecutor(),
                            .underneath = scope != nullptr && scope->previous() != nullptr
                                              ? &scope->previous()->executor()
                                              : nullptr });
}

/// Takes one item and records where it came back.
Task<void> popOnce(core::async::AsyncQueue<int>* queue, std::vector<IExecutor*>* out)
{
    std::ignore = co_await queue->pop();
    out->push_back(currentExecutor());
}

} // namespace

TEST_CASE("An EventLoop turn states the loop as the current executor", "[EventLoop][context]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto seen = std::vector<IExecutor*> {};

    loop.spawn(recordCurrent(&seen));
    std::ignore = loop.drain();

    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == &loop);
    // Only while the turn runs.
    CHECK(currentExecutor() == nullptr);
}

TEST_CASE("A strand over a loop runs inside the loop's turn, with the strand current and the loop "
          "beneath it",
          "[EventLoop][Strand][context]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto seen = std::optional<OnStrand> {};
    {
        auto strand = Strand { loop };
        auto task = hopOnto(&strand, &seen);
        task.handle().resume();
        std::ignore = loop.drain();
        CHECK(task.done());
    }
    REQUIRE(seen.has_value());
    CHECK(seen->onStrand);
    CHECK(seen->underneath == &loop);
    CHECK(seen->current != nullptr);
    CHECK(seen->current != &loop);
}

TEST_CASE("A coroutine on a loop that awaits a queue built over another executor comes back to the loop",
          "[EventLoop][AsyncQueue][context]")
{
    // The behaviour change of 0.4.0, seen from the loop: the queue's own executor is where a
    // consumer that parked OUTSIDE every executor's task goes back to, and no longer where one the
    // loop was running goes.
    auto clock = ManualClock {};
    auto foreign = core::async::testing::ManualExecutor {};
    auto queue = core::async::AsyncQueue<int> { foreign, core::async::AsyncQueueOptions {} };
    auto loop = TestLoop { clock };
    auto seen = std::vector<IExecutor*> {};

    loop.spawn(popOnce(&queue, &seen));
    std::ignore = loop.drain();
    REQUIRE(queue.hasWaiter());

    std::ignore = queue.push(1);
    CHECK(foreign.pending() == 0);
    std::ignore = loop.drain();

    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == &loop);
}
