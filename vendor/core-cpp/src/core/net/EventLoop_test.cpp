// SPDX-License-Identifier: Apache-2.0
#include <core/async/AsyncQueue.hpp>
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/SystemPipe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <expected>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using core::async::OperationCancelled;
using core::async::Task;
using core::net::EventLoop;
using core::net::testing::HandlerId;
using core::net::testing::ScriptedBackend;
using core::platform::ManualClock;

// fastcached#1546: MSVC 19.44's ARM64 code generator drops the enclosing `try` of a `co_await` on
// a temporary awaiter whose `await_ready` makes a call, so an `OperationCancelled` from
// `await_resume` passes every handler. An `await_ready` here answers a constant and the decision
// is `await_suspend`'s (.agent/rules/async-and-net.md); a call put back fails to compile wherever
// the question can be asked at compile time (core::async::awaitReadyIsConstantFalse).
static_assert(core::async::awaitReadyIsConstantFalse<core::net::DelayAwaiter>());

// Note on scripted registration ids: the loop no longer attaches a wakeup channel of
// its own — that belongs to the backend now, and ScriptedBackend has none — so the
// first coroutine fd waiter receives HandlerId{1}.

namespace
{

/// @param timeout A timeout a backend recorded.
/// @return The milliseconds it names, or -1 for an indefinite wait. Cases assert on
///         milliseconds because that is the unit the deadlines in them are written in;
///         the backend takes a duration so that the rounding to whatever its native
///         wait accepts happens once, in the backend, and not in every caller.
[[nodiscard]] long long timeoutMs(std::optional<core::platform::SteadyDuration> const& timeout)
{
    return timeout.has_value() ? std::chrono::duration_cast<std::chrono::milliseconds>(*timeout).count() : -1;
}

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

/// Sets *flag true after @p delayMs — a stand-in for the async condition
/// (queue drained, debounce fired) that pollUntil waits on.
Task<void> setFlagAfter(EventLoop* loop, int delayMs, bool* flag)
{
    co_await loop->delay(std::chrono::milliseconds { delayMs });
    *flag = true;
}

/// Polls until @p flag is set, then reports how it exited (true) plus the number
/// of poll iterations it took (via @p polls).
Task<bool> pollForFlag(EventLoop* loop, bool* flag, int* polls)
{
    co_await core::net::pollUntil(loop, [flag, polls] {
        ++*polls;
        return *flag;
    });
    co_return *flag;
}

/// Waits for @p fd to become readable; returns 1 on readiness or @p cancelSentinel
/// if cancelled while parked.
Task<int> awaitReadableOrCancel(EventLoop* loop, core::platform::NativeHandle fd, int cancelSentinel)
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

/// Waits for @p fd to become readable, distinguishing a refused registration from a
/// cancellation: the first is a plumbing failure the caller can report, the second is a
/// deliberate stop, and a flow that cannot tell them apart logs the wrong one.
Task<int> awaitReadableOrRefusal(EventLoop* loop, core::platform::NativeHandle fd)
{
    try
    {
        co_await loop->waitReadable(fd);
        co_return 1;
    }
    catch (core::net::FdRegistrationFailed const&)
    {
        co_return -2;
    }
    catch (OperationCancelled const&)
    {
        co_return -1;
    }
}

/// A value-producing task that completes synchronously — the "work wins" arm.
Task<int> produceValue(int value)
{
    co_return value;
}

/// Parks on a never-ready fd forever (until cancelled) — the "timeout wins" arm.
Task<int> parkOnFdForever(EventLoop* loop, core::platform::NativeHandle fd)
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

/// A root flow that completes at once, so `blockOn` pumps exactly as far as the
/// spawned flows need and no further.
Task<void> justReturn()
{
    co_return;
}

/// Wraps a real backend and records the timeout each `wait()` was ASKED for.
///
/// The property under test is what `blockOn` REQUESTS, which is a fact about the argument rather
/// than about what the backend does with it. Neither double in the tree can answer it: a
/// `ScriptedBackend` never really waits, so `blockOn` burns a script step per turn and the run
/// ends in "script exhausted"; and `runOnce` records nothing at all, because it never asks. So a
/// real backend, whose wait genuinely blocks on its wake channel, with the argument captured on
/// the way through.
///
/// Thread safety: `wait()` is called only by the loop thread, and `wake()` — the one member
/// another thread reaches — touches nothing recorded here. The vector is read after `blockOn`
/// has returned, on the thread that ran the loop, so nothing is appending by then.
class RecordingBackend final: public core::net::IoBackend
{
  public:
    /// @param inner The backend to delegate to; must outlive this.
    explicit RecordingBackend(core::net::IoBackend& inner) noexcept: _inner(inner) {}

    [[nodiscard]] core::net::BackendKind kind() const noexcept override { return _inner.kind(); }

    [[nodiscard]] std::expected<void, core::net::NetError> attach(
        core::net::ReadinessHandler& handler) override
    {
        return _inner.attach(handler);
    }

    [[nodiscard]] std::expected<void, core::net::NetError> setInterest(core::net::ReadinessHandler& handler,
                                                                       core::net::Interest interest) override
    {
        return _inner.setInterest(handler, interest);
    }

    void detach(core::net::ReadinessHandler& handler) noexcept override { _inner.detach(handler); }

    [[nodiscard]] core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        _timeouts.push_back(timeout);
        return _inner.wait(timeout);
    }

    void wake() noexcept override { _inner.wake(); }

    [[nodiscard]] bool isHostDriven() const noexcept override { return _inner.isHostDriven(); }

    void armWakeAt(std::optional<core::platform::SteadyTimePoint> deadline) noexcept override
    {
        _inner.armWakeAt(deadline);
    }

    void setPump(core::net::HostCallback pump, void* state) noexcept override { _inner.setPump(pump, state); }

    /// @return Every timeout `wait()` was given, in order. Read only after the drive has returned.
    [[nodiscard]] std::vector<std::optional<core::platform::SteadyDuration>> const& timeouts() const noexcept
    {
        return _timeouts;
    }

  private:
    core::net::IoBackend& _inner;
    std::vector<std::optional<core::platform::SteadyDuration>> _timeouts;
};

/// Hands itself to @p loop and records that it got there.
///
/// Resumed by hand rather than spawned, so the `submit` happens BETWEEN turns and lands in the
/// inbound queue — the same place a `ResumeOn` hand-off from a pool thread sits.
/// @param loop The loop to resume on; never null.
/// @param resumed Set once the hand-off has been honoured.
Task<void> handOverToLoop(EventLoop* loop, bool* resumed)
{
    co_await core::async::ResumeOn { *loop };
    *resumed = true;
}

/// Hops to a pool, works there, and hops back to the loop.
///
/// The shape `ResumeOn` exists for, and the one where the loop is momentarily empty while the
/// flow is very much alive: between the two `co_await`s nothing is queued, nothing is parked and
/// nothing is inbound, because the flow is running on somebody else's thread.
/// @param loop The loop to come back to; never null.
/// @param pool The executor to hop onto; never null.
Task<int> hopToPoolAndBack(EventLoop* loop, core::async::IExecutor* pool)
{
    co_await core::async::ResumeOn { *pool };
    std::this_thread::sleep_for(std::chrono::milliseconds { 40 });
    co_await core::async::ResumeOn { *loop };
    co_return 7;
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
Task<void> waitReadableWithGuard(EventLoop* loop, core::platform::NativeHandle fd, bool* destroyed)
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

/// A ScriptedBackend that advances an injected ManualClock by a fixed step on every
/// wait(). This models the passage of time deterministically: the loop schedules a
/// delay against the clock, and each blocking wait "elapses" exactly `step` of clock
/// time, so a delay fires after a known number of waits — with no real sleeping.
class ClockAdvancingBackend: public ScriptedBackend
{
  public:
    ClockAdvancingBackend(ManualClock& clock, std::chrono::milliseconds step) noexcept:
        _clock(clock), _step(step)
    {
    }

    core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        _clock.advance(_step);
        return ScriptedBackend::wait(timeout);
    }

  private:
    ManualClock& _clock;
    std::chrono::milliseconds _step;
};

} // namespace

TEST_CASE("delay(0) resumes without waiting", "[EventLoop]")
{
    auto source = ScriptedBackend {};
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
    auto source = ClockAdvancingBackend { clock, std::chrono::milliseconds { 250 } };
    source.pushTimeout(); // 250ms elapsed: still pending
    source.pushTimeout(); // 500ms elapsed: timer is now due -> flow resumes
    auto loop = EventLoop { source, clock };

    auto fired = false;
    auto const result = loop.blockOn(awaitDelayThenFire(&loop, 500, &fired));

    REQUIRE(fired);
    REQUIRE(result == 500);
    REQUIRE(source.waitCount() == 2);
    REQUIRE(timeoutMs(source.recordedTimeouts().front()) == 500); // exact: no real-clock jitter
    REQUIRE(timeoutMs(source.recordedTimeouts().back()) == 250);  // the remaining half
}

