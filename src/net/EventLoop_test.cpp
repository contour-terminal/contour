// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <ranges>
#include <thread>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/WaitChunking.h>
#include <net/WithTimeout.h>
#include <net/platform/Clock.h>
#include <net/platform/SystemPipe.h>
#include <net/testing/ScriptedEventSource.h>

using coro::OperationCancelled;
using coro::Task;
using net::EventLoop;
using net::ManualClock;
using net::testing::ScriptedEventSource;

// Note on scripted fd tokens: the EventLoop constructor attaches its post()
// self-pipe to the source first, so it owns FdToken{1} and one attachedCount
// slot; the first coroutine fd waiter therefore receives FdToken{2}.

namespace
{

/// Resumes immediately when the delay has already elapsed (the ready path).
Task<int> awaitZeroDelay(EventLoop* loop)
{
    co_await loop->delay(std::chrono::milliseconds { 0 });
    co_return 7;
}

/// Parks on a delay of @p delayMs, then sets *fired and returns it. Used with a
/// ManualClock to prove the timer fires only once the clock crosses the deadline.
Task<int> awaitDelayThenFire(EventLoop* loop, int delayMs, bool* fired)
{
    co_await loop->delay(std::chrono::milliseconds { delayMs });
    *fired = true;
    co_return delayMs;
}

/// Waits for @p fd to become readable; returns 1 on readiness or @p cancelSentinel
/// if cancelled while parked.
Task<int> awaitReadableOrCancel(EventLoop* loop, net::NativeHandle fd, int cancelSentinel)
{
    try
    {
        co_await loop->waitReadable(fd);
        co_return 1;
    }
    catch (OperationCancelled const&)
    {
        co_return cancelSentinel;
    }
}

/// A value-producing task that completes synchronously — the "work wins" arm.
Task<int> produceValue(int value)
{
    co_return value;
}

/// Parks on a never-ready fd forever (until cancelled) — the "timeout wins" arm.
Task<int> parkOnFdForever(EventLoop* loop, net::NativeHandle fd)
{
    co_await loop->waitReadable(fd);
    co_return 0;
}

/// A background flow with an observable completion, for the spawn-reap test.
Task<void> incrementAndFinish(int* counter)
{
    ++*counter;
    co_return;
}

/// Sets *destroyed = true when its frame unwinds (RAII), so a test can prove a
/// parked flow was cancelled-and-unwound rather than raw-destroyed.
struct UnwindFlag
{
    bool* destroyed;

    ~UnwindFlag() { *destroyed = true; }
};

/// Parks on waitReadable with an RAII guard; proves the frame unwinds (guard runs)
/// if the loop is torn down while the fd wait is parked.
Task<void> waitReadableWithGuard(EventLoop* loop, net::NativeHandle fd, bool* destroyed)
{
    *destroyed = false;
    auto guard = UnwindFlag { destroyed };
    try
    {
        co_await loop->waitReadable(fd);
    }
    catch (OperationCancelled const&)
    {
        // Expected on loop teardown: the frame unwinds and `guard` destructs,
        // which is exactly what this flow exists to demonstrate.
        static_cast<void>(destroyed);
    }
}

/// A ScriptedEventSource that advances an injected ManualClock by a fixed step on
/// every wait(). This models the passage of time deterministically: the loop
/// schedules a delay against the clock, and each blocking wait "elapses" exactly
/// `step` of clock time, so a delay fires after a known number of waits — with no
/// real sleeping.
class ClockAdvancingSource: public ScriptedEventSource
{
  public:
    ClockAdvancingSource(ManualClock& clock, std::chrono::milliseconds step) noexcept:
        _clock(clock), _step(step)
    {
    }

    net::WaitOutcome wait(int timeoutMs) override
    {
        _clock.advance(_step);
        return ScriptedEventSource::wait(timeoutMs);
    }

  private:
    ManualClock& _clock;
    std::chrono::milliseconds _step;
};

} // namespace

TEST_CASE("delay(0) resumes without waiting", "[EventLoop]")
{
    auto source = ScriptedEventSource {};
    auto loop = EventLoop { source };

    auto const result = loop.blockOn(awaitZeroDelay(&loop));

    REQUIRE(result == 7);
    REQUIRE(source.waitCount() == 0); // ready path never blocks
}

