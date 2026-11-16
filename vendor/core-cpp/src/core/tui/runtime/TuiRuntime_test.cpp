// SPDX-License-Identifier: Apache-2.0
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/SignalHandler.hpp>
#include <core/platform/SystemPipe.hpp>
#include <core/platform/Types.hpp>
#include <core/platform/Wakeup.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>
#include <core/tui/runtime/testing/ScriptedInputSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

using core::async::OperationCancelled;
using core::async::Task;
using core::net::EventLoop;
using core::net::testing::HandlerId;
using core::net::testing::ScriptedBackend;
using core::platform::ManualClock;
using core::platform::SystemPipe;
using core::tui::EndProbe;
using core::tui::InputEvent;
using core::tui::KeyEvent;
using core::tui::runtime::ActivityKind;
using core::tui::runtime::TuiRuntime;
using core::tui::runtime::TuiRuntimeOptions;
using core::tui::runtime::testing::HandleFor;
using core::tui::runtime::testing::ScriptedInputSource;

// fastcached#1546: MSVC 19.44's ARM64 code generator drops the enclosing `try` of a `co_await` on
// a temporary awaiter whose `await_ready` makes a call, so an `OperationCancelled` from
// `await_resume` passes every handler. An `await_ready` here answers a constant and the decision
// is `await_suspend`'s (.agent/rules/async-and-net.md); a call put back fails to compile wherever
// the question can be asked at compile time (core::async::awaitReadyIsConstantFalse).
static_assert(core::async::awaitReadyIsConstantFalse<core::tui::runtime::NextInputEventAwaiter>());
static_assert(core::async::awaitReadyIsConstantFalse<core::tui::runtime::NextEventForAwaiter>());
static_assert(core::async::awaitReadyIsConstantFalse<core::tui::runtime::NextActivityAwaiter>());
static_assert(core::async::awaitReadyIsConstantFalse<core::tui::runtime::NextAgentReadyAwaiter>());

// On registration ids: the runtime's input flow is the first thing to park, and a source with no
// resize channel starts no second flow, so the input registration is HandlerId{1}. It re-registers
// on every re-park, so the SECOND read of a scripted case is HandlerId{2}.

namespace
{

KeyEvent keyOf(char32_t codepoint)
{
    return KeyEvent { .codepoint = codepoint };
}

/// @param pipes The pipes to open.
/// @return @p pipes open channels, or an empty vector if the OS refused one.
[[nodiscard]] std::vector<std::unique_ptr<SystemPipe>> openPipes(std::size_t pipes)
{
    auto opened = std::vector<std::unique_ptr<SystemPipe>> {};
    opened.reserve(pipes);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, pipes))
    {
        auto pipe = core::platform::createSystemPipe();
        if (!pipe)
            return {};
        opened.push_back(std::move(*pipe));
    }
    return opened;
}

/// Returns the codepoint of the next key event the runtime delivers.
Task<char32_t> awaitOneKeyCodepoint(TuiRuntime* runtime)
{
    auto const event = co_await runtime->nextEvent();
    co_return std::get<KeyEvent>(event).codepoint;
}

/// Returns the next key's codepoint and records the thread its flow resumed on.
Task<char32_t> awaitOneKeyOnThread(TuiRuntime* runtime, std::thread::id* resumedOn)
{
    auto const event = co_await runtime->nextEvent();
    *resumedOn = std::this_thread::get_id();
    co_return std::get<KeyEvent>(event).codepoint;
}

/// Sums the codepoints of @p count key events, in arrival order.
Task<int> sumKeyCodepoints(TuiRuntime* runtime, int count)
{
    auto sum = 0;
    for ([[maybe_unused]] auto const index: std::views::iota(0, count))
    {
        auto const event = co_await runtime->nextEvent();
        sum += static_cast<int>(std::get<KeyEvent>(event).codepoint);
    }
    co_return sum;
}

/// Returns 1 if nextEventFor yielded an event, 0 if it resolved with none.
Task<int> awaitEventForResult(TuiRuntime* runtime, int timeoutMs)
{
    auto const event = co_await runtime->nextEventFor(std::chrono::milliseconds { timeoutMs });
    co_return event.has_value() ? 1 : 0;
}

/// The first half of the two-flow interleave on the input slot: it parks on @c nextEventFor and is
/// woken by non-input ACTIVITY, which takes it out of the slot and hands it to the loop — so its
/// `await_resume` runs one queue entry later, after a sibling has taken the slot it left.
/// @param runtime The runtime to await.
/// @param timeoutMs A timeout long enough that it cannot be what resumes this flow.
/// @param resumedWithoutEvent Set when the wait resolved with no event, which is what a focus
///        report produces: consumed by the source, so nothing is buffered.
Task<void> awaitEventForWokenByActivity(TuiRuntime* runtime, int timeoutMs, bool* resumedWithoutEvent)
{
    try
    {
        auto const event = co_await runtime->nextEventFor(std::chrono::milliseconds { timeoutMs });
        *resumedWithoutEvent = !event.has_value();
    }
    catch (OperationCancelled const&)
    {
        // Teardown after a failed assertion; the flag stays false and the case reports that.
        co_return;
    }
}

/// The second half: it parks on a delay that expires in the same turn the input handle becomes
/// readable, so it resumes BETWEEN the sibling leaving the slot and the sibling's `await_resume`,
/// and the deadline it arms there is the one that release would wrongly retire.
/// @param runtime The runtime to await.
/// @param delayMs How long to sleep before taking the slot.
/// @param timeoutMs The timeout whose survival is the assertion.
/// @param timedOut Set when the wait resolves with nothing — that is, when its deadline fired.
Task<void> delayThenAwaitEventFor(TuiRuntime* runtime, int delayMs, int timeoutMs, bool* timedOut)
{
    try
    {
        co_await runtime->delay(std::chrono::milliseconds { delayMs });
        auto const event = co_await runtime->nextEventFor(std::chrono::milliseconds { timeoutMs });
        *timedOut = !event.has_value();
    }
    catch (OperationCancelled const&)
    {
        // Teardown after a failed assertion; the flag stays false and the case reports that.
        co_return;
    }
}

/// Returns the kind of the first activity nextActivity observes (as its enum value).
Task<int> awaitActivityKind(TuiRuntime* runtime, int timeoutMs)
{
    auto const activity = co_await runtime->nextActivity(std::chrono::milliseconds { timeoutMs });
    co_return static_cast<int>(activity.kind);
}

/// Returns true once an agent message is observed.
Task<bool> awaitAgentReady(TuiRuntime* runtime)
{
    co_await runtime->nextAgentReady();
    co_return true;
}

/// Awaits input; returns a sentinel if cancelled instead.
Task<int> awaitKeyOrCancel(TuiRuntime* runtime, int cancelSentinel)
{
    try
    {
        auto const event = co_await runtime->nextEvent();
        co_return static_cast<int>(std::get<KeyEvent>(event).codepoint);
    }
    catch (OperationCancelled const&)
    {
        co_return cancelSentinel;
    }
}

/// Resumes immediately when the delay has already elapsed (it declines to park).
Task<int> awaitZeroDelay(TuiRuntime* runtime)
{
    co_await runtime->delay(0ms);
    co_return 7;
}

/// Parks on a delay of @p delayMs, then sets *fired and returns it.
Task<int> awaitDelayThenFire(TuiRuntime* runtime, int delayMs, bool* fired)
{
    co_await runtime->delay(std::chrono::milliseconds { delayMs });
    *fired = true;
    co_return delayMs;
}