// The two `clock.refresh()` calls the turn makes are asserted in `ClockRefresh_test.cpp`, one
// case per call: a single case here passed with either of them removed, which is not a test of
// them.

TEST_CASE("pollUntil returns as soon as its predicate holds", "[EventLoop][poll]")
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto flag = false;
    auto polls = 0;
    loop.spawn(setFlagAfter(&loop, 5, &flag)); // flips true a few poll ticks in
    auto const done = loop.blockOn(pollForFlag(&loop, &flag, &polls));

    CHECK(done);
    CHECK(polls >= 2); // checked at least once before and once after the flag flipped
}

TEST_CASE("pollUntil returns immediately when the predicate already holds", "[EventLoop][poll]")
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto flag = true; // already satisfied: no delay should be awaited
    auto polls = 0;
    auto const done = loop.blockOn(pollForFlag(&loop, &flag, &polls));

    CHECK(done);
    CHECK(polls == 1); // one check, then a prompt return
}

namespace
{

/// Counts a dispatch into the `int` its handler's owner points at.
void countDispatch(core::net::ReadinessHandler& handler) noexcept
{
    ++*static_cast<int*>(handler.owner);
}

} // namespace

TEST_CASE("the scripted backend dispatches to the registration its script names", "[net][backend]")
{
    // The scripted backend ignores the handle value (it names registrations by attach
    // order); a real SystemPipe supplies portable valid handles, because NativeHandle
    // is void* on Windows and integer literals would not compile.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto reader = 0;
    auto writer = 0;
    auto readHandler = core::net::ReadinessHandler { .handle = (*pipe)->readFd(),
                                                     .owner = &reader,
                                                     .onReadable = &countDispatch };
    auto writeHandler = core::net::ReadinessHandler { .handle = (*pipe)->writeFd(),
                                                      .owner = &writer,
                                                      .onWritable = &countDispatch };

    auto source = ScriptedBackend {};
    REQUIRE(source.attach(readHandler).has_value());
    auto const a = source.lastHandlerId();
    REQUIRE(source.attach(writeHandler).has_value());
    auto const b = source.lastHandlerId();

    REQUIRE(static_cast<bool>(a));
    REQUIRE(static_cast<bool>(b));
    REQUIRE(a != b);
    REQUIRE(source.attachedCount() == 2);

    REQUIRE(source.setInterest(readHandler, core::net::Interest::Read).has_value());
    REQUIRE(source.setInterest(writeHandler, core::net::Interest::Write).has_value());
    CHECK(source.interestOf(a) == core::net::Interest::Read);
    CHECK(source.interestOf(b) == core::net::Interest::Write);

    source.pushReadable(a);
    source.pushWritable(b);

    CHECK(source.wait(core::platform::SteadyDuration::zero()).dispatched == 1);
    CHECK(reader == 1);
    CHECK(writer == 0);

    CHECK(source.wait(core::platform::SteadyDuration::zero()).dispatched == 1);
    CHECK(writer == 1);
    CHECK(reader == 1);

    source.detach(readHandler);
    source.detach(writeHandler);
    REQUIRE(source.attachedCount() == 0);
}

TEST_CASE("the scripted backend detaches idempotently, like every real one", "[net][backend]")
{
    // IoBackend::detach is documented idempotent, and the loop really does detach twice
    // on normal paths — notifyHandleClosing then unregisterFdWaiter;
    // requeueForCancellation and wakeAllWaiters before await_resume. The scripted
    // double once counted DETACH CALLS instead of live registrations, so a second
    // detach of one registration cancelled out a different one: attachedCount() then
    // under-reported, and a leak assertion against it would pass on a registration that
    // never went away.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto a = core::net::ReadinessHandler { .handle = (*pipe)->readFd() };
    auto b = core::net::ReadinessHandler { .handle = (*pipe)->writeFd() };
    auto stranger = core::net::ReadinessHandler { .handle = (*pipe)->readFd() };

    auto source = ScriptedBackend {};
    REQUIRE(source.attach(a).has_value());
    REQUIRE(source.attach(b).has_value());
    REQUIRE(source.attachedCount() == 2);

    source.detach(a);
    source.detach(a); // the second detach of the SAME handler must change nothing
    CHECK(source.attachedCount() == 1);

    source.detach(stranger); // one that was never attached is a no-op too
    CHECK(source.attachedCount() == 1);

    source.detach(b);
    CHECK(source.attachedCount() == 0);
}

TEST_CASE("waitReadable resumes when the registered fd becomes readable", "[EventLoop][fd]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedBackend {};
    // The awaiter attaches the fd during await_suspend, and it is the loop's first
    // registration — the wakeup channel belongs to the backend now, and this one has
    // none — so the waiter receives HandlerId{1}. Script that one readable.
    source.pushReadable(HandlerId { 1 });
    auto loop = EventLoop { source };

    constexpr auto Cancelled = -1;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, (*pipe)->readFd(), Cancelled));

    REQUIRE(result == 1);
}

TEST_CASE("notifyHandleClosing on an unwatched fd records nothing", "[EventLoop][fd][closehang]")
{
    // Closing a descriptor nobody is parked on must not schedule any wake. If it did,
    // the next pump would deliver a ParkId naming a park that no longer exists —
    // harmless today only because queueParkedWaiter skips an unknown one, but an id
    // can be reused, and then the wake would land on an unrelated flow.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedBackend {};
    auto loop = EventLoop { source };

    loop.notifyHandleClosing((*pipe)->readFd(), core::net::FdWakePolicy::Resume);
    // And an invalid handle is refused outright rather than looked up: on Windows a
    // NativeHandle is a pointer, so a null one would otherwise be a perfectly good
    // multimap key that any other unset handle could collide with.
    loop.notifyHandleClosing(core::platform::InvalidHandle, core::net::FdWakePolicy::Cancel);

    // Nothing parked and nothing recorded, so this pump neither waits nor resumes.
    // A recorded wake would have turned the wait into a poll; an exhausted script
    // would have thrown had one been attempted.
    auto counter = 0;
    loop.blockOn(incrementAndFinish(&counter));
    REQUIRE(counter == 1);
    REQUIRE(source.waitCount() == 0);
}

TEST_CASE("notifyHandleClosing detaches the registration while the fd is still valid",
          "[EventLoop][fd][closehang]")
{
    // The kernel registration has to be dropped at CLOSE time, not left for the
    // awaiter's own detach. By then the descriptor number may have been reassigned
    // to a new socket, and epoll_ctl(EPOLL_CTL_DEL) / kqueue's delete-by-descriptor
    // would unregister THAT one instead. attachedCount is the observable proof.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes the still-
    // parked flow, whose RAII guard writes here as it unwinds.
    auto destroyed = false;

    auto source = ScriptedBackend {};
    source.pushTimeout(); // the park's first wait reports nothing
    auto loop = EventLoop { source };

    loop.spawn(waitReadableWithGuard(&loop, (*pipe)->readFd(), &destroyed));
    loop.blockOn(justReturn()); // let the spawned flow reach its park

    REQUIRE(source.attachedCount() == 1); // the parked waiter's registration
    REQUIRE(loop.parkedWaiterCount() == 1);

    loop.notifyHandleClosing((*pipe)->readFd(), core::net::FdWakePolicy::Resume);
    REQUIRE(source.attachedCount() == 0); // detached immediately, not at resume
    // ... and the park itself stays, because requestStop() and ~EventLoop must still
    // find this waiter if either runs before the next pump.
    REQUIRE(loop.parkedWaiterCount() == 1);
}