TEST_CASE("A pending delay bounds the wait timeout and fires deterministically", "[EventLoop][clock]")
{
    // With time frozen except for the scripted 250ms step per wait, a delay(500)
    // must bound the FIRST wait to exactly 500ms (not block indefinitely at -1)
    // and fire on the second wait, once the clock has crossed the deadline.
    auto clock = ManualClock {};
    auto source = ClockAdvancingSource { clock, std::chrono::milliseconds { 250 } };
    source.pushTimeout(); // 250ms elapsed: still pending
    source.pushTimeout(); // 500ms elapsed: timer is now due -> flow resumes
    auto loop = EventLoop { source, clock };

    auto fired = false;
    auto const result = loop.blockOn(awaitDelayThenFire(&loop, 500, &fired));

    REQUIRE(fired);
    REQUIRE(result == 500);
    REQUIRE(source.waitCount() == 2);
    REQUIRE(source.recordedTimeouts().front() == 500); // exact: no real-clock jitter
    REQUIRE(source.recordedTimeouts().back() == 250);  // the remaining half
}

TEST_CASE("EventSource fd registry hands out distinct tokens and reports readiness", "[EventSource]")
{
    // The scripted source ignores the handle value (it returns synthetic tokens);
    // a real SystemPipe supplies portable valid handles (NativeHandle is void* on
    // Windows, so integer literals would not compile).
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedEventSource {};
    auto const a = source.attach((*pipe)->readFd(), net::FdInterest::Read);
    auto const b = source.attach((*pipe)->writeFd(), net::FdInterest::Write);

    REQUIRE(static_cast<bool>(a));
    REQUIRE(static_cast<bool>(b));
    REQUIRE_FALSE(a == b);
    REQUIRE(source.attachedCount() == 2);

    source.pushReadable(a);
    source.pushWritable(b);

    auto const first = source.wait(0);
    REQUIRE(first.readyRead.size() == 1);
    REQUIRE(first.readyRead.front() == a);

    auto const second = source.wait(0);
    REQUIRE(second.readyWrite.size() == 1);
    REQUIRE(second.readyWrite.front() == b);

    source.detach(a);
    source.detach(b);
    REQUIRE(source.attachedCount() == 0);
}

TEST_CASE("An invalid FdToken is falsy and equals the invalid sentinel", "[EventSource]")
{
    auto const invalid = net::FdToken::invalid();
    REQUIRE_FALSE(static_cast<bool>(invalid));
    REQUIRE(invalid == net::FdToken {});
}

TEST_CASE("waitReadable resumes when the registered fd becomes readable", "[EventLoop][fd]")
{
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedEventSource {};
    // The awaiter attaches the fd during await_suspend; the loop's self-pipe holds
    // token 1, so the waiter receives token 2 — script that one readable.
    source.pushReadable(net::FdToken { 2 });
    auto loop = EventLoop { source };

    constexpr auto Cancelled = -1;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, (*pipe)->readFd(), Cancelled));

    REQUIRE(result == 1);
}

TEST_CASE("waitReadable on an invalid fd resolves immediately as cancelled", "[EventLoop][fd]")
{
    auto source = ScriptedEventSource {};
    auto loop = EventLoop { source };

    constexpr auto Cancelled = -3;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, net::InvalidHandle, Cancelled));

    REQUIRE(result == Cancelled);
    REQUIRE(source.waitCount() == 0); // never blocked: an unwaitable fd resolves inline
}

TEST_CASE("waitReadable resolves over a real SystemPipe via PollEventSource", "[EventLoop][fd][poll]")
{
    // End-to-end through the real OS readiness path (poll(2) / WaitForMultipleObjects),
    // not the scripted source: a SystemPipe whose write end already holds a byte is
    // readable, so a flow parked on waitReadable resolves on the first real wait and
    // reads the byte back.
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    char const payload = 'Z';
    REQUIRE((*pipe)->write(&payload, 1).has_value());

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto readByte = [](EventLoop* l, net::SystemPipe* p) -> Task<char> {
        co_await l->waitReadable(p->waitHandle());
        char buf = 0;
        auto const got = p->read(&buf, 1);
        co_return (got.has_value() && *got == 1) ? buf : '\0';
    };

    auto const result = loop.blockOn(readByte(&loop, pipe->get()));
    REQUIRE(result == 'Z');
    REQUIRE(source.attachedCount() == 1); // only the loop's self-pipe remains attached
}