/// Waits for @p handle to become readable; returns 1 on readiness or @p cancelSentinel if
/// cancelled while parked.
Task<int> awaitReadableOrCancel(TuiRuntime* runtime, core::platform::NativeHandle handle, int cancelSentinel)
{
    try
    {
        co_await runtime->waitReadable(handle);
        co_return 1;
    }
    catch (OperationCancelled const&)
    {
        co_return cancelSentinel;
    }
}

/// Waits for @p handle to become readable and counts the resolution in *resolved.
Task<void> countReadable(TuiRuntime* runtime, core::platform::NativeHandle handle, std::size_t* resolved)
{
    co_await runtime->waitReadable(handle);
    ++*resolved;
}

/// A value-producing task that completes synchronously — the "work wins" arm.
Task<int> produceValue(int value)
{
    co_return value;
}

/// Parks on a never-ready handle forever (until cancelled) — the "timeout wins" arm.
Task<int> parkOnHandleForever(TuiRuntime* runtime, core::platform::NativeHandle handle)
{
    co_await runtime->waitReadable(handle);
    co_return 0;
}

/// The whenAny WINNER: completes without ever suspending.
Task<void> completeNow()
{
    co_return;
}

/// The whenAny LOSER for core-cpp#18: parks on input and must unwind cancelled, which it records.
/// The record is the point -- a race that resolves because its loser was never really parked
/// would look the same from outside.
Task<void> parkOnInputUntilCancelled(TuiRuntime* runtime, bool* cancelled)
{
    try
    {
        auto const event = co_await runtime->nextEvent();
        static_cast<void>(event);
    }
    catch (OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// The whenAny LOSER for core-cpp#16: parks on a delay and must unwind cancelled.
Task<void> parkOnDelayUntilCancelled(TuiRuntime* runtime, bool* cancelled)
{
    try
    {
        co_await runtime->delay(50ms);
    }
    catch (OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// Races a flow parked on input against one that finishes at once, and reports the winner.
/// @param loserCancelled Set when the parked arm unwinds cancelled.
Task<int> raceOnInput(TuiRuntime* runtime, bool* loserCancelled)
{
    auto tasks = std::vector<Task<void>> {};
    tasks.reserve(2);
    tasks.push_back(parkOnInputUntilCancelled(runtime, loserCancelled));
    tasks.push_back(completeNow());
    auto const winner = co_await core::async::whenAny(std::move(tasks));
    co_return winner.has_value() ? static_cast<int>(*winner) : -1;
}

/// Races a flow parked on a delay against one that finishes at once, lets the loser's frame go,
/// and then outlives the deadline that loser was parked on.
Task<int> raceOnDelayThenOutliveIt(TuiRuntime* runtime, bool* loserCancelled)
{
    {
        auto tasks = std::vector<Task<void>> {};
        tasks.reserve(2);
        tasks.push_back(parkOnDelayUntilCancelled(runtime, loserCancelled));
        tasks.push_back(completeNow());
        static_cast<void>(co_await core::async::whenAny(std::move(tasks)));
    } // the loser's frame is destroyed here, with the race state

    co_await runtime->delay(120ms); // past the 50ms the cancelled delay was parked on
    co_return 1;
}

/// Sets *destroyed = true when its frame unwinds (RAII), so a case can prove a parked flow was
/// cancelled-and-unwound rather than raw-destroyed.
struct UnwindFlag
{
    bool* destroyed;

    ~UnwindFlag() { *destroyed = true; }
};

/// Parks forever on input with an RAII guard; the guard must still run if the flow is cancelled.
Task<void> parkForeverWithGuard(TuiRuntime* runtime, bool* destroyed)
{
    *destroyed = false;
    auto guard = UnwindFlag { destroyed };
    try
    {
        auto const event = co_await runtime->nextEvent();
        static_cast<void>(event);
    }
    catch (OperationCancelled const&)
    {
        static_cast<void>(destroyed);
    }
}

/// A ScriptedBackend that advances an injected ManualClock by a fixed step on every wait, so a
/// deadline is crossed deterministically with no real time passing.
class ClockAdvancingBackend: public ScriptedBackend
{
  public:
    /// @param clock The clock each wait advances.
    /// @param step How much time one wait stands for.
    ClockAdvancingBackend(ManualClock& clock, std::chrono::milliseconds step) noexcept:
        _clock(clock), _step(step)
    {
    }

    [[nodiscard]] core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        _clock.advance(_step);
        return ScriptedBackend::wait(timeout);
    }

  private:
    ManualClock& _clock;
    std::chrono::milliseconds _step;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Input delivery, over a real backend and a real readiness channel.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Runtime delivers an input event to a waiting flow", "[TuiRuntime]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    source.pushEvents({ InputEvent { keyOf(U'x') } });
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'x');
}

TEST_CASE("Runtime delivers buffered events in arrival order", "[TuiRuntime]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    // Two events decoded from one read are buffered and consumed in order.
    source.pushEvents({ InputEvent { keyOf(U'a') }, InputEvent { keyOf(U'b') } });
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(sumKeyCodepoints(&runtime, 2))
            == static_cast<int>(U'a') + static_cast<int>(U'b'));
}

TEST_CASE("Protocol reports never surface as input events", "[TuiRuntime]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    // A focus report (internal) followed by a real key: the flow must see only the key.
    source.pushEvents(
        { InputEvent { core::tui::FocusEvent { .focused = true } }, InputEvent { keyOf(U'z') } });
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'z');
}

TEST_CASE("Events a terminal query handed back are delivered without waiting", "[TuiRuntime]")
{
    // They were read off the input handle BEFORE anything still on it, so nothing is going to
    // become readable on their account: if the runtime only looked at them when a source flow
    // woke, a key typed during a DA1 probe would be stranded until the next keystroke.
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource {}; // no pipe at all: nothing can ever become ready
    source.pushPending({ InputEvent { keyOf(U'q') } });
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'q');
}

TEST_CASE("A resize arrives as an input event on its own channel", "[TuiRuntime]")
{
    auto const pipes = openPipes(2);
    REQUIRE(pipes.size() == 2);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get(), pipes[1].get() };
    source.pushResize(core::tui::ResizeEvent { .columns = 120, .rows = 40 });
    auto runtime = TuiRuntime { loop, source };

    auto awaitResize = [](TuiRuntime* rt) -> Task<int> {
        auto const event = co_await rt->nextEvent();
        co_return std::get<core::tui::ResizeEvent>(event).columns;
    };

    REQUIRE(runtime.blockOn(awaitResize(&runtime)) == 120);
}

TEST_CASE("nextEventFor yields an event that arrives before the timeout", "[TuiRuntime]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    source.pushEvents({ InputEvent { keyOf(U'e') } });
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(awaitEventForResult(&runtime, 5000)) == 1);
}

TEST_CASE("nextEventFor resolves with nothing when its deadline elapses", "[TuiRuntime]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() }; // nothing scripted: nothing arrives
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(awaitEventForResult(&runtime, 1)) == 0);
    // The deadline was retired by the resume, not left on the loop to fire into an empty slot.
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("nextActivity reports an input event, an agent message, or a timeout", "[TuiRuntime]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();

    SECTION("input event")
    {
        auto loop = EventLoop { *backend };
        auto source = ScriptedInputSource { pipes[0].get() };
        source.pushEvents({ InputEvent { keyOf(U'a') } });
        auto runtime = TuiRuntime { loop, source };
        REQUIRE(runtime.blockOn(awaitActivityKind(&runtime, 5000)) == static_cast<int>(ActivityKind::Event));
    }
    SECTION("agent message, already pending when the flow asks")
    {
        auto loop = EventLoop { *backend };
        auto source = ScriptedInputSource { pipes[0].get() };
        auto runtime = TuiRuntime { loop, source };
        loop.post([&runtime] { runtime.notifyAgentReady(); });
        REQUIRE(runtime.blockOn(awaitActivityKind(&runtime, 5000))
                == static_cast<int>(ActivityKind::AgentReady));
    }
    SECTION("timeout")
    {
        auto loop = EventLoop { *backend };
        auto source = ScriptedInputSource { pipes[0].get() };
        auto runtime = TuiRuntime { loop, source };
        REQUIRE(runtime.blockOn(awaitActivityKind(&runtime, 1)) == static_cast<int>(ActivityKind::Timeout));
    }
}

