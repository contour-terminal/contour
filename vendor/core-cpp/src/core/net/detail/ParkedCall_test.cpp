// SPDX-License-Identifier: Apache-2.0
//
// `detail::ParkedCall<T>`: the rendezvous both resolvers park on, and Task 11's stdin read will.
// Whoever takes the waiter first decides; an answer after a stop is kept for the owner, not lost.
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/detail/ParkedCall.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <optional>
#include <utility>

using core::async::Task;
using core::net::detail::ParkedCall;
using core::net::testing::TestLoop;

namespace
{

/// How one wait on a call ended.
struct Outcome
{
    std::optional<int> answer;
    bool isCancelled = false;
    bool isFinished = false;
};

/// Waits for @p call's answer and records how the wait ended.
Task<void> waitInto(std::shared_ptr<ParkedCall<int>> call, Outcome* out)
{
    try
    {
        out->answer = co_await ParkedCall<int>::wait(std::move(call));
    }
    catch (core::async::OperationCancelled const&)
    {
        out->isCancelled = true;
    }
    out->isFinished = true;
}

} // namespace

TEST_CASE("a parked call delivers its answer through the loop", "[net][parked-call]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto call = std::make_shared<ParkedCall<int>>(&loop);

    auto outcome = Outcome {};
    loop.spawn(waitInto(call, &outcome));
    loop.drain();
    REQUIRE_FALSE(outcome.isFinished);

    call->settle(7);
    CHECK_FALSE(outcome.isFinished); // handed to the loop, not resumed by the settler
    loop.drain();
    REQUIRE(outcome.isFinished);
    CHECK(outcome.answer == 7);
    CHECK_FALSE(call->takeLateAnswer().has_value()); // delivered, so never a late one
}

TEST_CASE("an answer already there is taken without parking", "[net][parked-call]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto call = std::make_shared<ParkedCall<int>>(&loop);
    call->settle(3);

    // Stopped too: an answer that is already there is not turned into a cancellation.
    loop.rootStopSource().request_stop();
    auto outcome = Outcome {};
    loop.spawn(waitInto(call, &outcome));
    loop.drain();
    REQUIRE(outcome.isFinished);
    CHECK(outcome.answer == 3);
}

TEST_CASE("the first answer wins", "[net][parked-call]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto call = std::make_shared<ParkedCall<int>>(&loop);

    auto outcome = Outcome {};
    loop.spawn(waitInto(call, &outcome));
    loop.drain();
    call->settle(1);
    call->settle(2);
    loop.drain();
    REQUIRE(outcome.isFinished);
    CHECK(outcome.answer == 1);
}

TEST_CASE("a stop hands the flow back cancelled and keeps the late answer for its owner",
          "[net][parked-call]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto call = std::make_shared<ParkedCall<int>>(&loop);

    auto outcome = Outcome {};
    loop.spawn(waitInto(call, &outcome));
    loop.drain();
    CHECK_FALSE(call->takeLateAnswer().has_value()); // nothing answered yet

    loop.rootStopSource().request_stop();
    loop.drain();
    REQUIRE(outcome.isFinished);
    CHECK(outcome.isCancelled);
    CHECK_FALSE(call->takeLateAnswer().has_value()); // stopped, but not answered yet

    call->settle(9);
    CHECK(loop.drain() == 0); // nobody to deliver it to
    CHECK(call->takeLateAnswer() == 9);
    CHECK_FALSE(call->takeLateAnswer().has_value()); // taken once
}

TEST_CASE("a stopped wait rejoins the same call, before or after its late answer", "[net][parked-call]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto const answeredFirst = GENERATE(false, true);
    CAPTURE(answeredFirst);
    auto call = std::make_shared<ParkedCall<int>>(&loop);

    auto stopped = Outcome {};
    auto source = core::async::StopSource {};
    auto first = waitInto(call, &stopped);
    first.handle().promise().setStopToken(source.get_token());
    first.handle().resume();
    source.request_stop();
    loop.drain();
    REQUIRE(stopped.isFinished);
    CHECK(stopped.isCancelled);

    if (answeredFirst)
        call->settle(5);
    call->rejoin();
    auto joined = Outcome {};
    loop.spawn(waitInto(call, &joined));
    loop.drain();
    if (!answeredFirst)
    {
        CHECK_FALSE(joined.isFinished); // parked again, as the first waiter was
        call->settle(5);
        loop.drain();
    }
    REQUIRE(joined.isFinished);
    CHECK_FALSE(joined.isCancelled);
    CHECK(joined.answer == 5);
}

TEST_CASE("rejoining a call no stop ended changes nothing", "[net][parked-call]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto call = std::make_shared<ParkedCall<int>>(&loop);
    call->settle(3);
    call->rejoin();
    CHECK_FALSE(call->takeLateAnswer().has_value()); // answered, not late
    auto outcome = Outcome {};
    loop.spawn(waitInto(call, &outcome));
    loop.drain();
    REQUIRE(outcome.isFinished);
    CHECK(outcome.answer == 3);
}