TEST_CASE("post() wakes a blocked wait and runs its callback on the loop thread", "[EventLoop][post]")
{
    // The root flow parks on a pipe that never receives data, so poll(2) blocks
    // indefinitely (-1): ONLY the post self-pipe can wake it. A second thread posts
    // a callback that feeds the pipe; the flow completing at all proves the
    // cross-thread wakeup, and the recorded thread id proves the callback ran on
    // the loop thread, not the poster's.
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto const loopThread = std::this_thread::get_id();
    auto callbackThread = std::thread::id {};

    auto poster = std::thread { [&] {
        loop.post([&] {
            callbackThread = std::this_thread::get_id();
            char const byte = 'x';
            std::ignore = (*pipe)->write(&byte, 1);
        });
    } };

    constexpr auto Cancelled = -1;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, (*pipe)->waitHandle(), Cancelled));
    poster.join();

    REQUIRE(result == 1);
    REQUIRE(callbackThread == loopThread);
}

TEST_CASE("requestStop() posted from another thread cancels a parked flow", "[EventLoop][post]")
{
    // The daemon's shutdown path: a signal handler thread posts requestStop(); the
    // parked waitReadable unwinds via OperationCancelled instead of waiting for an
    // fd that will never become ready.
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto poster = std::thread { [&] { loop.post([&] { loop.requestStop(); }); } };

    constexpr auto Cancelled = -7;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, (*pipe)->waitHandle(), Cancelled));
    poster.join();

    REQUIRE(result == Cancelled);
    REQUIRE(source.attachedCount() == 1); // the cancelled waiter detached; self-pipe remains
}

TEST_CASE("Finished spawned flows are reaped on the next pump", "[EventLoop][spawn]")
{
    // Upstream Endo held every spawned frame until destruction — an unbounded leak
    // for a long-lived loop spawning per-connection flows. The reap runs at the top
    // of every pump, so frames finished during one blockOn are reclaimed by the
    // first pump of the next.
    auto source = ScriptedEventSource {};
    auto loop = EventLoop { source };

    auto counter = 0;
    loop.spawn(incrementAndFinish(&counter));
    loop.spawn(incrementAndFinish(&counter));
    REQUIRE(loop.spawnedCount() == 2);

    loop.blockOn(awaitZeroDelay(&loop));
    REQUIRE(counter == 2); // both flows ran to completion...
    // ...but were not yet reaped: the reap preceding their completion already ran.
    REQUIRE(loop.spawnedCount() == 2);

    loop.blockOn(awaitZeroDelay(&loop));
    REQUIRE(loop.spawnedCount() == 0); // the next pump's reap reclaimed the frames
}

TEST_CASE("withTimeout returns the work's value when it finishes first", "[EventLoop][timeout]")
{
    auto source = ScriptedEventSource {};
    auto loop = EventLoop { source };

    // The work completes synchronously, so the timeout arm never matters.
    auto result = loop.blockOn(net::withTimeout(&loop, produceValue(42), std::chrono::seconds { 10 }));

    REQUIRE(result.has_value());
    REQUIRE(*result == 42);
}

TEST_CASE("withTimeout returns nullopt and cancels the work when the deadline fires",
          "[EventLoop][timeout][poll]")
{
    // The work parks on a SystemPipe that never receives data, so only the timeout
    // arm can win. When it does, whenAny requests stop; the parked waitReadable is
    // re-queued via the stop-callback (requeueForCancellation) and unwinds, so the
    // work is genuinely cancelled rather than leaking. A short real timeout drives
    // the loop's timer.
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto result = loop.blockOn(net::withTimeout(
        &loop, parkOnFdForever(&loop, (*pipe)->waitHandle()), std::chrono::milliseconds { 20 }));

    REQUIRE_FALSE(result.has_value());    // the timeout won
    REQUIRE(source.attachedCount() == 1); // the cancelled work detached its fd; self-pipe remains
}