TEST_CASE("An agent message posted while a flow is parked resumes it", "[TuiRuntime][agent]")
{
    // The interesting order: the waiter parks FIRST and the message arrives afterwards, from the
    // loop's cross-thread surface, which is what replaced the agent wakeup handle.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto runtime = TuiRuntime { loop, source };

    // `std::thread` with an explicit join rather than `std::jthread`: AppleClang's libc++ has no
    // `<stop_token>`, so it has no `jthread` either, and this file has to build there. Nothing a
    // Windows or Linux preset runs can catch that -- CI's macOS leg is what did.
    auto worker = std::thread { [&loop, &runtime] {
        std::this_thread::sleep_for(20ms);
        loop.post([&runtime] { runtime.notifyAgentReady(); });
    } };

    auto const ready = runtime.blockOn(awaitAgentReady(&runtime));
    worker.join();
    REQUIRE(ready);
}

TEST_CASE("Non-input activity resolves a timed waiter but never a plain nextEvent", "[TuiRuntime]")
{
    // A focus change is activity, not input. `nextEventFor` can say "nothing happened" and so is
    // told; `nextEvent` cannot -- its await_resume yields an event or throws -- so resuming it
    // would report a focus change as a cancellation, which used to close an open modal.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();

    SECTION("a timed waiter resolves with no event")
    {
        auto loop = EventLoop { *backend };
        auto source = ScriptedInputSource { pipes[0].get() };
        source.pushEvents({ InputEvent { core::tui::FocusEvent { .focused = true } } });
        auto runtime = TuiRuntime { loop, source };
        REQUIRE(runtime.blockOn(awaitEventForResult(&runtime, 5000)) == 0);
    }
    SECTION("a plain nextEvent waits past it for real input")
    {
        auto loop = EventLoop { *backend };
        auto source = ScriptedInputSource { pipes[0].get() };
        source.pushEvents({ InputEvent { core::tui::FocusEvent { .focused = true } } });
        source.pushEvents({ InputEvent { keyOf(U'm') } });
        auto runtime = TuiRuntime { loop, source };
        REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'm');
    }
}

// ---------------------------------------------------------------------------------------------
// The backend matrix: the same scenario over every readiness mechanism this platform builds.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Input arriving while the loop waits resumes the flow on the loop thread", "[TuiRuntime][backend]")
{
    // Nothing is ready when the flow parks, so the loop is inside its backend's wait when the
    // other thread scripts the input -- the case the runtime exists for, and the one no scripted
    // backend can stand in for. Run over every backend this platform builds: on Windows that is
    // IOCP, and the terminal input handle is the reason its waitable-HANDLE bridge exists at all.
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        auto backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue; // not built on this platform

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto const pipes = openPipes(1);
            REQUIRE(pipes.size() == 1);
            auto loop = EventLoop { *backend };
            auto source = ScriptedInputSource { pipes[0].get() };
            auto runtime = TuiRuntime { loop, source };

            auto const driver = std::this_thread::get_id();
            auto resumedOn = std::thread::id {};
            // `std::thread` with an explicit join, for `jthread`'s absence on AppleClang.
            auto writer = std::thread { [&source] {
                std::this_thread::sleep_for(30ms);
                source.pushEvents({ InputEvent { keyOf(U'w') } });
            } };

            auto const key = runtime.blockOn(awaitOneKeyOnThread(&runtime, &resumedOn));
            writer.join();
            REQUIRE(key == U'w');
            REQUIRE(resumedOn == driver);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Deadlines, which are the loop's.
// ---------------------------------------------------------------------------------------------

TEST_CASE("delay(0) resumes without waiting", "[TuiRuntime][clock]")
{
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend };
    auto source = ScriptedInputSource {};
    auto runtime = TuiRuntime { loop, source };

    REQUIRE(runtime.blockOn(awaitZeroDelay(&runtime)) == 7);
    REQUIRE(backend.waitCount() == 0); // the ready path never reaches a wait
}

