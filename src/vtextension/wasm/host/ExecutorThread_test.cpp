// SPDX-License-Identifier: Apache-2.0
//
// A thread whose stack size is chosen: it runs its body, is running by the time it is returned, and a stack
// the platform cannot give is an error, not an exception.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <limits>
#include <thread>

#include <vtextension/wasm/host/ExecutorThread.hpp>

using vtextension::wasm::host::ExecutorThread;

TEST_CASE("ExecutorThread.runsItsBody", "[vtextension][host]")
{
    auto ran = std::atomic<int> { 0 };
    {
        auto thread = ExecutorThread::start(std::size_t { 1024 } * 1024, [&] { ++ran; });
        REQUIRE(thread.has_value());
    }
    CHECK(ran.load() == 1);
}

TEST_CASE("ExecutorThread.startedThreadRunsTheBody", "[vtextension][host]")
{
    // A thread start() returned runs its body. start() returns once the thread has entered the function that
    // runs the body -- the C library's start-up done, which is what the host locks down after -- and that
    // may be just before the body's first statement: so the body is waited for, with a bound, not raced.
    auto entered = std::latch { 1 };
    auto release = std::latch { 1 };
    auto thread = ExecutorThread::start(std::size_t { 1024 } * 1024, [&] {
        entered.count_down();
        release.wait();
    });
    REQUIRE(thread.has_value());
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
    while (!entered.try_wait() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(entered.try_wait());
    release.count_down();
}

TEST_CASE("ExecutorThread.unavailableStackIsAnError", "[vtextension][host]")
{
    // More address space than any platform reserves for one thread.
    auto ran = std::atomic<int> { 0 };
    auto const thread = ExecutorThread::start(std::numeric_limits<std::size_t>::max() / 2, [&] { ++ran; });
    REQUIRE(!thread.has_value());
    CHECK(thread.error() != std::error_code {});
    CHECK(ran.load() == 0);
}
