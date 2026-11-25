// SPDX-License-Identifier: Apache-2.0
//
// A thread whose stack size is chosen: it runs its body, and a stack the platform cannot give is an error,
// not an exception.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <limits>

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

TEST_CASE("ExecutorThread.unavailableStackIsAnError", "[vtextension][host]")
{
    // More address space than any platform reserves for one thread.
    auto ran = std::atomic<int> { 0 };
    auto const thread = ExecutorThread::start(std::numeric_limits<std::size_t>::max() / 2, [&] { ++ran; });
    REQUIRE(!thread.has_value());
    CHECK(thread.error() != std::error_code {});
    CHECK(ran.load() == 0);
}