TEST_CASE("Destroying the loop unwinds a flow parked on waitReadable", "[EventLoop][fd]")
{
    auto pipe = net::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedEventSource {};
    source.pushTimeout(); // benign wait for the root's post-completion pump
    auto destroyed = false;
    {
        auto loop = EventLoop { source };
        loop.spawn(waitReadableWithGuard(&loop, (*pipe)->readFd(), &destroyed));
        loop.blockOn(awaitZeroDelay(&loop)); // drive the spawned flow to its park
        REQUIRE_FALSE(destroyed);
    } // ~EventLoop: stop + flush fd waiters + drain -> the frame unwinds, guard runs

    REQUIRE(destroyed);
}

// The pure chunking / rotation math that PollEventSource's Windows path uses to
// wait on more than MAXIMUM_WAIT_OBJECTS handles. Platform-neutral (no windows.h)
// so it is exercised here on every platform, including this Linux CI.
TEST_CASE("WaitChunking splits a handle set into wait-sized chunks", "[WaitChunking]")
{
    constexpr std::size_t MaxChunk = 64; // MAXIMUM_WAIT_OBJECTS on Windows.

    SECTION("chunk count is the ceiling of the total over the max chunk size")
    {
        CHECK(net::waitChunkCount(0, MaxChunk) == 0);
        CHECK(net::waitChunkCount(1, MaxChunk) == 1);
        CHECK(net::waitChunkCount(64, MaxChunk) == 1);
        CHECK(net::waitChunkCount(65, MaxChunk) == 2);
        CHECK(net::waitChunkCount(128, MaxChunk) == 2);
        CHECK(net::waitChunkCount(129, MaxChunk) == 3);
        CHECK(net::waitChunkCount(200, MaxChunk) == 4);
    }

    SECTION("the boundary case just past one wait yields a full chunk and a remainder")
    {
        REQUIRE(net::waitChunkCount(65, MaxChunk) == 2);
        CHECK(net::waitChunkAt(65, MaxChunk, 0) == net::WaitChunk { .offset = 0, .count = 64 });
        CHECK(net::waitChunkAt(65, MaxChunk, 1) == net::WaitChunk { .offset = 64, .count = 1 });
    }

    SECTION("chunks tile the handle array with no gaps, overlaps, or oversized spans")
    {
        constexpr std::size_t Total = 200;
        auto const chunks = net::waitChunkCount(Total, MaxChunk);
        REQUIRE(chunks == 4);

        auto expectedOffset = std::size_t { 0 };
        for (auto const i: std::views::iota(std::size_t { 0 }, chunks))
        {
            auto const chunk = net::waitChunkAt(Total, MaxChunk, i);
            CHECK(chunk.offset == expectedOffset); // contiguous: no gap and no overlap
            CHECK(chunk.count >= 1);
            CHECK(chunk.count <= MaxChunk); // never larger than one wait accepts
            expectedOffset += chunk.count;
        }
        CHECK(expectedOffset == Total); // every handle covered exactly once
    }
}

TEST_CASE("WaitChunking rotates the start chunk fairly and maps indices back", "[WaitChunking]")
{
    SECTION("rotation advances by one and wraps at the chunk count")
    {
        CHECK(net::nextWaitRotation(4, 0) == 1);
        CHECK(net::nextWaitRotation(4, 1) == 2);
        CHECK(net::nextWaitRotation(4, 2) == 3);
        CHECK(net::nextWaitRotation(4, 3) == 0); // wrap
    }

    SECTION("a stale cursor past the chunk count is folded back into range")
    {
        CHECK(net::nextWaitRotation(4, 10) == 3); // 10 % 4 == 2, then +1
        CHECK(net::nextWaitRotation(4, 7) == 0);  // 7 % 4 == 3, then wraps
        CHECK(net::nextWaitRotation(1, 0) == 0);  // a single chunk always stays put
    }
}
