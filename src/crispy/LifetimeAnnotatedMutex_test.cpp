// SPDX-License-Identifier: Apache-2.0
#include <crispy/LifetimeAnnotatedMutex.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

using crispy::LifetimeAnnotatedMutex;

namespace
{

/// Builds a mutex in each slot, takes @p outer's and, holding it, @p inner's, then destroys both.
/// @param outer The slot whose mutex is taken first.
/// @param inner The slot whose mutex is taken while the first is held.
void takeNestedInNewMutexes(std::optional<LifetimeAnnotatedMutex>& outer,
                            std::optional<LifetimeAnnotatedMutex>& inner)
{
    outer.emplace();
    inner.emplace();
    {
        auto const outerHold = std::scoped_lock { *outer };
        auto const innerHold = std::scoped_lock { *inner };
    }
    for (auto* const slot: { &outer, &inner })
    {
        CHECK((*slot)->try_lock()); // released with its guard
        (*slot)->unlock();
        slot->reset();
    }
}

} // namespace

TEST_CASE("LifetimeAnnotatedMutex.aNewMutexAtADeadOnesAddressInheritsNoLockOrder",
          "[LifetimeAnnotatedMutex][threading]")
{
    // ThreadSanitizer's deadlock detector knows a mutex by its address. Two mutexes are taken in one order
    // and destroyed; two new ones, built in the same storage, are then taken in the other order. No mutex is
    // ever taken in both orders, so there is no cycle -- unless the detector still holds the order of the
    // dead ones, as it does for a plain std::mutex, whose end it never hears of. It then reports a
    // lock-order inversion, and under the clang-tsan tree this process fails. Anywhere, each mutex is free
    // again once its guard is gone.
    auto first = std::optional<LifetimeAnnotatedMutex> {};
    auto second = std::optional<LifetimeAnnotatedMutex> {};
    takeNestedInNewMutexes(first, second);
    takeNestedInNewMutexes(second, first);
}

TEST_CASE("LifetimeAnnotatedMutex.waitsOnAStdConditionVariable", "[LifetimeAnnotatedMutex]")
{
    // It is a std::mutex, so a std::condition_variable waits on it -- through a lock that names
    // std::mutex, as the condition variable takes no other.
    auto mutex = LifetimeAnnotatedMutex {};
    auto condition = std::condition_variable {};
    auto lock = std::unique_lock<std::mutex> { mutex };
    CHECK(condition.wait_for(lock, std::chrono::milliseconds { 0 }) == std::cv_status::timeout);
}