TEST_CASE("A pending delay bounds the wait and fires when the clock crosses it", "[TuiRuntime][clock]")
{
    // Each wait stands for 40ms of clock time, so a 100ms delay is still pending after two and
    // due on the third -- deterministic, with no real time passing.
    auto clock = ManualClock {};
    auto backend = ClockAdvancingBackend { clock, 40ms };
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    REQUIRE(source.inputHandle() != core::platform::InvalidHandle); // the premise, now checkable
    auto runtime = TuiRuntime { loop, source };
    for ([[maybe_unused]] auto const step: std::views::iota(0, 6))
        backend.pushTimeout();

    auto fired = false;
    REQUIRE(runtime.blockOn(awaitDelayThenFire(&runtime, 100, &fired)) == 100);

    REQUIRE(fired);
    // The ARGUMENT, not just the outcome: the first wait was bounded by the pending deadline
    // rather than left indefinite or forced to zero.
    REQUIRE(backend.recordedTimeouts().front().has_value());
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(*backend.recordedTimeouts().front())
            == 100ms);
    REQUIRE(backend.waitCount() == 3);
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("A read that decodes to nothing is flushed once its escape deadline elapses",
          "[TuiRuntime][clock][escape]")
{
    // The defect the CHANGELOG claims: a lone ESC is a prefix of every arrow key, so a decoder
    // cannot tell Escape from an unfinished sequence without a clock -- and the old runtime called
    // the parser's timeout hook only when its multiplexed wait timed out, which for a flow parked
    // on `nextEvent()` with no timer pending meant an indefinite wait and a hook that never ran.
    auto clock = ManualClock {};
    auto backend = ClockAdvancingBackend { clock, 40ms };
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    REQUIRE(source.inputHandle() != core::platform::InvalidHandle); // the premise, now checkable
    // Bytes arrive and decode to nothing -- no scripted read -- and THEN the flush completes them.
    source.pushFlush({ InputEvent { keyOf(U'\x1b') } });
    auto runtime = TuiRuntime { loop, source };

    backend.pushReadable(HandlerId { 1 }); // the input handle becomes readable
    for ([[maybe_unused]] auto const step: std::views::iota(0, 6))
        backend.pushTimeout(); // each wait advances the clock 40ms, so 50ms is crossed on the second

    REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'\x1b');
    REQUIRE(source.flushCount() == 1);
    // The flush timer is the runtime's and keyed on `this`, so it must not outlive the delivery.
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("A read that decodes to something arms no escape flush", "[TuiRuntime][clock][escape]")
{
    // The other branch, and the one that decides whether an ordinary keystroke leaves a 50ms timer
    // behind on every single read.
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    REQUIRE(source.inputHandle() != core::platform::InvalidHandle); // the premise, now checkable
    source.pushEvents({ InputEvent { keyOf(U'a') } });
    auto runtime = TuiRuntime { loop, source };

    backend.pushReadable(HandlerId { 1 });
    backend.pushTimeout();

    REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'a');
    REQUIRE(source.flushCount() == 0);
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("A waiter released after a sibling took the input slot leaves the sibling's timeout armed",
          "[TuiRuntime][clock][regression]")
{
    // H1: `releaseInputWaiter` retired `_inputDeadline` unconditionally, and by the time a QUEUED
    // waiter's `await_resume` runs that deadline may belong to a different flow -- so a
    // `nextEventFor`/`nextActivity` was silently left parked with no timeout at all.
    //
    // **The fix shipped with no case because the defect was derived to be unreachable, and the
    // derivation was wrong.** It ran: the ready queue is FIFO, so anything that could park is
    // either ahead of the queued waiter -- and so ran while the slot was still occupied, which
    // `parkOnInput` asserts against -- or appended behind it. The flaw is "ahead ... so ran while
    // the slot was occupied": the queued waiter is appended DURING the drain, by the entry at its
    // front, so everything already behind that front entry runs after the slot was emptied and
    // before the waiter resumes. `EventLoop::turn` produces such neighbours as a matter of course:
    // step 4 dispatches readiness into `_ready` and step 5 fires expired deadlines into the same
    // queue, and the next turn's step 2 drains them back to back.
    //
    // So the shape below is not contrived, it is the ordinary one:
    //
    //   turn 2  step 4 queues the input flow (readable), step 5 queues the sibling (delay due)
    //   turn 3  drain: input flow -> focus report -> notifyActivity takes the first waiter out of
    //                  the slot and appends it;  sibling -> parks on input, arming ITS deadline;
    //                  first waiter -> await_resume -> releaseInputWaiter.
    //
    // A focus report is what makes the wake leave the buffer EMPTY: the source consumes it, so the
    // sibling's `await_ready` does not short-circuit on a buffered event.
    auto clock = ManualClock {};
    auto backend = ClockAdvancingBackend { clock, 40ms };
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    REQUIRE(source.inputHandle() != core::platform::InvalidHandle); // the premise, checkable
    source.pushEvents({ InputEvent { core::tui::FocusEvent { .focused = true } } });
    auto runtime = TuiRuntime { loop, source };

    auto activityResumed = false;
    auto siblingTimedOut = false;
    runtime.spawn(awaitEventForWokenByActivity(&runtime, 10'000, &activityResumed));
    runtime.spawn(delayThenAwaitEventFor(&runtime, 50, 120, &siblingTimedOut));

    // The first wait consumes the wakes `submit` and `spawn` left behind and spends no script
    // step, as core-cpp#17's case says; the SECOND is the one that reports the input handle ready.
    backend.pushReadable(HandlerId { 1 });
    for ([[maybe_unused]] auto const step: std::views::iota(0, 30))
        backend.pushTimeout();

    std::ignore = loop.runOnce(core::platform::SteadyDuration::zero()); // clock 40ms: all three park
    std::ignore = loop.runOnce(core::platform::SteadyDuration::zero()); // clock 80ms: readable, 50ms due
    std::ignore = loop.runOnce(core::platform::SteadyDuration::zero()); // the interleaved drain

    // The premise, asserted rather than assumed: the interleave only happens if the activity wake
    // really did resume the first flow with no event.
    REQUIRE(activityResumed);
    // The sibling's 120ms deadline, still armed. The defect retires it here, and NOTHING else
    // observable changes -- no assertion trips, nothing is logged, and in a release build the
    // symptom is a TUI that stops redrawing.
    CHECK(loop.pendingTimerCount() == 1);

    // And the consequence, which is what a count alone cannot show: the timeout must actually
    // fire. Bounded at 20 turns -- 3 would do, each wait advancing the clock 40ms past the 200ms
    // the deadline falls at -- so a lost timeout reports as a failure here rather than as a hang.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 20))
    {
        if (siblingTimedOut)
            break;
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
    }
    REQUIRE(siblingTimedOut);
}

// ---------------------------------------------------------------------------------------------
// Handle waits, which are the loop's too.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// core-cpp#49: an input handle at its end ends the input stream.
//
// These cases drive the loop a bounded number of turns rather than through blockOn(): the defect
// is a flow that never finishes, and blockOn() would run the script out (a throw out of a parked
// flow) where a turn count reports it.
// ---------------------------------------------------------------------------------------------

namespace
{

constexpr auto EndedSentinel = -1;

/// More hangups than @c TurnBound turns can consume, so a runtime that re-parks after reading the
/// end reads once per turn for the whole bound instead of running the script out.
constexpr auto HangupScript = std::uint64_t { 12 };

/// How many turns a runtime gets to report the end. A correct one needs three: park, dispatch the
/// hangup, and resume the waiter it woke.
constexpr auto TurnBound = std::size_t { 8 };

/// Scripts a hangup on each of the input handle's first @c HangupScript registrations. A handle at
/// its end is reported on every wait, whoever is parked on it, and the runtime re-registers on
/// every re-park, so registration N is HandlerId{N}.
void scriptHangups(ScriptedBackend& backend)
{
    for (auto const id: std::views::iota(std::uint64_t { 1 }, HangupScript + 1))
        backend.pushFailure(HandlerId { id });
}

/// What a spawned flow produced, and whether it has finished.
struct Outcome
{
    bool done = false;
    int value = 0;
};

/// Runs @p work to completion and records its result in @p outcome.
Task<void> record(Task<int> work, Outcome* outcome)
{
    outcome->value = co_await std::move(work);
    outcome->done = true;
}

/// Runs turns until @p outcome is done or @c TurnBound turns have run.
/// @return How many turns ran.
std::size_t turnUntilDone(EventLoop& loop, Outcome const& outcome)
{
    auto turns = std::size_t { 0 };
    while (!outcome.done && turns < TurnBound)
    {
        std::ignore = loop.runOnce(0ms);
        ++turns;
    }
    return turns;
}

/// Awaits nextEventFor; returns the key's codepoint, 0 for "nothing", or @c EndedSentinel.
Task<int> awaitEventForOrCancel(TuiRuntime* runtime, int timeoutMs)
{
    try
    {
        auto const event = co_await runtime->nextEventFor(std::chrono::milliseconds { timeoutMs });
        co_return event ? static_cast<int>(std::get<KeyEvent>(*event).codepoint) : 0;
    }
    catch (OperationCancelled const&)
    {
        co_return EndedSentinel;
    }
}

/// Awaits nextActivity; returns the activity kind's value, or @c EndedSentinel.
Task<int> awaitActivityOrCancel(TuiRuntime* runtime, int timeoutMs)
{
    try
    {
        auto const activity = co_await runtime->nextActivity(std::chrono::milliseconds { timeoutMs });
        co_return static_cast<int>(activity.kind);
    }
    catch (OperationCancelled const&)
    {
        co_return EndedSentinel;
    }
}

} // namespace

TEST_CASE("core-cpp#49: an input handle at its end cancels nextEvent instead of spinning",
          "[TuiRuntime][hangup]")
{
    auto first = Outcome {};
    auto second = Outcome {};
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.closeInput();
    auto runtime = TuiRuntime { loop, source };
    scriptHangups(backend);

    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &first));
    auto const turns = turnUntilDone(loop, first);
    INFO("turns: " << turns << ", reads: " << source.readCount());
    CHECK(first.done);
    CHECK(first.value == EndedSentinel);
    CHECK(runtime.inputClosed());
    // One read found the end, and the handle was not watched again. A runtime that re-parked read
    // once per turn: the spin, bounded here by the turn count instead of by nothing.
    CHECK(source.readCount() == 1);

    // A wait after the end does not park: nothing is left that could wake it. (Only once the first
    // wait has ended: the runtime holds one input waiter, and asserts it.)
    REQUIRE(first.done);
    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &second));
    std::ignore = loop.runOnce(0ms);
    CHECK(second.done);
    CHECK(second.value == EndedSentinel);
    CHECK(source.readCount() == 1);
}