TEST_CASE("a recorded close is delivered without blocking the pump", "[EventLoop][fd][closehang]")
{
    // A closed descriptor can no longer produce readiness, so the pump must not
    // block waiting for it. It still performs its wait — merged, not skipped, so
    // nothing else ready in the same instant is starved — but with a zero timeout.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    // Declared BEFORE the loop, so it outlives it (see the case above).
    auto destroyed = false;

    auto source = ScriptedBackend {};
    source.pushTimeout(); // the park's wait
    source.pushTimeout(); // the close-wake pump's wait, which must be a poll
    auto loop = EventLoop { source };

    loop.spawn(waitReadableWithGuard(&loop, (*pipe)->readFd(), &destroyed));
    // Driven with runOnce rather than blockOn: blockOn exists to finish one flow and stops the
    // moment that flow is finished, so a trivial root no longer makes the loop perform a turn's
    // wait at all -- which is the point of it, and useless for a case about that wait.
    std::ignore = loop.runOnce();
    auto const waitsBeforeClose = source.waitCount();

    loop.notifyHandleClosing((*pipe)->readFd(), core::net::FdWakePolicy::Resume);
    std::ignore = loop.runOnce();
    std::ignore = loop.runOnce(); // step 2 of the next turn is where the resumption happens

    REQUIRE(source.waitCount() == waitsBeforeClose + 1);
    // Zero, not -1: an indefinite wait would never return on the closed fd's account.
    REQUIRE(timeoutMs(source.recordedTimeouts().back()) == 0);
    REQUIRE(destroyed); // the parked flow resumed and unwound
}

TEST_CASE("a registration the backend refuses fails the await rather than parking it", "[EventLoop][fd]")
{
    // The whole reason IoBackend::attach answers an expected. A flow parked on a
    // registration the backend never made has nothing left to resume it: no message,
    // no stack, just a hang. So the awaiter resumes at once and throws
    // FdRegistrationFailed, which is distinct from OperationCancelled because a
    // plumbing failure and a deliberate stop are different things to report.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedBackend {};
    source.refuseNextAttach();
    auto loop = EventLoop { source };

    REQUIRE(loop.blockOn(awaitReadableOrRefusal(&loop, (*pipe)->readFd())) == -2);
    CHECK(source.attachedCount() == 0);
    CHECK(loop.parkedWaiterCount() == 0);
    CHECK(source.waitCount() == 0); // it never parked, so the loop never waited
}

TEST_CASE("a kernel that refuses the interest leaves no registration behind", "[EventLoop][fd]")
{
    // fastcached#1054's shape as the loop meets it: `attach` succeeded and
    // `setInterest` did not, which on kqueue is the ordinary way a refusal arrives
    // because only the filter reaches the kernel at all. The park must not survive
    // that, or the backend keeps a registration for a flow that has already unwound —
    // and on a real backend that registration names a descriptor the caller is about
    // to close.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedBackend {};
    source.refuseNextSetInterest();
    auto loop = EventLoop { source };

    REQUIRE(loop.blockOn(awaitReadableOrRefusal(&loop, (*pipe)->readFd())) == -2);
    CHECK(source.attachedCount() == 0); // the attach was undone, not left dangling
    CHECK(loop.parkedWaiterCount() == 0);
    CHECK(source.waitCount() == 0);
}

TEST_CASE("a hangup resumes a parked reader, because a park watches one direction", "[EventLoop][fd]")
{
    // A park registers one direction and NO onError, so a failure the kernel
    // volunteers — a hangup, a peer's reset — reaches the direction it does watch.
    // That is what lets the flow resume, look, and report EOF; routing it nowhere
    // would leave it parked on a descriptor that is level-triggered and reported
    // again on every wait, which is a loop at 100% CPU telling nobody.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedBackend {};
    source.pushFailure(HandlerId { 1 });
    auto loop = EventLoop { source };

    constexpr auto Cancelled = -1;
    REQUIRE(loop.blockOn(awaitReadableOrCancel(&loop, (*pipe)->readFd(), Cancelled)) == 1);
    CHECK(loop.parkedWaiterCount() == 0);
}

TEST_CASE("waitReadable on an invalid fd resolves immediately as cancelled", "[EventLoop][fd]")
{
    auto source = ScriptedBackend {};
    auto loop = EventLoop { source };

    constexpr auto Cancelled = -3;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, core::platform::InvalidHandle, Cancelled));

    REQUIRE(result == Cancelled);
    REQUIRE(source.waitCount() == 0); // never blocked: an unwaitable fd resolves inline
}

TEST_CASE("waitReadable resolves over a real SystemPipe via the default backend", "[EventLoop][fd][poll]")
{
    // End-to-end through the real OS readiness path (epoll, kqueue, poll(2) or
    // WaitForMultipleObjects), not the scripted one: a SystemPipe whose write end
    // already holds a byte is
    // readable, so a flow parked on waitReadable resolves on the first real wait and
    // reads the byte back.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    char const payload = 'Z';
    REQUIRE((*pipe)->write(&payload, 1).has_value());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto readByte = [](EventLoop* l, core::platform::SystemPipe* p) -> Task<char> {
        co_await l->waitReadable(p->waitHandle());
        char buf = 0;
        auto const got = p->read(&buf, 1);
        co_return (got.has_value() && got->bytesRead() == 1) ? buf : '\0';
    };

    auto const result = loop.blockOn(readByte(&loop, pipe->get()));
    REQUIRE(result == 'Z');
    REQUIRE(loop.parkedWaiterCount() == 0); // the resumed waiter unregistered its park
}

TEST_CASE("post() wakes a blocked wait and runs its callback on the loop thread", "[EventLoop][post]")
{
    // The root flow parks on a pipe that never receives data, so the backend blocks
    // indefinitely: ONLY IoBackend::wake, which post() calls, can end that wait. A
    // second thread posts a callback that feeds the pipe; the flow completing at all
    // proves the cross-thread wakeup, and the recorded thread id proves the callback
    // ran on the loop thread, not the poster's.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    // Declared after the loop, and safe only because the work that writes here has
    // FINISHED by the time the loop is destroyed. A case that left this work parked would
    // make ~EventLoop resume it into storage already gone; every case here that does so
    // declares its counter BEFORE the loop for that reason.
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
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto poster = std::thread { [&] { loop.post([&] { loop.requestStop(); }); } };

    constexpr auto Cancelled = -7;
    auto const result = loop.blockOn(awaitReadableOrCancel(&loop, (*pipe)->waitHandle(), Cancelled));
    poster.join();

    REQUIRE(result == Cancelled);
    REQUIRE(loop.parkedWaiterCount() == 0); // the cancelled waiter unregistered its park
}

TEST_CASE("Finished spawned flows are released by the turn that ran them", "[EventLoop][spawn]")
{
    // Upstream Endo held every spawned frame until destruction -- an unbounded leak for a
    // long-lived loop spawning per-connection flows -- and contour's fix was a sweep at the top of
    // every pump, which reclaimed them one pump late and cost O(n) per pump to do it. Now the turn
    // that resumes a spawned flow to its end unlinks it there, in O(1).
    auto source = ScriptedBackend {};
    auto loop = EventLoop { source };

    // Declared after the loop, and safe only because the work that writes here has
    // FINISHED by the time the loop is destroyed. A case that left this work parked would
    // make ~EventLoop resume it into storage already gone; every case here that does so
    // declares its counter BEFORE the loop for that reason.
    auto counter = 0;
    loop.spawn(incrementAndFinish(&counter));
    loop.spawn(incrementAndFinish(&counter));
    REQUIRE(loop.spawnedCount() == 2);

    loop.blockOn(awaitZeroDelay(&loop));
    REQUIRE(counter == 2);             // both flows ran to completion...
    REQUIRE(loop.spawnedCount() == 0); // ...and their frames went with the turn that ran them
}

TEST_CASE("withTimeout returns the work's value when it finishes first", "[EventLoop][timeout]")
{
    auto source = ScriptedBackend {};
    auto loop = EventLoop { source };

    // The work completes synchronously, so the timeout arm never matters.
    auto result = loop.blockOn(core::net::withTimeout(&loop, produceValue(42), std::chrono::seconds { 10 }));

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
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto result = loop.blockOn(core::net::withTimeout(
        &loop, parkOnFdForever(&loop, (*pipe)->waitHandle()), std::chrono::milliseconds { 20 }));

    REQUIRE_FALSE(result.has_value());      // the timeout won
    REQUIRE(loop.parkedWaiterCount() == 0); // the cancelled work unregistered its park
}

