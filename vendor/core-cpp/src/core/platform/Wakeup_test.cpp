// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Wakeup.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#ifndef _WIN32
    #include <poll.h>
#endif

using namespace core::platform;

TEST_CASE("Wakeup.signal_and_reset", "[platform][wakeup]")
{
    auto wakeup = Wakeup {};

    // Signal should make the handle readable.
    wakeup.signal();

#ifndef _WIN32
    auto pfd = pollfd { .fd = wakeup.nativeHandle(), .events = POLLIN, .revents = 0 };
    auto const result = ::poll(&pfd, 1, 0);
    REQUIRE(result == 1);
    REQUIRE((pfd.revents & POLLIN) != 0);
#endif

    // Reset should clear the signaled state.
    wakeup.reset();

#ifndef _WIN32
    pfd.revents = 0;
    auto const result2 = ::poll(&pfd, 1, 0);
    REQUIRE(result2 == 0); // Not signaled after reset.
#endif
}

TEST_CASE("Wakeup.signal_before_wait", "[platform][wakeup]")
{
    auto wakeup = Wakeup {};

    // Signal before anyone is waiting.
    wakeup.signal();

    // The signal should persist until reset.
#ifndef _WIN32
    auto pfd = pollfd { .fd = wakeup.nativeHandle(), .events = POLLIN, .revents = 0 };
    auto const result = ::poll(&pfd, 1, 100);
    REQUIRE(result == 1);
#endif

    wakeup.reset();
}

TEST_CASE("Wakeup.multiple_signals_coalesce", "[platform][wakeup]")
{
    auto wakeup = Wakeup {};

    // Multiple signals should coalesce — a single reset clears them all.
    wakeup.signal();
    wakeup.signal();
    wakeup.signal();

    wakeup.reset();

#ifndef _WIN32
    auto pfd = pollfd { .fd = wakeup.nativeHandle(), .events = POLLIN, .revents = 0 };
    auto const result = ::poll(&pfd, 1, 0);
    REQUIRE(result == 0); // All signals cleared by single reset.
#endif
}

TEST_CASE("Wakeup.cross_thread_signal", "[platform][wakeup]")
{
    auto wakeup = Wakeup {};
    auto signaled = std::atomic<bool> { false };

    auto worker = std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Store before signalling: poll() returns as soon as signal() lands, so the reader
        // can observe the wakeup and check the flag before a later store became visible.
        signaled.store(true);
        wakeup.signal();
    });

#ifndef _WIN32
    auto pfd = pollfd { .fd = wakeup.nativeHandle(), .events = POLLIN, .revents = 0 };
    auto const result = ::poll(&pfd, 1, 2000); // Wait up to 2s.
    auto const signaledWhenWoken = signaled.load();
#endif
    // Joined before any assertion, which would otherwise leave a joinable thread to terminate().
    worker.join();

#ifndef _WIN32
    REQUIRE(result == 1);
    CHECK(signaledWhenWoken);
#endif

    wakeup.reset();
}