TEST_CASE("core-cpp#49: input read before the end is delivered before the stream ends",
          "[TuiRuntime][hangup]")
{
    auto key = Outcome {};
    auto after = Outcome {};
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushEvents({ InputEvent { keyOf(U'k') } });
    source.closeInput();
    auto runtime = TuiRuntime { loop, source };
    backend.pushReadable(HandlerId { 1 }); // the key
    scriptHangups(backend);                // registration 1 is gone by then; 2 is the re-park

    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &key));
    std::ignore = turnUntilDone(loop, key);
    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &after));
    auto const turns = turnUntilDone(loop, after);
    INFO("turns: " << turns << ", reads: " << source.readCount());
    CHECK(key.value == 'k');
    CHECK(after.done);
    CHECK(after.value == EndedSentinel);
    CHECK(runtime.inputClosed());
    CHECK(source.readCount() == 2);
}

TEST_CASE("core-cpp#49: the timed waits end with the input stream too", "[TuiRuntime][hangup]")
{
    auto outcome = Outcome {};
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.closeInput();
    auto runtime = TuiRuntime { loop, source };
    scriptHangups(backend);

    // A deadline the manual clock, which nothing advances, cannot reach: what resolves these waits
    // has to be the end of the input.
    SECTION("nextEventFor")
    {
        loop.spawn(record(awaitEventForOrCancel(&runtime, 60'000), &outcome));
    }
    SECTION("nextActivity")
    {
        loop.spawn(record(awaitActivityOrCancel(&runtime, 60'000), &outcome));
    }
    auto const turns = turnUntilDone(loop, outcome);
    INFO("turns: " << turns << ", reads: " << source.readCount());
    CHECK(outcome.done);
    CHECK(outcome.value == EndedSentinel);
    CHECK(runtime.inputClosed());
    CHECK(source.readCount() == 1);
}

TEST_CASE("core-cpp#49: nextActivity reports a pending agent message before the end",
          "[TuiRuntime][hangup][agent]")
{
    auto ended = Outcome {};
    auto agent = Outcome {};
    auto after = Outcome {};
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.closeInput();
    auto runtime = TuiRuntime { loop, source };
    scriptHangups(backend);

    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &ended));
    std::ignore = turnUntilDone(loop, ended);
    REQUIRE(runtime.inputClosed());

    runtime.notifyAgentReady();
    loop.spawn(record(awaitActivityOrCancel(&runtime, 60'000), &agent));
    std::ignore = turnUntilDone(loop, agent);
    loop.spawn(record(awaitActivityOrCancel(&runtime, 60'000), &after));
    std::ignore = turnUntilDone(loop, after);
    CHECK(agent.value == static_cast<int>(ActivityKind::AgentReady));
    CHECK(after.value == EndedSentinel);
}

// ---------------------------------------------------------------------------------------------
// An input whose end reaches no handle: a Windows console whose host process died. Its handle is
// never signalled again, so the read that finds every other end never runs; only asking finds it.
// ---------------------------------------------------------------------------------------------

namespace
{

/// More waits than any case below needs, each standing for @c ProbeStep of clock time.
constexpr auto ProbeScript = 16;
constexpr auto ProbeStep = 250ms;

} // namespace

TEST_CASE("An input whose end no readiness reports is asked, and its end cancels nextEvent",
          "[TuiRuntime][hangup][probe]")
{
    // No readiness is scripted at all: the handle is never signalled, as a dead console's is not.
    auto clock = ManualClock {};
    auto backend = ClockAdvancingBackend { clock, ProbeStep };
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushProbe(EndProbe::Open);  // asked at construction
    source.pushProbe(EndProbe::Open);  // one interval later
    source.pushProbe(EndProbe::Ended); // two intervals later: the host is gone
    auto runtime = TuiRuntime { loop, source, TuiRuntimeOptions { .endProbe = 1000ms } };
    for ([[maybe_unused]] auto const step: std::views::iota(0, ProbeScript))
        backend.pushTimeout();

    // Turns are bounded below the script, so a runtime that never asked fails here, not by a hang.
    auto outcome = Outcome {};
    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &outcome));
    for ([[maybe_unused]] auto const turn: std::views::iota(0, ProbeScript))
    {
        if (outcome.done)
            break;
        std::ignore = loop.runOnce(); // the loop's own timeout, so the probe's deadline bounds it
    }

    CHECK(outcome.done);
    CHECK(outcome.value == EndedSentinel);
    CHECK(runtime.inputClosed());
    CHECK(source.probeCount() == 3);
    CHECK(source.readCount() == 0); // the end was asked for, not read
    // The ARGUMENT: the first wait was bounded by the probe, where it was indefinite before.
    REQUIRE(backend.recordedTimeouts().front().has_value());
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(*backend.recordedTimeouts().front())
          == 1000ms);
    CHECK(loop.pendingTimerCount() == 0); // an ended input is asked no more
}

TEST_CASE("An input whose end arrives as readiness is asked once and costs no timer",
          "[TuiRuntime][hangup][probe]")
{
    // Every POSIX terminal and pipe: its hangup makes the handle readable, so a timer that woke the
    // loop to ask would be a wakeup per interval for the life of every TUI, bought for nothing.
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input }; // answers NotNeeded
    auto const runtime = TuiRuntime { loop, source };
    REQUIRE(source.probeCount() == 0); // the constructor asks nothing: it may not be on the loop's thread
    backend.pushTimeout();
    std::ignore = loop.runOnce(); // the first turn, where the question is asked

    CHECK(source.probeCount() == 1);
    CHECK(loop.pendingTimerCount() == 0);
    CHECK_FALSE(runtime.inputClosed());
}

TEST_CASE("An end probe of zero never asks", "[TuiRuntime][hangup][probe]")
{
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushProbe(EndProbe::Open);
    auto const runtime = TuiRuntime { loop, source, TuiRuntimeOptions { .endProbe = 0ms } };
    backend.pushTimeout();
    std::ignore = loop.runOnce();

    CHECK(source.probeCount() == 0);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("An input found ended in the first turn declines the first wait", "[TuiRuntime][hangup][probe]")
{
    // A prompt drawn after a foreground job ran while the console died: the runtime is built over an
    // input that is already gone, and the first wait has to learn it at once.
    auto outcome = Outcome {};
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushProbe(EndProbe::Ended);
    auto runtime = TuiRuntime { loop, source };
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, TurnBound))
        backend.pushTimeout();

    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &outcome));
    auto const turns = turnUntilDone(loop, outcome);
    INFO("turns: " << turns);
    CHECK(outcome.done);
    CHECK(outcome.value == EndedSentinel);
    CHECK(runtime.inputClosed());
    CHECK(source.probeCount() == 1);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("A partial sequence held when the probe finds the end is delivered before the end",
          "[TuiRuntime][hangup][probe][escape]")
{
    // Bytes arrived and decoded to nothing -- a lone ESC -- and then the console died before the
    // escape flush ran. No continuation is coming, so the ESC is delivered, and THEN the stream ends.
    auto key = Outcome {};
    auto after = Outcome {};
    auto clock = ManualClock {};
    auto backend = ClockAdvancingBackend { clock, ProbeStep };
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushFlush({ InputEvent { keyOf(U'\x1b') } });
    source.pushProbe(EndProbe::Open);
    source.pushProbe(EndProbe::Ended);
    // An escape flush far beyond the probe, so it is the probe that has to deliver the ESC.
    auto runtime =
        TuiRuntime { loop, source, TuiRuntimeOptions { .escapeFlush = 60'000ms, .endProbe = 1000ms } };
    backend.pushReadable(HandlerId { 1 }); // the bytes that decode to nothing
    for ([[maybe_unused]] auto const step: std::views::iota(0, ProbeScript))
        backend.pushTimeout();

    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &key));
    for ([[maybe_unused]] auto const turn: std::views::iota(0, ProbeScript))
    {
        if (key.done)
            break;
        std::ignore = loop.runOnce();
    }
    REQUIRE(key.done);
    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &after));
    std::ignore = turnUntilDone(loop, after);

    CHECK(key.value == 0x1b);
    CHECK(after.done);
    CHECK(after.value == EndedSentinel);
    CHECK(source.flushCount() == 1);
    CHECK(loop.pendingTimerCount() == 0); // the escape flush was retired with the input
}

TEST_CASE("An input that ends by a read is asked no more", "[TuiRuntime][hangup][probe]")
{
    // The two ways an end is found must not both run: once a read has found it, the probe that is
    // waiting out its interval asks nothing more when it wakes.
    auto outcome = Outcome {};
    auto clock = ManualClock {};
    auto backend = ClockAdvancingBackend { clock, ProbeStep };
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushProbe(EndProbe::Open); // the first turn
    source.pushProbe(EndProbe::Open); // what a probe that kept asking would get, one interval later
    source.closeInput();
    auto runtime = TuiRuntime { loop, source, TuiRuntimeOptions { .endProbe = 1000ms } };
    scriptHangups(backend); // the first wait reports the end; the rest are 250ms that pass

    loop.spawn(record(awaitKeyOrCancel(&runtime, EndedSentinel), &outcome));
    for ([[maybe_unused]] auto const turn: std::views::iota(std::uint64_t { 0 }, HangupScript))
    {
        if (outcome.done && loop.pendingTimerCount() == 0)
            break;
        std::ignore = loop.runOnce(); // the loop's own timeout, so the probe's interval elapses
    }

    CHECK(outcome.value == EndedSentinel);
    CHECK(source.readCount() == 1);
    CHECK(source.probeCount() == 1);
    CHECK(loop.pendingTimerCount() == 0); // the probe woke after its interval, and stopped
}

TEST_CASE("A runtime destroyed while it is still asking leaves nothing parked", "[TuiRuntime][hangup][probe]")
{
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend, clock };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    source.pushProbe(EndProbe::Open);
    {
        auto const runtime = TuiRuntime { loop, source };
        backend.pushTimeout();
        std::ignore = loop.runOnce();
        REQUIRE(source.probeCount() == 1);
        REQUIRE(loop.pendingTimerCount() == 1); // the probe, waiting out its interval
    }
    // The probe's frame names the runtime, so one left parked would resume into freed storage.
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("waitReadable resolves over a real SystemPipe", "[TuiRuntime][fd]")
{
    auto const pipes = openPipes(2);
    REQUIRE(pipes.size() == 2);
    char const payload = 'Z';
    REQUIRE(pipes[1]->write(&payload, 1).has_value());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto runtime = TuiRuntime { loop, source };

    auto readByte = [](TuiRuntime* rt, SystemPipe* pipe) -> Task<char> {
        co_await rt->waitReadable(pipe->waitHandle());
        char byte = 0;
        auto const got = pipe->read(&byte, 1);
        co_return (got.has_value() && got->bytesRead() == 1) ? byte : '\0';
    };

    REQUIRE(runtime.blockOn(readByte(&runtime, pipes[1].get())) == 'Z');
}

TEST_CASE("waitReadable on an invalid handle resolves immediately as cancelled", "[TuiRuntime][fd]")
{
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend };
    auto source = ScriptedInputSource {};
    auto runtime = TuiRuntime { loop, source };

    constexpr auto Cancelled = -3;
    REQUIRE(runtime.blockOn(awaitReadableOrCancel(&runtime, core::platform::InvalidHandle, Cancelled))
            == Cancelled);
    REQUIRE(backend.waitCount() == 0); // an unwaitable handle resolves inline
}

TEST_CASE("More concurrent handle waits than one native wait accepts all resolve",
          "[TuiRuntime][fd][chunking]")
{
    // core-cpp#19: the Windows event source called WaitForMultipleObjects with an unchecked count
    // and mapped its refusal past 64 handles onto "interrupted", so a consumer watching ~60
    // descriptors saw the whole TUI unwind as if Ctrl+C had been pressed. The loop's Windows
    // backend sweeps its set in chunks instead, and this runtime inherits that by being composed
    // on it -- so the number below is deliberately past MAXIMUM_WAIT_OBJECTS.
    constexpr auto Watched = std::size_t { 70 };
    auto const pipes = openPipes(Watched + 1);
    if (pipes.size() != Watched + 1)
        SKIP("the OS refused " << (Watched + 1) << " pipes");

    char const payload = 'r';
    for (auto const index: std::views::iota(std::size_t { 1 }, pipes.size()))
        REQUIRE(pipes[index]->write(&payload, 1).has_value());

    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto runtime = TuiRuntime { loop, source };

    auto resolved = std::size_t { 0 };
    auto watchAll = [](TuiRuntime* rt,
                       std::vector<std::unique_ptr<SystemPipe>> const* channels,
                       std::size_t* count) -> Task<void> {
        auto waiters = std::vector<Task<void>> {};
        waiters.reserve(channels->size() - 1);
        for (auto const index: std::views::iota(std::size_t { 1 }, channels->size()))
            waiters.push_back(countReadable(rt, (*channels)[index]->waitHandle(), count));
        co_await core::async::whenAll(std::move(waiters));
    };

    runtime.blockOn(watchAll(&runtime, &pipes, &resolved));
    REQUIRE(resolved == Watched);
}

// ---------------------------------------------------------------------------------------------
// Interrupts.
// ---------------------------------------------------------------------------------------------

TEST_CASE("An interrupt cancels a flow parked on input", "[TuiRuntime][interrupt]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto wakeup = core::platform::Wakeup {};
    auto runtime = TuiRuntime { loop, source, TuiRuntimeOptions { .interruptWakeup = &wakeup } };

    // What SignalHandler does on Ctrl+C: record the signal, then signal the wakeup the loop is
    // already watching. The interrupt flow parked there is what runs the policy.
    core::platform::SignalHandler::simulateSigint();
    wakeup.signal();

    constexpr auto Sentinel = -99;
    REQUIRE(runtime.blockOn(awaitKeyOrCancel(&runtime, Sentinel)) == Sentinel);
    // The flow observed it, so the flag is consumed: a second wait must not see this one again.
    REQUIRE_FALSE(core::platform::SignalHandler::hasPendingSigint());
}

TEST_CASE("An installed interrupt handler replaces the default stop", "[TuiRuntime][interrupt]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto wakeup = core::platform::Wakeup {};
    auto runtime = TuiRuntime { loop, source, TuiRuntimeOptions { .interruptWakeup = &wakeup } };

    // A consumer that scopes cancellation -- closing the open modal rather than the program --
    // delivers a key instead of stopping the loop, so the parked flow resumes normally.
    auto interrupts = 0;
    runtime.setInterruptHandler([&interrupts, &source] {
        ++interrupts;
        source.pushEvents({ InputEvent { keyOf(U'i') } });
    });

    core::platform::SignalHandler::simulateSigint();
    wakeup.signal();

    constexpr auto Sentinel = -99;
    REQUIRE(runtime.blockOn(awaitKeyOrCancel(&runtime, Sentinel)) == static_cast<int>(U'i'));
    REQUIRE(interrupts == 1);
    REQUIRE_FALSE(core::platform::SignalHandler::hasPendingSigint());
}

TEST_CASE("An interrupt handler that ends its runtime posts the teardown, and it leaves nothing",
          "[TuiRuntime][interrupt][teardown]")
{
    // The FOURTH state a source flow can be in at teardown, RUNNING, and the one the destructor
    // refuses rather than handles. The handler runs inline inside `interruptFlow`, so a handler
    // that destroyed the runtime directly would destroy that flow's frame -- and the handler's own
    // `std::function` -- while both are executing. Built and run, not argued: with the assertion
    // taken out, doing that reports a heap-use-after-free under AddressSanitizer; with it in, the
    // assertion fires. Neither is a passing case, so what is committed is the prescribed remedy:
    // post the teardown, and by the time it runs the flow is parked again -- state 2, which the
    // destructor takes back.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto wakeup = core::platform::Wakeup {};
    auto runtime = std::optional<TuiRuntime> {};
    runtime.emplace(loop, source, TuiRuntimeOptions { .interruptWakeup = &wakeup });

    auto interrupts = 0;
    runtime->setInterruptHandler([&interrupts, &loop, &runtime] {
        ++interrupts;
        loop.post([&runtime] { runtime.reset(); });
    });

    // The premise: both source flows are parked before the interrupt, so the teardown has two
    // registrations to take back and "nothing left" is not what an idle loop reports anyway.
    std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
    REQUIRE(loop.parkedWaiterCount() == 2);

    core::platform::SignalHandler::simulateSigint();
    wakeup.signal();

    // Bounded: readiness is dispatched in one turn, drained in the next, and the post runs in the
    // one after that at the latest. A teardown that never ran reports as a failure, not a hang.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 10))
    {
        if (!runtime)
            break;
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
    }

    REQUIRE(interrupts == 1);
    REQUIRE_FALSE(runtime.has_value());
    REQUIRE(loop.parkedWaiterCount() == 0);
    REQUIRE(loop.readyCount() == 0);
    REQUIRE_FALSE(core::platform::SignalHandler::hasPendingSigint());
}

// ---------------------------------------------------------------------------------------------
// withTimeout, which is core::net's and takes the loop.
// ---------------------------------------------------------------------------------------------

TEST_CASE("withTimeout returns the work's value when it finishes first", "[TuiRuntime][timeout]")
{
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend };
    auto source = ScriptedInputSource {};
    auto runtime = TuiRuntime { loop, source };

    auto result = runtime.blockOn(
        core::net::withTimeout(&runtime.loop(), produceValue(42), std::chrono::seconds { 10 }));

    REQUIRE(result.has_value());
    REQUIRE(*result == 42);
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("withTimeout cancels the work and leaves nothing parked when its deadline fires",
          "[TuiRuntime][timeout][fd]")
{
    auto const pipes = openPipes(2);
    REQUIRE(pipes.size() == 2);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto runtime = TuiRuntime { loop, source };

    // The work parks on a pipe nothing will ever write to, so only the timeout arm can win.
    auto result = runtime.blockOn(
        core::net::withTimeout(&runtime.loop(), parkOnHandleForever(&runtime, pipes[1]->waitHandle()), 20ms));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(loop.pendingTimerCount() == 0);
    // Only the runtime's own input flow is still parked; the cancelled work took its park with it.
    REQUIRE(loop.parkedWaiterCount() == 1);
}

// ---------------------------------------------------------------------------------------------
// The four deferred defects, each asserted on what distinguishes it.
// ---------------------------------------------------------------------------------------------

TEST_CASE("core-cpp#16: a cancelled delay leaves nothing behind for a later turn to touch",
          "[TuiRuntime][regression]")
{
    // The old runtime's DelayAwaiter reset its stop-callback and left its entry in the timer heap.
    // Crossing that deadline after the loser's frame was destroyed called done() on freed storage
    // -- a SIGSEGV in a plain debug build, not a subtle wrong answer. The loop's own awaitable
    // unregisters its park on every resume, ready or cancelled, which is what this asserts.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto runtime = TuiRuntime { loop, source };

    auto loserCancelled = false;
    REQUIRE(runtime.blockOn(raceOnDelayThenOutliveIt(&runtime, &loserCancelled)) == 1);
    REQUIRE(loserCancelled);
    REQUIRE(loop.pendingTimerCount() == 0);
    REQUIRE(loop.pendingTimerSlotCount() == 0);
}

TEST_CASE("core-cpp#17: a turn with nothing ready waits rather than spinning", "[TuiRuntime][regression]")
{
    // The old runtime's pumpOnce returned without waiting whenever nothing it knew about was
    // parked, and blockOn looped on it unconditionally, so a flow waiting on a source that
    // registered no fd burned a core in silence. An outcome test cannot tell a loop that slept
    // from one that spun -- both deliver the key -- so the assertion is on the ARGUMENT the first
    // wait was given: indefinite, because nothing bounds it.
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend };
    auto source = ScriptedInputSource { nullptr, nullptr, HandleFor::Input };
    REQUIRE(source.inputHandle() != core::platform::InvalidHandle); // the premise, now checkable
    source.pushEvents({ InputEvent { keyOf(U'q') } });
    auto runtime = TuiRuntime { loop, source };

    backend.pushTimeout();                 // the idle wait, whose argument is the assertion
    backend.pushReadable(HandlerId { 1 }); // then the input flow's registration becomes ready

    REQUIRE(runtime.blockOn(awaitOneKeyCodepoint(&runtime)) == U'q');

    // THREE waits for two scripted steps, and the extra one is not slack: the runtime is
    // constructed outside a turn, so its `submit` of the input flow goes through the inbound queue
    // and wakes the backend, and the first wait consumes that wake without spending a step.
    REQUIRE(backend.waitCount() == 3);
    // Every one of them was INDEFINITE. That is the whole assertion: with no deadline pending
    // there is nothing to bound a wait, so a loop that meant to sleep asks for no timeout, and one
    // that was about to spin asks for zero.
    for (auto const& timeout: backend.recordedTimeouts())
        CHECK_FALSE(timeout.has_value());
}