TEST_CASE("withTimeout drops the loser's timer entry when the work wins after parking",
          "[EventLoop][timeout][clock]")
{
    // Regression: the work first PARKS on a timer, then wins before the deadline.
    // The timeout arm is therefore genuinely scheduled on the timer heap (unlike the
    // synchronous-work case above, where it never parks). When the work wins, whenAny
    // cancels the timeout arm via requeueForCancellation, which MUST drop its heap
    // entry — otherwise the arm's frame is destroyed with a live timer entry still
    // pointing at it, and the loop later dereferences that dangling handle
    // (fireExpiredTimers() at the deadline, or ~EventLoop's wakeAllWaiters at teardown
    // below) — a use-after-free.
    auto clock = ManualClock {};
    auto source = ClockAdvancingBackend { clock, std::chrono::milliseconds { 20 } };
    source.pushTimeout(); // one wait: advances the clock past the work's 10ms delay
    source.pushTimeout(); // spare, should the drain need another pump
    auto loop = EventLoop { source, clock };

    auto fired = false;
    auto const result = loop.blockOn(core::net::withTimeout(
        &loop, awaitDelayThenFire(&loop, 10, &fired), std::chrono::milliseconds { 500 }));

    REQUIRE(fired);
    REQUIRE(result.has_value());
    REQUIRE(*result == 10);
    // The cancelled timeout arm left NO timer entry behind, so no dangling handle
    // survives for the loop teardown (and ASan) to trip over.
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("Destroying the loop unwinds a flow parked on waitReadable", "[EventLoop][fd]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = ScriptedBackend {};
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

// --------------------------------------------------------------------------------------------
// The turn: its order, its bound, and the thread-affinity guarantees it holds.
// --------------------------------------------------------------------------------------------

namespace
{

/// Parks on a deadline an hour out and records how it came back.
/// @param loop The loop to park on.
/// @param outcome 1 if the deadline arrived, 2 if the flow was cancelled.
Task<void> parkForAnHour(EventLoop* loop, int* outcome)
{
    try
    {
        co_await loop->sleepUntil(loop->clock().now() + std::chrono::hours { 1 });
        *outcome = 1;
    }
    catch (OperationCancelled const&)
    {
        *outcome = 2;
    }
}

/// Hands the awaiting coroutine straight back to the loop's ready queue.
///
/// Work that re-queues itself is what a turn's batch bound exists to bound, and a loop's own
/// `resumeSoon` is how the socket layer will do it.
struct ResumeOnLoop
{
    EventLoop* loop; ///< Where to hand the coroutine back.

    /// @return False: always suspend, so the resumption goes through the loop.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine to re-queue.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiting) const
    {
        loop->resumeSoon(core::async::detail::parkedWorkFor(awaiting));
    }

    void await_resume() const noexcept {}
};

/// A flow that yields back to the loop @p limit times, counting its passes.
/// @param loop The loop to yield to.
/// @param passes Incremented once per resumption.
/// @param limit How many passes to make.
Task<void> yieldRepeatedly(EventLoop* loop, int* passes, int limit)
{
    for ([[maybe_unused]] auto const pass: std::views::iota(0, limit))
    {
        ++*passes;
        co_await ResumeOnLoop { loop };
    }
}

/// A ScriptedBackend that publishes whether the loop is inside its wait right now.
///
/// What makes guarantee G2 assertable from the flow's own frame: a resumption that happened
/// inside `backend.wait()` is one a backend performed rather than the loop, and that is the whole
/// class of defect Rule 1 exists to prevent.
class WaitMarkingBackend: public ScriptedBackend
{
  public:
    /// @param timeout Passed through.
    /// @return What the next scripted step dispatched.
    core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        _inWait = true;
        auto const result = ScriptedBackend::wait(timeout);
        _inWait = false;
        return result;
    }

    /// @return True while a wait is in flight on this thread.
    [[nodiscard]] bool inWait() const noexcept { return _inWait; }

  private:
    bool _inWait = false;
};

/// Parks on readiness and records, from its own frame, what was true at the instant it resumed.
/// @param loop The loop to park on.
/// @param backend The backend to ask.
/// @param fd The handle to park on.
/// @param sawWait Set to whether the backend was inside its wait.
/// @param sawDispatch Set to whether a readiness dispatch was in flight.
Task<void> recordResumptionContext(EventLoop* loop,
                                   WaitMarkingBackend* backend,
                                   core::platform::NativeHandle fd,
                                   bool* sawWait,
                                   bool* sawDispatch)
{
    co_await loop->waitReadable(fd);
    *sawWait = backend->inWait();
    *sawDispatch = core::net::detail::readinessDispatchInFlight();
}

/// A flow that finishes at once, so a spawn can be watched being RELEASED.
/// @param finished Incremented when the body runs.
Task<void> finishAtOnce(int* finished)
{
    ++*finished;
    co_return;
}

/// A flow that parks on a deadline nothing reaches, so a spawn can be watched being HELD.
/// @param loop The loop to park on.
Task<void> parkOnce(EventLoop* loop)
{
    co_await loop->sleepUntil(loop->clock().now() + std::chrono::hours { 1 });
}

} // namespace

TEST_CASE("A cross-thread cancel is resolved before the turn drains, so it unwinds in that turn",
          "[EventLoop][turn][cancel]")
{
    // **The ordering of step 1 and step 2, made observable.** A stop requested from another
    // thread reaches the loop as a ParkId in the inbound queue, and step 1 is what turns it into
    // a queued resumption. Resolved AFTER the drain instead, that resumption would sit in the
    // ready queue while steps 3 and 4 computed a timeout and waited -- and this loop's only
    // deadline is an hour out, so a cancel from another thread would take effect an hour later,
    // or never, depending on whether anything else woke the loop.
    auto clock = ManualClock {};
    auto source = ScriptedBackend {};
    source.pushTimeout(); // the one wait the parking turn performs
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto outcome = 0;
    auto loop = EventLoop { source, clock };

    loop.spawn(parkForAnHour(&loop, &outcome));

    auto const parking = loop.runOnce();
    REQUIRE(parking.drained == 1);
    REQUIRE(loop.pendingTimerCount() == 1);
    REQUIRE(outcome == 0);

    // Requested from a thread that is not the loop's, which is where a stop callback commonly
    // runs -- a signal handler, a watchdog, a peer's thread. The callback hands back a ParkId; it
    // does not touch the park table itself.
    auto stopper = std::thread { [&loop] { loop.rootStopSource().request_stop(); } };
    stopper.join();
    CHECK(loop.pendingTimerCount() == 1); // nothing was resolved on that thread
    CHECK(loop.readyCount() == 0);

    auto const settled = loop.runOnce();
    CHECK(settled.drained == 1); // resolved in step 1, resumed in step 2, one turn
    CHECK(outcome == 2);
    CHECK(loop.pendingTimerCount() == 0);
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("A turn resumes at most its dispatch batch and leaves the rest queued", "[EventLoop][turn]")
{
    // A turn is BOUNDED, or work that re-queues itself starves the readiness and deadline steps:
    // a loop whose flows yield in a loop would never reach step 4 at all, and every socket it
    // serves would stall behind them. What the bound must not do is DROP anything, so the
    // remainder is asserted as well as the batch.
    auto clock = ManualClock {};
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto passes = 0;
    auto loop = core::net::testing::TestLoop { clock, core::net::EventLoopOptions { .dispatchBatch = 4 } };

    loop.spawn(yieldRepeatedly(&loop, &passes, 10));

    auto const first = loop.runOnce();
    CHECK(first.drained == 4);
    CHECK(passes == 4);
    CHECK(loop.readyCount() == 1); // queued again, not lost

    auto const second = loop.runOnce();
    CHECK(second.drained == 4);
    CHECK(passes == 8);

    std::ignore = loop.runUntilIdle();
    CHECK(passes == 10);
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("IdlePolicy::Return never blocks inside a turn", "[EventLoop][turn]")
{
    // A loop somebody else drives a turn at a time must not wait inside one: the caller is what
    // waits. The observable form is the timeout it hands the backend -- zero, even with a deadline
    // an hour out that a blocking loop would happily sleep on.
    auto clock = ManualClock {};
    auto source = ScriptedBackend {};
    source.pushTimeout();
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto outcome = 0;
    auto loop =
        EventLoop { source, clock, core::net::EventLoopOptions { .idle = core::net::IdlePolicy::Return } };

    loop.spawn(parkForAnHour(&loop, &outcome));

    auto const turn = loop.runOnce();
    CHECK(turn.drained == 1);
    REQUIRE(source.waitCount() == 1);
    CHECK(timeoutMs(source.recordedTimeouts().back()) == 0);
}

TEST_CASE("G2: a flow is never resumed from inside the backend's wait", "[EventLoop][threading]")
{
    // Rule 1 seen from the loop's side. A backend DISPATCHES -- it runs the callbacks on the
    // handlers it holds -- and those callbacks only enqueue; the resumption happens afterwards, in
    // step 2, on the loop's thread. A resume from inside the wait lets the resumed frame free the
    // object whose entry the backend's walk has not reached yet.
    //
    // Asserted from the FLOW's own frame, which is the only place that can see it: a loop
    // asserting its own invariant would pass on a backend that never resumed anything at all.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    auto source = WaitMarkingBackend {};
    source.pushReadable(HandlerId { 1 });
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto sawWait = true;
    auto sawDispatch = true;
    auto loop = EventLoop { source };

    loop.blockOn(recordResumptionContext(&loop, &source, (*pipe)->readFd(), &sawWait, &sawDispatch));

    CHECK_FALSE(sawWait);
    CHECK_FALSE(sawDispatch);
    CHECK_FALSE(source.inWait()); // and the wait had ended by then, not merely not been entered
}

TEST_CASE("G3: a helper thread only posts, and the work runs on the loop's thread", "[EventLoop][threading]")
{
    // Everything a helper thread may do to a loop -- post, submit, schedule, cancel, stop -- hands
    // work over and returns. None of it runs on the helper's thread, and none of it touches the
    // loop's own containers. What proves it is where the work RAN, recorded by the work itself.
    auto source = ScriptedBackend {};
    auto loop = EventLoop { source };

    auto ranOn = std::thread::id {};
    auto helperRan = std::thread::id {};
    auto const loopThread = std::this_thread::get_id();

    auto helper = std::thread { [&loop, &ranOn, &helperRan] {
        helperRan = std::this_thread::get_id();
        loop.post([&ranOn] { ranOn = std::this_thread::get_id(); });
    } };
    helper.join();

    CHECK(helperRan != loopThread);     // the helper really was another thread
    CHECK(ranOn == std::thread::id {}); // and it ran none of the work itself

    std::ignore = loop.runOnce();
    CHECK(ranOn == loopThread);
}

TEST_CASE("A running loop refuses teardown from any other thread", "[EventLoop][threading]")
{
    // Guarantee G5's two facts, asked of a loop that is actually running. A loop that forgot to
    // claim its worker thread would answer `true` to the rule from every thread -- the false-safe
    // direction, where the teardown assertion stays green while checking nothing. `run()` claims
    // it, and `run()` is non-virtual so no loop can enter the turn without having claimed.
    // A real backend, because this is the one case that actually enters `run()`: an idle turn
    // there BLOCKS on the backend's wake channel, which is what `stop()` breaks. A scripted
    // backend would have to script a step per turn and would race the stop for the last one.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    CHECK_FALSE(loop.running());
    CHECK(loop.teardownIsSerialisedWithDispatch()); // the legitimate "stopped" arm

    auto entered = std::atomic<bool> { false };
    loop.post([&entered] { entered.store(true, std::memory_order_release); });

    auto worker = std::thread { [&loop] { loop.run(); } };
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();

    CHECK(loop.running());
    CHECK_FALSE(loop.isOnWorkerThread());
    CHECK_FALSE(loop.teardownIsSerialisedWithDispatch());

    loop.stop();
    worker.join();

    // Released on the way out, or every later teardown would be refused forever by a loop that
    // has finished.
    CHECK_FALSE(loop.running());
    CHECK(loop.teardownIsSerialisedWithDispatch());
}

TEST_CASE("spawn releases a finished flow in the turn that finished it", "[EventLoop][spawn]")
{
    // O(1) per completion, and what that means observably: the count drops as each flow ends, not
    // on some later turn that sweeps the whole list. A sweep would leave the finished flow's frame
    // -- and everything it holds: a socket, a temporary directory, a slot in somebody's counter --
    // alive until the next turn, and would cost O(n) on every turn to do it.
    auto clock = ManualClock {};
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto finished = 0;
    auto loop = core::net::testing::TestLoop { clock };

    loop.spawn(parkOnce(&loop));         // held: it parks and never comes back
    loop.spawn(finishAtOnce(&finished)); // released: it runs to its end in this turn
    REQUIRE(loop.spawnedCount() == 2);

    std::ignore = loop.runOnce();

    CHECK(finished == 1);
    CHECK(loop.spawnedCount() == 1); // dropped by the turn that ran it, with no sweep
    CHECK(loop.pendingTimerCount() == 1);
}

TEST_CASE("spawn at scale unlinks per completion rather than sweeping", "[EventLoop][spawn]")
{
    // The same property as the case above, at a scale where a sweep is not merely wasteful: with
    // one at the top of every turn, running N flows to their ends is O(N x N/batch), which for ten
    // thousand flows and a batch of sixty-four is sixteen million `done()` calls that answer
    // nothing. The unlink makes it one list erase and one map erase apiece.
    //
    // **Ten thousand rather than a hundred thousand, and the reason is honest rather than tidy.**
    // At a hundred thousand this case spent 38 of a 39-second MSVC Debug run inside the allocator
    // -- 100k coroutine frames, a list node and a map node each, with iterator debugging on -- and
    // took the whole net binary past the 120-second backstop that exists to report a HANG. That
    // measured the Debug CRT's heap, not the loop; what it bought over ten thousand was one more
    // digit. The per-completion assertion below is what actually discriminates a sweep, and it
    // holds at any scale.
    constexpr auto Flows = 10000;
    constexpr auto Batch = std::size_t { 64 };

    auto clock = ManualClock {};
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto finished = 0;
    auto loop =
        core::net::testing::TestLoop { clock, core::net::EventLoopOptions { .dispatchBatch = Batch } };

    for ([[maybe_unused]] auto const index: std::views::iota(0, Flows))
        loop.spawn(finishAtOnce(&finished));
    REQUIRE(loop.spawnedCount() == Flows);

    // One turn, and the count drops by exactly what that turn ran. A sweep at the top of the next
    // turn would leave all ten thousand here.
    auto const first = loop.runOnce();
    CHECK(first.drained == Batch);
    CHECK(loop.spawnedCount() == Flows - Batch);

    std::ignore = loop.runUntilIdle();
    CHECK(finished == Flows);
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("a readiness park dispatched but not yet resumed is still detached at close",
          "[EventLoop][fd][closehang]")
{
    // The window between a dispatch and its resumption is a full turn wide BY CONSTRUCTION:
    // readiness is dispatched in step 4 of one turn and resumed in step 2 of the next, which is
    // guarantee G2. A close landing inside it is the ordinary case — post() is how another thread
    // asks the loop to close a socket — and the close must still find the park. If it cannot, the
    // kernel-side removal is left to the awaiter's own detach a turn later, which issues it
    // against a descriptor number the kernel may already have handed to a new socket.
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes the queued flow and its
    // RAII guard writes here as it unwinds.
    auto destroyed = false;

    auto source = ScriptedBackend {};
    source.pushReadable(HandlerId { 1 }); // turn 1's wait dispatches the park
    auto loop = EventLoop { source };

    loop.spawn(waitReadableWithGuard(&loop, (*pipe)->readFd(), &destroyed));

    // TWO turns, and the reason is not padding. `spawn` off-turn wakes the backend, and a wake
    // consumes no script step — so turn 1's wait returns on the wake and dispatches nothing. Turn
    // 1 is what runs the flow to its park; turn 2's wait is what delivers the scripted readiness.
    // Written as one turn, this case passes while dispatching nothing at all.
    std::ignore = loop.runOnce();
    REQUIRE(source.attachedCount() == 1);
    REQUIRE(loop.readyCount() == 0); // nothing dispatched yet

    std::ignore = loop.runOnce();
    // The discriminator: the waiter IS queued, so the park is in the window this case is about —
    // dispatched in step 4 of one turn, resumed in step 2 of a turn that has not run.
    REQUIRE(loop.readyCount() == 1);

    // Still a registration the loop holds, so the count that names those says so. A park whose
    // waiter has been queued has not stopped being attached.
    CHECK(loop.parkedWaiterCount() == 1);

    loop.notifyHandleClosing((*pipe)->readFd(), core::net::FdWakePolicy::Resume);
    CHECK(source.attachedCount() == 0); // detached at close, not at the resume a turn later
    // And the park is STILL COUNTED here, which is what `parkedWaiterCount` means: one per park
    // holding a handle key, not one per backend registration. The two differ in exactly this
    // window, and the case stopping before this line is what let the doc claim otherwise.
    CHECK(loop.parkedWaiterCount() == 1);
}

TEST_CASE("teardown drops borrowed work still waiting in the inbound queue", "[EventLoop][teardown]")
{
    // `~EventLoop` resumes what it borrows in the ready queue and the park table. It does NOT do
    // so here, and this case is what pins that rather than leaving it to be rediscovered.
    //
    // The reason is that the loop cannot tell what a borrowed handle names. `submit` takes a bare
    // `std::coroutine_handle<>`, so a suspended flow that would unwind and a never-started lazy
    // `Task` that would RUN are the same type -- and a `ResumeOn` continuation is no safer,
    // because `await_resume()` is noexcept and returns rather than throwing, so resuming one runs
    // its body against a loop that is being destroyed.
    //
    // What that costs is exactly what this case shows: a cross-thread hand-off whose loop dies
    // before the next turn strands its flow, and whatever awaits that flow waits forever. An
    // owner that needs the hand-off delivered runs one more turn before destroying the loop.
    auto resumed = false;
    // Outlives the loop, because the loop only borrows this frame: the `Task` here is its owner
    // and what destroys it.
    auto flow = std::optional<Task<void>> {};

    {
        auto clock = ManualClock {};
        auto loop = core::net::testing::TestLoop { clock };

        flow = handOverToLoop(&loop, &resumed);
        // Started by hand, off-turn, so `submit` routes through the inbound queue exactly as a
        // cross-thread `ResumeOn` does.
        flow->handle().resume();

        // Not in the ready queue: that is what makes this the inbound case rather than the one
        // step 2 of the teardown already covers.
        REQUIRE(loop.readyCount() == 0);
        REQUIRE_FALSE(resumed);
    }

    // Still false, and the frame is still suspended. Destroying `flow` below is what frees it.
    CHECK_FALSE(resumed);
}

TEST_CASE("blockOn completes a flow that leaves the loop and comes back", "[EventLoop][blockOn]")
{
    // While the flow is on the pool, the loop has nothing queued, nothing parked and nothing
    // inbound -- and a `blockOn` that reads that instant as "this can no longer advance" answers
    // about a flow that is RUNNING, then destroys its frame under the thread running it.
    //
    // The loop's own wake channel is what makes waiting correct rather than optimistic: the
    // pool's `ResumeOn { loop }` calls `submit`, which wakes the backend, which ends the wait.
    // Nothing here depends on the 40ms elapsing -- it exists only to make the window wide enough
    // that the loop is certainly idle inside it.
    auto pool = core::async::ThreadPoolExecutor { 2 };
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };

    auto const result = loop.blockOn(hopToPoolAndBack(&loop, &pool));
    CHECK(result == 7);
}

TEST_CASE("blockOn asks for an indefinite wait rather than polling", "[EventLoop][blockOn]")
{
    // core-cpp#17 was filed about the SPIN, and this is the property that answers it: while the
    // flow is on the pool, the loop has nothing to do and must ask to SLEEP, not to poll. The
    // assertion is on the argument `wait()` received, because that is what `blockOn` controls.
    auto pool = core::async::ThreadPoolExecutor { 1 };
    auto const inner = core::net::makeDefaultBackend();
    auto recording = RecordingBackend { *inner };
    auto loop = EventLoop { recording };

    auto const result = loop.blockOn(hopToPoolAndBack(&loop, &pool));
    REQUIRE(result == 7);

    // Read after the drive returned, on the thread that ran it: see RecordingBackend's note.
    auto const& asked = recording.timeouts();
    REQUIRE_FALSE(asked.empty());
    // At least one indefinite wait. A poll-forever loop records only zeros.
    CHECK(std::ranges::any_of(asked, [](auto const& t) { return !t.has_value(); }));
}

namespace
{

/// The leaf of a nested flow: it parks in ITS OWN frame, so the ready entry that resumes the chain
/// names this frame rather than the spawned root.
/// @param loop The loop to park on.
/// @param delay How long to park.
Task<void> parkInLeaf(EventLoop* loop, std::chrono::milliseconds delay)
{
    co_await loop->sleepUntil(loop->clock().now() + delay);
}

/// A spawned flow that awaits a sub-task and ends when the sub-task does, inside the leaf's resume:
/// the leaf's final_suspend transfers here, and this runs to its end in the same `resume()`.
/// @param loop The loop to park on.
/// @param delay How long the leaf parks.
/// @param finished Incremented when this flow reaches its end, normally or by unwinding.
Task<void> nestedFlow(EventLoop* loop, std::chrono::milliseconds delay, int* finished)
{
    struct Count
    {
        explicit Count(int* counter) noexcept: finished(counter) {}
        int* finished;
        Count(Count const&) = delete;
        Count(Count&&) = delete;
        Count& operator=(Count const&) = delete;
        Count& operator=(Count&&) = delete;
        ~Count() { ++*finished; }
    };
    auto const count = Count { finished };
    co_await parkInLeaf(loop, delay);
}

} // namespace

TEST_CASE("spawn releases a nested flow whose completion arrives through its sub-task's frame",
          "[EventLoop][spawn]")
{
    // The unlink used to key on the frame a ready entry names. A flow that parks in a sub-task's
    // frame is resumed through THAT frame, completes inside the same resume by symmetric transfer,
    // and nothing unlinked it: it stayed in `_roots`, and in spawnedCount(), until ~EventLoop -- one
    // leaked frame per connection for a daemon that spawns a flow per connection.
    auto clock = ManualClock {};
    auto finished = 0;
    auto loop = core::net::testing::TestLoop { clock };

    loop.spawn(nestedFlow(&loop, std::chrono::milliseconds { 10 }, &finished));
    std::ignore = loop.runOnce();
    REQUIRE(loop.spawnedCount() == 1);
    REQUIRE(finished == 0);

    clock.advance(std::chrono::milliseconds { 10 });
    std::ignore = loop.runUntilIdle();
    CHECK(finished == 1);
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("spawn releases a nested flow that requestStop unwinds", "[EventLoop][spawn]")
{
    auto clock = ManualClock {};
    auto finished = 0;
    auto loop = core::net::testing::TestLoop { clock };

    loop.spawn(nestedFlow(&loop, std::chrono::hours { 1 }, &finished));
    std::ignore = loop.runOnce();
    REQUIRE(loop.spawnedCount() == 1);

    loop.requestStop();
    std::ignore = loop.runUntilIdle();
    CHECK(finished == 1);
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("spawn at scale unlinks nested flows per completion", "[EventLoop][spawn]")
{
    // The per-completion property of "spawn at scale unlinks per completion rather than sweeping",
    // for flows that complete through a sub-task's frame. One turn's drain releases exactly what it
    // ran to completion; a sweep, or no unlink at all, leaves them all.
    //
    // **Ten thousand, not a hundred thousand, for the sibling case's reason.** At a hundred thousand
    // this case alone took 26 of cl-debug's seconds -- two frames and a park per flow, under the
    // Debug CRT's heap -- inside a binary bounded at 120, which the sanitizer legs run slower
    // still. The per-turn assertion is what tells an unlink from a sweep, and it holds at any scale.
    constexpr auto Flows = 10000;
    constexpr auto Batch = std::size_t { 64 };

    auto clock = ManualClock {};
    auto finished = 0;
    auto loop =
        core::net::testing::TestLoop { clock, core::net::EventLoopOptions { .dispatchBatch = Batch } };

    for ([[maybe_unused]] auto const index: std::views::iota(0, Flows))
        loop.spawn(nestedFlow(&loop, std::chrono::milliseconds { 1 }, &finished));
    std::ignore = loop.runUntilIdle();
    REQUIRE(loop.spawnedCount() == Flows);
    REQUIRE(finished == 0);

    // The turn that finds the deadlines due queues the leaves; the next one drains a batch of them,
    // and each resume runs its whole chain to the end.
    clock.advance(std::chrono::milliseconds { 1 });
    std::ignore = loop.runOnce();
    auto const drainingTurn = loop.runOnce();
    CHECK(drainingTurn.drained == Batch);
    CHECK(std::cmp_equal(finished, Batch));
    CHECK(loop.spawnedCount() == static_cast<std::size_t>(Flows) - Batch);

    std::ignore = loop.runUntilIdle();
    CHECK(finished == Flows);
    CHECK(loop.spawnedCount() == 0);
}

namespace
{

/// Hands the awaiting flow to the back of the ready queue, the way a flow yields.
struct YieldOnce
{
    EventLoop* loop;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> self) const
    {
        loop->resumeSoon(core::async::ParkedWork { .resume = self });
    }
    void await_resume() const noexcept {}
};

/// Parks the awaiting flow until somebody hands its handle to @c EventLoop::resumeSoon.
struct ParkUntilCompleted
{
    std::coroutine_handle<>* parked;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> self) const noexcept { *parked = self; }
    void await_resume() const noexcept {}
};

/// What the ordering case observed, in order.
struct Trace
{
    std::vector<std::string_view> events;
    std::coroutine_handle<> waiter;
    EventLoop* loop = nullptr;
};

/// The waiter W: parks until the callback completes it, then records that it ran.
Task<void> waiterFlow(Trace* trace)
{
    co_await ParkUntilCompleted { &trace->waiter };
    trace->events.emplace_back("W");
}

/// The flow A: its deadline falls due ahead of the callback's, so the turn that finds both due
/// queues A first; A then yields once to let "readiness that was already reported" run, and
/// records what it finds after the yield.
Task<void> yieldingFlow(Trace* trace, core::platform::SteadyTimePoint deadline)
{
    co_await trace->loop->sleepUntil(deadline);
    trace->events.emplace_back("A1");
    co_await YieldOnce { trace->loop }; // the yield that must come AFTER the callback's waiter
    trace->events.emplace_back("A2");
}

/// The callback C: completes the waiter, as a readiness callback completes a socket operation.
/// @param state The @c Trace.
void completeWaiter(void* state)
{
    auto* const trace = static_cast<Trace*>(state);
    trace->events.emplace_back("C");
    trace->loop->resumeSoon(core::async::ParkedWork { .resume = std::exchange(trace->waiter, {}) });
}

} // namespace

TEST_CASE("A waiter completed by a drain-step callback resumes in the callback's position",
          "[EventLoop][turn][ordering]")
{
    // 0.2.1 made a completion from a callback go through `resumeSoon`, onto the BACK of the ready
    // queue. So with A queued ahead of callback C, the drain ran A, C, then A's own yield, and only
    // then C's waiter W: a flow that yields once to let already-reported readiness run -- fastcached's
    // AbandonIfPeerGone -- read state W had not updated yet. 0.2.0 resumed W inline, in C's position.
    // The order restored, without resuming inside the callback (G2): W runs right after C returns.
    auto clock = ManualClock {};
    auto trace = Trace {};
    auto loop = core::net::testing::TestLoop { clock };
    trace.loop = &loop;

    auto const start = clock.now();
    loop.spawn(waiterFlow(&trace));
    loop.spawn(yieldingFlow(&trace, start + std::chrono::milliseconds { 1 }));
    std::ignore = loop.addTimer(start + std::chrono::milliseconds { 2 }, &completeWaiter, &trace);
    std::ignore = loop.runUntilIdle(); // W parks on its completion, A on its deadline
    REQUIRE(trace.events.empty());

    // Both due in one turn, queued in deadline order: A ahead of C.
    clock.advance(std::chrono::milliseconds { 2 });
    std::ignore = loop.runUntilIdle();

    CHECK(trace.events == std::vector<std::string_view> { "A1", "C", "W", "A2" });
}

namespace
{

/// W, twice: it parks, is completed by one callback, records, parks again, and is completed by a
/// second callback.
Task<void> waiterTwice(Trace* trace)
{
    co_await ParkUntilCompleted { &trace->waiter };
    trace->events.emplace_back("W1");
    co_await ParkUntilCompleted { &trace->waiter };
    trace->events.emplace_back("W2");
}

/// A, interleaved with both callbacks: each of its deadlines falls due just ahead of one callback,
/// and after each it yields once.
Task<void> yieldingTwice(Trace* trace,
                         core::platform::SteadyTimePoint first,
                         core::platform::SteadyTimePoint second)
{
    co_await trace->loop->sleepUntil(first);
    trace->events.emplace_back("A1");
    co_await YieldOnce { trace->loop };
    trace->events.emplace_back("A2");
    co_await trace->loop->sleepUntil(second);
    trace->events.emplace_back("A3");
    co_await YieldOnce { trace->loop };
    trace->events.emplace_back("A4");
}

/// A callback that queues its waiter and then takes it back with `cancelPending`.
/// @param state The @c Trace.
void completeThenCancel(void* state)
{
    auto* const trace = static_cast<Trace*>(state);
    auto const waiter = trace->waiter;
    trace->loop->resumeSoon(core::async::ParkedWork { .resume = waiter });
    trace->events.emplace_back(trace->loop->cancelPending(waiter) ? "taken back" : "not found");
}

/// A spawned flow that ends in an exception.
Task<void> throwAtOnce()
{
    co_await std::suspend_never {};
    throw std::runtime_error { "a spawned flow's own failure" };
}

/// A spawned flow that continues on @p pool and ends there, normally or by throwing.
/// @param pool Where to end.
/// @param ended Set on @p pool just before the flow ends.
/// @param fail Whether to end by throwing.
Task<void> endOnPool(core::async::ThreadPoolExecutor* pool, std::atomic<bool>* ended, bool fail)
{
    co_await core::async::ResumeOn { *pool };
    ended->store(true, std::memory_order_release);
    if (fail)
        throw std::runtime_error { "ended on the pool by throwing" };
}

/// Waits, bounded, for @p flag.
/// @param flag What to wait for.
/// @return Whether it was set within five seconds.
[[nodiscard]] bool waitFor(std::atomic<bool> const& flag)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 5000))
    {
        if (flag.load(std::memory_order_acquire))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }
    return false;
}

} // namespace

