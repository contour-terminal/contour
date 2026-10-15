// SPDX-License-Identifier: Apache-2.0
//
// What a QThread writes before it ends happens before what the thread that waited for it reads next.
// Under ThreadSanitizer that holds only when the runtime sees QThread::wait() join the thread. On
// Linux the join is glibc's pthread_clockjoin_np, which ThreadSanitizer does not intercept, so the
// read below is reported as a race, and the next QThread the process starts aborts the runtime once
// glibc hands it the first one's thread descriptor. platform/ThreadSanitizerJoin.cpp is what makes the
// join visible.

#include <QtCore/QDeadlineTimer>
#include <QtCore/QThread>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <ranges>

using namespace std::chrono_literals;

TEST_CASE("a QThread's writes happen before what its waiter reads after wait()", "[threading]")
{
    // Several rounds, so that glibc reuses a descriptor; the odd ones wait with a deadline, which
    // takes the join's timed path rather than its blocking one.
    for (auto const round: std::views::iota(0, 6))
    {
        auto written = 0;
        auto const thread = std::unique_ptr<QThread>(QThread::create([&] { written = round + 1; }));
        thread->start();
        if (round % 2 == 0)
            REQUIRE(thread->wait());
        else
            REQUIRE(thread->wait(QDeadlineTimer { 30s }));
        CHECK(written == round + 1);
    }
}