TEST_CASE("core-cpp#18: a whenAny loser parked on input unwinds without waiting for input",
          "[TuiRuntime][regression]")
{
    // The old input awaiters armed no stop-callback, so a cancelled one stayed in the runtime's
    // single waiter slot until input happened to arrive -- which, after EOF on stdin, is never.
    // The race could then not resolve at all, and the next flow to ask for input found the slot
    // taken. So the ARGUMENT again: this race must need no wait whatsoever.
    auto backend = ScriptedBackend {};
    auto loop = EventLoop { backend };
    auto source = ScriptedInputSource {}; // nothing scripted: no input will ever arrive
    auto runtime = TuiRuntime { loop, source };

    auto loserCancelled = false;
    REQUIRE(runtime.blockOn(raceOnInput(&runtime, &loserCancelled)) == 1);
    REQUIRE(loserCancelled);
    REQUIRE(backend.waitCount() == 0);
}

TEST_CASE("core-cpp#18: the input slot is free again for the next flow", "[TuiRuntime][regression]")
{
    // The other half of the same defect: having released the slot, the runtime must serve the
    // flow that asks next. A runtime that left it occupied trips its own single-waiter assertion
    // in a debug build and silently strands the first flow in a release one.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    source.pushEvents({ InputEvent { keyOf(U'k') } });
    auto runtime = TuiRuntime { loop, source };

    auto loserCancelled = false;
    auto raceThenRead = [](TuiRuntime* rt, bool* cancelled) -> Task<char32_t> {
        static_cast<void>(co_await raceOnInput(rt, cancelled));
        auto const event = co_await rt->nextEvent();
        co_return std::get<KeyEvent>(event).codepoint;
    };

    REQUIRE(runtime.blockOn(raceThenRead(&runtime, &loserCancelled)) == U'k');
    REQUIRE(loserCancelled);
}