TEST_CASE("A waiter completed by two callbacks in turn resumes in each one's position",
          "[EventLoop][turn][ordering]")
{
    // W parks again after the first completion and is completed by a SECOND callback, through the
    // ordinary path: no recursion through the first callback's position, and the second puts it in
    // its own. A interleaves with both, yielding once after each of its deadlines.
    auto clock = ManualClock {};
    auto trace = Trace {};
    auto loop = core::net::testing::TestLoop { clock };
    trace.loop = &loop;

    auto const start = clock.now();
    loop.spawn(waiterTwice(&trace));
    loop.spawn(yieldingTwice(
        &trace, start + std::chrono::milliseconds { 1 }, start + std::chrono::milliseconds { 3 }));
    std::ignore = loop.addTimer(start + std::chrono::milliseconds { 2 }, &completeWaiter, &trace);
    std::ignore = loop.addTimer(start + std::chrono::milliseconds { 4 }, &completeWaiter, &trace);
    std::ignore = loop.runUntilIdle();
    REQUIRE(trace.events.empty());

    clock.advance(std::chrono::milliseconds { 2 });
    std::ignore = loop.runUntilIdle();
    clock.advance(std::chrono::milliseconds { 2 });
    std::ignore = loop.runUntilIdle();

    CHECK(trace.events == std::vector<std::string_view> { "A1", "C", "W1", "A2", "A3", "C", "W2", "A4" });
    CHECK_FALSE(trace.waiter);
}

