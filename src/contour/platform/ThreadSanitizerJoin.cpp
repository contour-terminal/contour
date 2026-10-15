// SPDX-License-Identifier: Apache-2.0
//
// Lets ThreadSanitizer see Qt join its threads. Linked into both executables that load Qt, contour and
// contour_gui_test, and only when they are built with ThreadSanitizer against glibc.
//
// QThread::wait() joins with glibc's pthread_clockjoin_np, the only join libQt6Core 6.10 calls, and
// LLVM 22's ThreadSanitizer runtime intercepts pthread_join, pthread_tryjoin_np and
// pthread_timedjoin_np but not that one. To the runtime every QThread that was waited for therefore never
// ended: nothing its thread wrote is ordered before what the waiter does next, and when glibc gives the ended
// thread's descriptor to the next thread the process starts, the runtime's thread registry fails a CHECK and
// aborts the process.
//
// The definition below takes the place of glibc's for every library in the process, the dynamic
// linker binding Qt's reference to the executable first, and performs the join through
// pthread_timedjoin_np or pthread_join, which the runtime does intercept.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>

#include <pthread.h>

namespace
{

constexpr auto NanosecondsPerSecond = std::int64_t { 1'000'000'000 };

[[nodiscard]] std::int64_t nanosecondsOf(timespec const& time) noexcept
{
    return (static_cast<std::int64_t>(time.tv_sec) * NanosecondsPerSecond) + time.tv_nsec;
}

/// The time on CLOCK_REALTIME that lies as far from now as @p deadline does on @p clock.
/// @param clock The clock @p deadline is measured on.
/// @param deadline An absolute time on @p clock.
/// @return The same deadline on CLOCK_REALTIME, never before the clock's epoch.
[[nodiscard]] timespec realtimeDeadline(clockid_t clock, timespec const& deadline) noexcept
{
    auto clockNow = timespec {};
    auto realtimeNow = timespec {};
    clock_gettime(clock, &clockNow);
    clock_gettime(CLOCK_REALTIME, &realtimeNow);
    auto const remaining = nanosecondsOf(deadline) - nanosecondsOf(clockNow);
    auto const target = std::max(std::int64_t { 0 }, nanosecondsOf(realtimeNow) + remaining);
    return timespec { .tv_sec = static_cast<decltype(timespec::tv_sec)>(target / NanosecondsPerSecond),
                      .tv_nsec = static_cast<decltype(timespec::tv_nsec)>(target % NanosecondsPerSecond) };
}

} // namespace

/// Joins @p thread as glibc's pthread_clockjoin_np does, through a join ThreadSanitizer intercepts.
/// The asm label gives it the C library's name, which is how Qt's reference finds it.
/// @param thread The thread to join.
/// @param result Receives the thread's return value unless null.
/// @param clock The clock @p deadline is measured on: CLOCK_MONOTONIC or CLOCK_REALTIME.
/// @param deadline The absolute time to give up at, or null to wait for as long as it takes.
/// @return 0 once joined, ETIMEDOUT at the deadline, EINVAL for another clock, or what the join reports.
extern "C" int joinThroughInterceptedJoin(pthread_t thread,
                                          void** result,
                                          clockid_t clock,
                                          timespec const* deadline) __asm__("pthread_clockjoin_np");

extern "C" int joinThroughInterceptedJoin(pthread_t thread,
                                          void** result,
                                          clockid_t clock,
                                          timespec const* deadline)
{
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
        return EINVAL;
    if (deadline == nullptr)
        return pthread_join(thread, result);
    if (clock == CLOCK_REALTIME)
        return pthread_timedjoin_np(thread, result, deadline);
    auto const realtime = realtimeDeadline(clock, *deadline);
    return pthread_timedjoin_np(thread, result, &realtime);
}