// ---------------------------------------------------------------------------------------------
// Teardown.
// ---------------------------------------------------------------------------------------------

TEST_CASE("A runtime destroyed before its first turn leaves the loop holding nothing",
          "[TuiRuntime][teardown]")
{
    // **A source flow has three states at teardown, not two**, and this is the third: SUBMITTED
    // AND NEVER STARTED. `core::async::Task` is lazy, so between the constructor and the first
    // turn every source flow sits at its initial suspend point with its body not yet entered.
    //
    // Every other case in this file calls `blockOn` or `runOnce` before the runtime dies, so every
    // other case observes a flow that has at least reached its first park. That is why six
    // configurations and both sanitizers were green over a destructor that mishandled this one:
    // the state was unreachable from the suite, not absent from the program. An error-return path
    // between construction and the first turn reaches it, and so does constructing and destroying
    // a runtime inside a single turn.
    //
    // **All FOUR source flows, not the input one alone.** The fix is a `while (!_stopping)` guard
    // repeated in each flow, so a case over one of them proves the guard is there in one place and
    // infers the other three from their resemblance -- which is how the state that started this
    // finding was missed in the first place. The runtime below is configured so that
    // `startSourceFlows` starts every flow it knows how to start.
    auto const pipes = openPipes(3); // input, resize, and a stand-in for the signal fd
    REQUIRE(pipes.size() == 3);
    auto wakeup = core::platform::Wakeup {};
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get(), pipes[1].get() };
    auto const options = TuiRuntimeOptions { .interruptWakeup = &wakeup, .signalFd = pipes[2]->waitHandle() };

    // The premise, made checkable, because "the loop holds nothing" is ALSO what a runtime that
    // started no flow at all would leave behind: this configuration really does start four, and
    // each of them parks on its own handle.
    {
        auto runtime = TuiRuntime { loop, source, options };
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
        REQUIRE(loop.parkedWaiterCount() == 4);
    }
    REQUIRE(loop.parkedWaiterCount() == 0);

    // And now the state this case exists for: the same four, destroyed with no turn at all.
    {
        auto runtime = TuiRuntime { loop, source, options };
        // Deliberately no turn of any kind.
    }

    // Nothing may be left naming a runtime that is gone. A park here would be worse than a leak:
    // its handler points into a coroutine frame that `_sources` has just destroyed, so the next
    // readiness on that handle -- or `~EventLoop` -- resumes freed storage.
    REQUIRE(loop.parkedWaiterCount() == 0);
    REQUIRE(loop.readyCount() == 0);
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("A runtime destroyed while a source flow is queued after readiness leaves nothing behind",
          "[TuiRuntime][teardown]")
{
    // The third teardown state, and the one core-cpp#41 was about: the input handle has become
    // readable and the loop has queued the flow, but no turn has drained it, so `await_resume` --
    // which unregisters the park -- has not run. The destructor takes the flow back with
    // `cancelPending` and destroys it WITHOUT resuming it, so nothing but `cancelPending` itself
    // can have brought the park down.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    {
        auto runtime = TuiRuntime { loop, source };
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
        REQUIRE(loop.parkedWaiterCount() == 1);

        source.pushEvents({ InputEvent { keyOf(U'q') } });
        // Bounded: the readiness may be delivered from another thread on some backends, so a turn
        // can come back before it has arrived. The ready queue is what is waited for.
        constexpr auto MaxTurns = 500;
        auto turns = 0;
        while (loop.readyCount() == 0 && turns < MaxTurns)
        {
            std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
            ++turns;
        }
        INFO("waited " << turns << " turns for the input's readiness to reach the ready queue");
        REQUIRE(loop.readyCount() == 1);
        REQUIRE(loop.parkedWaiterCount() == 1); // queued, and its park still filed and attached
    }

    REQUIRE(loop.parkedWaiterCount() == 0);
    REQUIRE(loop.readyCount() == 0);

    // The input is still readable. A registration left behind would dispatch into the destroyed
    // frame here, which is what AddressSanitizer is for.
    std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
    CHECK(loop.readyCount() == 0);
}