TEST_CASE("cancelPending finds a waiter a callback queued, before the callback returns",
          "[EventLoop][turn][ordering]")
{
    // What a callback queues sits apart from the ready queue until the callback returns, so the
    // search that takes a queued handle back has to look there too, or it answers "not queued" for
    // a waiter that is about to run.
    auto clock = ManualClock {};
    auto trace = Trace {};
    auto loop = core::net::testing::TestLoop { clock };
    trace.loop = &loop;

    auto waiter = waiterFlow(&trace);
    waiter.handle().resume(); // parks, and records its handle
    REQUIRE(trace.waiter);

    std::ignore = loop.addTimer(clock.now(), &completeThenCancel, &trace);
    std::ignore = loop.runUntilIdle();
    CHECK(trace.events == std::vector<std::string_view> { "taken back" });
    CHECK_FALSE(waiter.handle().done()); // taken back, so never resumed
}

TEST_CASE("spawn releases a flow that ends in an exception, and contains it", "[EventLoop][spawn]")
{
    auto clock = ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    loop.spawn(throwAtOnce());
    REQUIRE(loop.spawnedCount() == 1);
    CHECK_NOTHROW(loop.runUntilIdle());
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("spawn releases a flow that ends on another thread", "[EventLoop][spawn][threads]")
{
    // A spawned flow may END anywhere: `co_await ResumeOn { pool }` and return, or throw, there.
    // Its root's final suspension then runs on the pool thread, so it hands its slot to the loop
    // through the inbound queue rather than writing the loop's own list beside a turn -- and the
    // loop reaps it on its own thread, in the next turn's first step.
    auto fail = false;
    SECTION("returning there")
    {
    }
    SECTION("throwing there")
    {
        fail = true;
    }
    auto pool = core::async::ThreadPoolExecutor { 1 };
    auto clock = ManualClock {};
    auto ended = std::atomic<bool> { false };
    auto loop = core::net::testing::TestLoop { clock };

    loop.spawn(endOnPool(&pool, &ended, fail));
    std::ignore = loop.runOnce(); // starts it; it leaves for the pool
    REQUIRE(waitFor(ended));

    auto turns = 0;
    while (loop.spawnedCount() > 0 && turns < 5000)
    {
        std::ignore = loop.runOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        ++turns;
    }
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("A loop destroyed with a finished root it has not reaped frees it", "[EventLoop][spawn][threads]")
{
    // The root finished on the pool and handed its slot over; no turn ran after that. The
    // destructor frees the root with the rest, and forgets the hand-off rather than reaping
    // through it later.
    auto pool = core::async::ThreadPoolExecutor { 1 };
    auto clock = ManualClock {};
    auto ended = std::atomic<bool> { false };
    {
        auto loop = core::net::testing::TestLoop { clock };
        loop.spawn(endOnPool(&pool, &ended, false));
        std::ignore = loop.runOnce();
        REQUIRE(waitFor(ended));
        // Wait, bounded, for the hand-off itself, which follows `ended` on the pool thread.
        auto waited = 0;
        while (loop.inboundFinishedRootCount() == 0 && waited < 5000)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
            ++waited;
        }
        REQUIRE(loop.inboundFinishedRootCount() == 1);
        REQUIRE(loop.spawnedCount() == 1);
    }
    CHECK(ended.load());
}

namespace
{

/// A scripted backend whose `wake()`, called from any thread but the loop's, stalls before it
/// returns -- long enough for a loop thread that does not wait for it to reap, and for its owner to
/// destroy the loop, while that thread is still inside the loop's backend.
class StallingWakeBackend final: public ScriptedBackend
{
  public:
    /// @param loopThread The thread that drives the loop; its own wakes do not stall.
    explicit StallingWakeBackend(std::thread::id loopThread): _loopThread(loopThread)
    {
        for ([[maybe_unused]] auto const step: std::views::iota(0, 10000))
            pushTimeout();
    }

    void wake() noexcept override
    {
        ScriptedBackend::wake();
        if (std::this_thread::get_id() == _loopThread)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds { 200 });
        wakeReturned.store(true, std::memory_order_release);
    }

    std::atomic<bool> wakeReturned { false }; ///< Set as an off-thread wake returns.

  private:
    std::thread::id _loopThread;
};

/// What the nested-cancel case observed.
struct NestedCancel
{
    EventLoop* loop = nullptr;
    std::coroutine_handle<> x;
    bool takenBack = false;
};

/// The outer callback: queues X, then drives a nested turn that runs the inner one.
/// @param state The @c NestedCancel.
void queueXThenNest(void* state)
{
    auto* const nested = static_cast<NestedCancel*>(state);
    nested->loop->resumeSoon(core::async::ParkedWork { .resume = nested->x });
    std::ignore = nested->loop->runOnce();
}

/// The inner callback: takes X back while the outer callback is still running.
/// @param state The @c NestedCancel.
void cancelX(void* state)
{
    auto* const nested = static_cast<NestedCancel*>(state);
    nested->takenBack = nested->loop->cancelPending(nested->x);
}

} // namespace

