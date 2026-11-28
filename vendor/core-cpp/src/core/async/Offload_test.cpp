// SPDX-License-Identifier: Apache-2.0
//
// async::offload: the work runs on the blocking executor and nowhere else, the caller resumes on the
// home executor and nowhere else, and the flow's stop token is read exactly once, before the hop.
// Two ManualExecutors make each of those an observable step rather than a timing.
#include <core/async/Cancellation.hpp>
#include <core/async/Offload.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using core::async::offload;
using core::async::OperationCancelled;
using core::async::StopSource;
using core::async::testing::ManualExecutor;

TEST_CASE("offload: the work runs on the blocking executor and the caller resumes on home",
          "[async][offload]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto ran = false;
    auto task = offload(&home, &blocking, [&ran] {
        ran = true;
        return 42;
    });

    task.handle().resume();
    CHECK_FALSE(ran);
    CHECK(blocking.pending() == 1);
    CHECK(home.pending() == 0);

    CHECK(blocking.drain() == 1);
    CHECK(ran);
    CHECK_FALSE(task.done());
    CHECK(home.pending() == 1);

    CHECK(home.drain() == 1);
    REQUIRE(task.done());
    CHECK(task.result() == 42);
}

TEST_CASE("offload: void work completes on home", "[async][offload]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto ran = 0;
    auto task = offload(&home, &blocking, [&ran] { ++ran; });

    task.handle().resume();
    blocking.drain();
    CHECK(ran == 1);
    CHECK_FALSE(task.done());
    home.drain();
    REQUIRE(task.done());
    task.result();
}

TEST_CASE("offload: a stop requested before the call throws and never runs the work", "[async][offload]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto source = StopSource {};
    source.request_stop();
    auto ran = false;
    auto task = offload(&home, &blocking, [&ran] {
        ran = true;
        return 1;
    });
    task.handle().promise().setStopToken(source.get_token());

    task.handle().resume();
    REQUIRE(task.done());
    CHECK(blocking.pending() == 0);
    CHECK(home.pending() == 0);
    CHECK_FALSE(ran);
    CHECK_THROWS_AS(task.result(), OperationCancelled);
}

TEST_CASE("offload: a stop requested while the work is parked still returns its result", "[async][offload]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto source = StopSource {};
    auto task = offload(&home, &blocking, [] { return std::string { "written" }; });
    task.handle().promise().setStopToken(source.get_token());

    task.handle().resume();
    REQUIRE(blocking.pending() == 1);
    source.request_stop();
    blocking.drain();
    home.drain();
    REQUIRE(task.done());
    CHECK(task.result() == "written");
}

TEST_CASE("offload: what the work throws reaches the caller on home", "[async][offload]")
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto task = offload(&home, &blocking, []() -> int { throw std::runtime_error { "refused" }; });

    task.handle().resume();
    CHECK(blocking.drain() == 1);
    CHECK_FALSE(task.done());
    CHECK(home.drain() == 1);
    REQUIRE(task.done());
    CHECK_THROWS_AS(task.result(), std::runtime_error);
}