TEST_CASE("Destroying the runtime leaves the loop holding nothing of it", "[TuiRuntime][teardown]")
{
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    {
        auto runtime = TuiRuntime { loop, source };
        // One bounded turn, so the input flow reaches its park. `runOnce` with a zero bound never
        // waits, which is what makes this safe to do with nothing ready.
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
        REQUIRE(loop.parkedWaiterCount() == 1);
    } // ~TuiRuntime: the source flow is taken back off the loop and finished

    REQUIRE(loop.parkedWaiterCount() == 0);
    REQUIRE(loop.pendingTimerCount() == 0);
    REQUIRE(loop.readyCount() == 0);
}

TEST_CASE("Destroying the runtime with input already dispatched holds nothing either",
          "[TuiRuntime][teardown]")
{
    // The other teardown case, and the one that is easy to get wrong: a source flow the loop has
    // already QUEUED still owns a live backend registration, because only its await_resume
    // unregisters. A destructor that merely dropped the frame would leave the loop watching a
    // terminal handle for an object that no longer exists.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    {
        auto runtime = TuiRuntime { loop, source };
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero()); // the flow parks
        source.pushEvents({ InputEvent { keyOf(U'd') } });
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero()); // readiness dispatched
    }

    REQUIRE(loop.parkedWaiterCount() == 0);
    REQUIRE(loop.pendingTimerCount() == 0);
}

TEST_CASE("A flow parked on the runtime unwinds while the runtime is still alive", "[TuiRuntime][teardown]")
{
    // **The timing is the assertion.** A flow awaiting this runtime holds an awaiter that reads it
    // on resume -- the slot, the deadline, the buffer -- and a cancellation callback that names
    // it, registered on a token the LOOP owns. So the unwind cannot be left to ~EventLoop: by then
    // the runtime is gone, and the loop's own request_stop() calls that callback against storage
    // that no longer exists. AddressSanitizer reported exactly that, as a stack-use-after-scope
    // read of the runtime from one scope further out, and a plain debug build did not notice.
    //
    // So the guard has to run at ~TuiRuntime, not merely by the end of the block: waiting for the
    // loop would be the defect, and "it unwound eventually" cannot tell the two apart.
    auto const pipes = openPipes(1);
    REQUIRE(pipes.size() == 1);
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipes[0].get() };
    auto destroyed = false;
    {
        auto runtime = TuiRuntime { loop, source };
        runtime.spawn(parkForeverWithGuard(&runtime, &destroyed));
        runtime.blockOn(awaitZeroDelay(&runtime)); // drive the spawned flow to its park
        REQUIRE_FALSE(destroyed);                  // still parked on input
    } // ~TuiRuntime: it resumes what it parked, so the frame unwinds and the guard runs

    REQUIRE(destroyed);
    // And nothing of it is left on the loop, which outlives it here exactly as it does in a
    // program: the parked flow was taken back, not abandoned to the loop's own teardown.
    REQUIRE(loop.parkedWaiterCount() == 0);
    REQUIRE(loop.readyCount() == 0);
}