TEST_CASE("A root that ends on another thread is released only once that thread is out of the loop",
          "[EventLoop][spawn][threads]")
{
    // The hand-off woke the backend AFTER releasing the inbound lock, so the loop could reap the root
    // -- and its owner, seeing spawnedCount() reach zero, destroy the loop -- while the pool thread
    // was still inside `wake()`: a use-after-free of the backend and the loop. The wake is made
    // under the lock now, and the reap takes the lock, so zero is seen only after the wake returned.
    auto pool = core::async::ThreadPoolExecutor { 1 };
    auto clock = ManualClock {};
    auto ended = std::atomic<bool> { false };
    auto backend = StallingWakeBackend { std::this_thread::get_id() };
    auto loop =
        EventLoop { backend, clock, core::net::EventLoopOptions { .idle = core::net::IdlePolicy::Return } };

    loop.spawn(endOnPool(&pool, &ended, false));
    std::ignore = loop.runOnce();
    REQUIRE(waitFor(ended));

    auto turns = 0;
    while (loop.spawnedCount() > 0 && turns < 5000)
    {
        std::ignore = loop.runOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        ++turns;
    }
    REQUIRE(loop.spawnedCount() == 0);
    CHECK(backend.wakeReturned.load(std::memory_order_acquire));
}

TEST_CASE("A nested callback that takes back what an outer callback queued leaves both ranges valid",
          "[EventLoop][turn][ordering]")
{
    // The outer callback queues X and drives a nested turn; the inner callback, whose range begins
    // after X, takes X back. Erasing X shifted the inner range below its own start, and the splice
    // then ran on an invalid range. X is marked taken instead, and the splice skips it.
    auto clock = ManualClock {};
    auto trace = Trace {};
    auto loop = core::net::testing::TestLoop { clock };
    trace.loop = &loop;
    auto nested = NestedCancel { .loop = &loop, .x = {}, .takenBack = false };

    auto x = waiterFlow(&trace);
    x.handle().resume(); // parks
    nested.x = trace.waiter;
    REQUIRE(nested.x);

    std::ignore = loop.addTimer(clock.now(), &queueXThenNest, &nested);
    std::ignore = loop.addTimer(clock.now(), &cancelX, &nested);
    std::ignore = loop.runUntilIdle();
    CHECK(nested.takenBack);
    CHECK_FALSE(x.handle().done()); // taken back, so never resumed
    CHECK(trace.events.empty());
}

namespace
{

/// Touches the loop's park table when destroyed -- as a flow owning a timer does when it unwinds.
struct CancelsTimerOnDestroy
{
    CancelsTimerOnDestroy(EventLoop* owner, core::net::TimerId armed, bool* answer) noexcept:
        loop(owner), timer(armed), cancelled(answer)
    {
    }
    EventLoop* loop;
    core::net::TimerId timer;
    bool* cancelled;
    CancelsTimerOnDestroy(CancelsTimerOnDestroy const&) = delete;
    CancelsTimerOnDestroy(CancelsTimerOnDestroy&&) = delete;
    CancelsTimerOnDestroy& operator=(CancelsTimerOnDestroy const&) = delete;
    CancelsTimerOnDestroy& operator=(CancelsTimerOnDestroy&&) = delete;
    ~CancelsTimerOnDestroy() { *cancelled = loop->cancelTimer(timer); }
};

/// Parks the awaiting flow and hands over its work WITH the chain's claim, as an awaitable does.
struct ParkWithClaim
{
    core::async::ParkedWork* parked;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> self) const
    {
        *parked = core::async::detail::parkedWorkFor(self);
    }
    void await_resume() const noexcept {}
};

/// An owned chain -- nobody holds it -- that parks, and whose frame touches the loop when freed.
core::async::DetachedTask ownedWaiter(EventLoop* loop,
                                      core::net::TimerId timer,
                                      core::async::ParkedWork* parked,
                                      bool* cancelled)
{
    auto const touch = CancelsTimerOnDestroy { loop, timer, cancelled };
    co_await ParkWithClaim { parked };
}

/// What the throwing callback needs.
struct ThrowingCompletion
{
    EventLoop* loop = nullptr;
    core::async::ParkedWork* parked = nullptr;
};

/// Completes the waiter, and then throws.
/// @param state The @c ThrowingCompletion.
void completeThenThrow(void* state)
{
    auto* const completion = static_cast<ThrowingCompletion*>(state);
    completion->loop->resumeSoon(std::move(*completion->parked));
    throw std::runtime_error { "a callback that fails after completing its waiter" };
}

/// A timer callback that does nothing.
void doNothing(void* /*state*/)
{
}

} // namespace

TEST_CASE("A waiter a throwing callback completed is still the loop's to free at teardown",
          "[EventLoop][turn][ordering]")
{
    // What a callback queued moves into the callback position when it returns -- and, until this
    // was a scope guard, only when it returned: a callback that queued its waiter and THREW left it
    // in `_resumeFirst`, which no teardown step reads. The owned chain was then freed with the
    // loop's members, after the park table it touches on the way out: a use-after-free under ASan.
    auto clock = ManualClock {};
    auto parked = core::async::ParkedWork {};
    auto cancelled = false;
    {
        auto loop = core::net::testing::TestLoop { clock };
        auto const timer = loop.addTimer(clock.now() + std::chrono::hours { 1 }, &doNothing, nullptr);
        ownedWaiter(&loop, timer, &parked, &cancelled);
        REQUIRE(parked.resume);

        auto completion = ThrowingCompletion { .loop = &loop, .parked = &parked };
        std::ignore = loop.addTimer(clock.now(), &completeThenThrow, &completion);
        CHECK_THROWS_AS(loop.runUntilIdle(), std::runtime_error);
        CHECK(loop.readyCount() == 1); // the waiter, spilled to the ready queue by the guard
    }
    // The chain was freed by the teardown, while the park table was still there to answer.
    CHECK(cancelled);
}
