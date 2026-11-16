// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TuiRuntime` — the TUI's input semantics, composed on @c core::net::EventLoop.
///
/// **It is no longer a scheduler.** It used to be the second one in this project: its own ready
/// queue, its own timer heap, its own park slots, its own `pumpOnce`, driving its own blocking
/// `EventSource`. All of that is @c core::net::EventLoop's, and every one of this class's
/// scheduling members now forwards to it — `blockOn`, `spawn`, `delay`, `sleepUntil`,
/// `waitReadable`, `waitWritable`, the clock and the root stop source. Two schedulers meant two
/// answers to every question a scheduler answers, and the four defects this task closes
/// ([core-cpp#16](https://github.com/contour-terminal/core-cpp/issues/16) through
/// [#19](https://github.com/contour-terminal/core-cpp/issues/19)) were all in the second one's
/// answers.
///
/// What is genuinely the TUI's, and all that is left here, is **input semantics**: a decoded-event
/// buffer, the "next event / next event or timeout / next activity / next agent message" vocabulary
/// a terminal program is written in, and the interrupt policy. Those are not readiness questions,
/// so the loop has nothing to say about them.
///
/// **Its sources are parked flows, not entries in a hand-built wait set.** One flow per handle —
/// terminal input, resize, the interrupt wakeup, the POSIX signal fd — each parked on
/// `loop.waitReadable()` and each decoding only what it is for. That is what replaces the two
/// platform bodies of `TerminalEventSource` and the `#ifdef` in `PollEventSource`, and it is why
/// the Windows 64-handle cliff (core-cpp#19) is gone: the loop's Windows backend chunks its wait
/// set, and every backend does it the same way.
///
/// The agent-message wakeup is not one of them and has no handle at all. A worker with a message
/// calls `loop.post([&]{ runtime.notifyAgentReady(); })`, which is the loop's own cross-thread
/// surface — so the TUI no longer carries a wakeup object, a handle and a wait-set slot to say
/// what a post already says.
///
/// Threading: every member here is the loop thread's, exactly as the loop's own are. The one
/// cross-thread surface is @c core::net::EventLoop::post.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>
#include <core/platform/Wakeup.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/runtime/InputSource.hpp>
#include <core/tui/runtime/TerminalInputSource.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace core::tui::runtime
{

class NextInputEventAwaiter;
class NextEventForAwaiter;
class NextActivityAwaiter;
class NextAgentReadyAwaiter;

/// What a `nextActivity()` wait observed first.
enum class ActivityKind : std::uint8_t
{
    Event,      ///< An input event is available.
    AgentReady, ///< The agent-message wakeup fired; drain the agent channel.
    Timeout,    ///< The wait elapsed with nothing to do (run idle ticks).
};

/// The result of a `nextActivity()` wait: the kind plus the event (if Event).
struct Activity
{
    ActivityKind kind = ActivityKind::Timeout; ///< Which source resolved the wait.
    std::optional<InputEvent> event;           ///< The input event, set iff kind == Event.
};

/// What a parked input waiter may be resumed for.
///
/// An `enum class` rather than the `bool wantsAgent` this replaces, because the question is not
/// binary and the third case is the one that was wrong: a plain `nextEvent()` awaiter has no way
/// to return "nothing happened" — its `await_resume` can only produce an event or throw — so
/// resuming it for a focus change made a focus change indistinguishable from a cancellation, and
/// `runModal` closed the modal. Only a waiter that can SAY nothing happened may be told so.
enum class InputWake : std::uint8_t
{
    EventOnly, ///< Resume for an input event alone (nextEvent).
    OrNothing, ///< Resume for an event, an elapsed deadline, or non-input activity (nextEventFor).
    OrAgent,   ///< OrNothing, and for a pending agent message too (nextActivity).
};

/// The runtime's configuration, fixed at construction.
struct TuiRuntimeOptions
{
    /// The wakeup @c core::platform::SignalHandler signals on SIGINT, or null for a runtime that
    /// observes no interrupts. Borrowed: it must outlive the runtime.
    ///
    /// **The interrupt path files nothing on the loop.** The signal handler signals this wakeup,
    /// the loop is already watching its handle, and the flow parked there is what runs the
    /// interrupt policy. So there is no `wake()` here and none is missing: waking is for work
    /// handed to the loop, and this hands it none.
    platform::Wakeup* interruptWakeup = nullptr;

    /// The POSIX signal fd to watch -- @c core::platform::SignalHandler::nativeHandle, which is
    /// already this type -- or @c platform::InvalidHandle. Windows has none, and passing none is
    /// how that is said.
    platform::NativeHandle signalFd = platform::InvalidHandle;

    /// How long a partial escape sequence may sit unfinished before it is flushed as what it is.
    ///
    /// A lone ESC is a prefix of every arrow key, so a decoder cannot tell "the user pressed
    /// Escape" from "the rest is still in flight" without a clock. The runtime arms this after any
    /// read that decoded to nothing, and flushes when it elapses.
    std::chrono::milliseconds escapeFlush { 50 };

    /// How often the input is asked whether it has ended where no readiness reports it -- a
    /// Windows console whose host died (@c InputSource::probeEnd) -- or zero to never ask.
    ///
    /// Asked once in the runtime's first turn; a source answering @c EndProbe::NotNeeded is never
    /// asked again and costs no timer, so only an input that needs the question pays for it. The
    /// interval is how long a process may outlive such a console before its input ends.
    std::chrono::milliseconds endProbe { 1000 };
};

/// Drives TUI coroutine flows on a @c core::net::EventLoop.
///
/// Construct with a loop and an input source (or a @c Terminal, which is adapted to one), `spawn`
/// background flows and/or `blockOn` a root flow.
///
/// **Destroy it before its loop, on the loop's thread.** Its source flows are parked on the loop
/// and their frames name this object, so a readiness dispatched after it is gone would resume one
/// into freed storage. The destructor takes every one of them back off the loop, which it can only
/// do while the loop is still there — the obligation half of @c core::net::EventLoop's own rule
/// that objects registered with a loop are destroyed before it.
class TuiRuntime
{
  public:
    /// @param loop The event loop this runtime's flows and sources run on (not owned; outlives
    ///        this runtime).
    /// @param input Where input comes from (not owned; outlives this runtime). Its handles are
    ///        read ONCE, here, so whatever opens them has already done so.
    /// @param options The runtime's configuration; see @c TuiRuntimeOptions.
    TuiRuntime(net::EventLoop& loop, InputSource& input, TuiRuntimeOptions options = {});

    /// Convenience: runs over @p terminal's input, through an owned @c TerminalInputSource.
    /// @param loop The event loop (not owned; outlives this runtime).
    /// @param terminal The terminal to read (not owned; outlives this runtime). **Initialized
    ///        already**, for the reason @c TerminalInputSource states.
    /// @param options The runtime's configuration.
    TuiRuntime(net::EventLoop& loop, Terminal& terminal, TuiRuntimeOptions options = {});

    TuiRuntime(TuiRuntime const&) = delete;
    TuiRuntime& operator=(TuiRuntime const&) = delete;
    TuiRuntime(TuiRuntime&&) = delete;
    TuiRuntime& operator=(TuiRuntime&&) = delete;

    /// Retires every source flow, so nothing the loop holds names this object any more.
    ~TuiRuntime();

    /// @name The scheduler, which is the loop's
    /// @{

    /// @return The loop this runtime is composed on. What `core::net::withTimeout(&rt.loop(), …)`
    ///         and anything else taking an executor is handed.
    [[nodiscard]] net::EventLoop& loop() const noexcept { return _loop; }

    /// Drives the loop until @p task completes, then returns its result.
    /// @param task The root flow to run.
    /// @return The value produced by @p task (or void).
    template <typename T>
    T blockOn(async::Task<T> task)
    {
        return _loop.blockOn(std::move(task));
    }

    /// Starts a background flow that runs alongside the root flow. Loop thread only.
    /// @param task The flow to run.
    void spawn(async::Task<void> task) { _loop.spawn(std::move(task)); }

    /// @return The root cancellation source; `request_stop()` cancels every flow. Prefer
    ///         @c core::net::EventLoop::requestStop, which also unparks what it cancels.
    [[nodiscard]] async::StopSource& rootStopSource() noexcept { return _loop.rootStopSource(); }

    /// @return The monotonic clock backing every deadline.
    [[nodiscard]] platform::IClock& clock() const noexcept { return _loop.clock(); }

    /// @param duration How long to suspend.
    /// @return An awaitable that resumes after @p duration elapses.
    [[nodiscard]] net::DelayAwaiter delay(std::chrono::milliseconds duration) const noexcept
    {
        return _loop.delay(duration);
    }

    /// @param deadline The absolute instant (on this runtime's clock) to resume at.
    /// @return An awaitable that resumes once the clock reaches @p deadline.
    [[nodiscard]] net::DelayAwaiter sleepUntil(platform::SteadyTimePoint deadline) const noexcept
    {
        return _loop.sleepUntil(deadline);
    }

    /// Suspends until @p handle is readable, without consuming any bytes.
    /// @param handle The native handle to wait on (must outlive the await).
    /// @param kind What @p handle is; a Windows SOCKET and a waitable HANDLE reach different wait
    ///        primitives and the backend cannot tell them apart from the value.
    /// @return An awaitable resolving when @p handle is readable. It throws
    ///         @c core::async::OperationCancelled if the flow is cancelled while parked, and
    ///         @c core::net::FdRegistrationFailed — which the runtime's own awaitable used to
    ///         flatten into a cancellation — if the backend refused the handle.
    [[nodiscard]] net::WaitHandleAwaiter waitReadable(
        platform::NativeHandle handle, net::HandleKind kind = net::DefaultHandleKind) const noexcept
    {
        return _loop.waitReadable(handle, kind);
    }

    /// Suspends until @p handle is writable.
    /// @param handle The native handle to wait on (must outlive the await).
    /// @param kind What @p handle is; see @c waitReadable.
    /// @return An awaitable resolving when @p handle is writable.
    [[nodiscard]] net::WaitHandleAwaiter waitWritable(
        platform::NativeHandle handle, net::HandleKind kind = net::DefaultHandleKind) const noexcept
    {
        return _loop.waitWritable(handle, kind);
    }

    /// @}

    /// @name Input
    /// @{

    /// @return An awaitable yielding the next input event. It throws
    ///         @c core::async::OperationCancelled if the flow is cancelled while parked, or once
    ///         the input has ended (@c inputClosed) and nothing is buffered, and resumes for
    ///         nothing else — a focus change or an elapsed deadline cannot reach a waiter that has
    ///         no way to report them.
    [[nodiscard]] NextInputEventAwaiter nextEvent() noexcept;

    /// @param timeout How long to wait for an event before giving up.
    /// @return An awaitable yielding the next input event, or std::nullopt if @p timeout elapses
    ///         or non-input activity occurs (so the caller can run idle ticks). Once the input has
    ///         ended (@c inputClosed) and nothing is buffered, it throws
    ///         @c core::async::OperationCancelled instead of waiting.
    [[nodiscard]] NextEventForAwaiter nextEventFor(std::chrono::milliseconds timeout) noexcept;

    /// Waits for input, an agent message, or a timeout — whichever happens first.
    /// @param timeout How long to wait before reporting ActivityKind::Timeout.
    /// @return An awaitable yielding the first activity observed. Once the input has ended
    ///         (@c inputClosed), buffered events and a pending agent message are still reported,
    ///         and then it throws @c core::async::OperationCancelled instead of waiting.
    [[nodiscard]] NextActivityAwaiter nextActivity(std::chrono::milliseconds timeout) noexcept;

    /// @return An awaitable that resumes when an agent message is pending.
    [[nodiscard]] NextAgentReadyAwaiter nextAgentReady() noexcept;

    /// Whether the terminal's input has ended: its handle hung up, closed, or could no longer be
    /// watched ([core-cpp#49](https://github.com/contour-terminal/core-cpp/issues/49)).
    ///
    /// Once it has, the runtime has stopped watching the handle, and every input wait --
    /// @c nextEvent, @c nextEventFor and @c nextActivity -- still delivers what was read before
    /// the end and then throws @c core::async::OperationCancelled, without parking, for as long as
    /// the runtime lives; @c nextActivity reports a pending agent message first. This is what an
    /// application asks after such a cancellation to tell "the terminal is gone, exit" from an
    /// interrupt or a cancelled flow. A loop that catches the cancellation and waits again without
    /// asking is a loop that never waits.
    /// @return true once the input stream has ended.
    [[nodiscard]] bool inputClosed() const noexcept { return _inputClosed; }

    /// Records that the agent worker has a message, and resumes whoever is waiting for one.
    ///
    /// **Loop thread only.** From the worker's own thread this is
    /// `loop.post([&]{ runtime.notifyAgentReady(); })`, which is what replaced the wakeup handle
    /// this runtime used to keep in its wait set.
    void notifyAgentReady();

    /// Sets the policy run when an interrupt (SIGINT / Ctrl+C) is observed. The default requests
    /// stop on the loop, which cancels every flow and unparks what it cancelled.
    ///
    /// **The handler must not destroy this runtime.** It runs inline, inside the source flow that
    /// observed the interrupt, and that flow resumes the moment the handler returns -- into a frame
    /// and an object that would both be gone. `post()` the teardown to the loop instead: by the
    /// time it runs, the flow is parked again and the destructor can take it back. Asserted in a
    /// debug build.
    /// @param handler The interrupt policy, or `nullptr` to restore the default.
    void setInterruptHandler(std::function<void()> handler) { _onInterrupt = std::move(handler); }

    /// @}

    /// @name Awaiter-facing primitives (internal)
    /// Called by this file's awaitables; not part of the consumer API.
    /// @{

    /// @return Whether this runtime is being torn down, so every wait on it is cancelled.
    ///
    /// The awaiters consult it in BOTH directions -- `await_suspend` declines to park and
    /// `await_resume` throws -- and that pair is what makes the destructor's unwind converge: a
    /// flow resumed there cannot re-enter a slot that is going away, however its body reacts.
    [[nodiscard]] bool isStopping() const noexcept { return _stopping; }

    /// @return Whether an input event is available now. Not `const`: a terminal query hands back
    ///         what it read but did not want, and this is the moment that matters, so the
    ///         handed-back events are taken here rather than polled for.
    [[nodiscard]] bool hasBufferedInput();

    /// @return The next buffered input event.
    /// @pre @c hasBufferedInput() answered true.
    [[nodiscard]] InputEvent popBufferedInput();

    /// @return Whether an agent message is pending. Meaningful only to an agent-interested
    ///         waiter; a plain input waiter never consumes it.
    [[nodiscard]] bool agentPending() const noexcept { return _agentPending; }

    /// Clears the pending agent message. Called only by an agent-interested waiter.
    void consumeAgentPending() noexcept { _agentPending = false; }

    /// Parks @p waiter for input.
    /// @param waiter The coroutine to resume.
    /// @param wake What it may be resumed for.
    /// @param deadline When to resume it with nothing, or nullopt to wait indefinitely.
    /// @pre No input waiter is parked. One flow reads input at a time; two would clobber this slot
    ///      and strand the first. (A modal nests by suspending its parent, so the parent's awaiter
    ///      is not parked while the child's is.)
    void parkOnInput(std::coroutine_handle<> waiter,
                     InputWake wake,
                     std::optional<platform::SteadyTimePoint> deadline);

    /// Releases @p waiter's input park: clears the slot if it still holds it, and retires its
    /// deadline. Idempotent; called by the awaiter on every resume, ready or cancelled.
    /// @param waiter The coroutine that was parked.
    void releaseInputWaiter(std::coroutine_handle<> waiter) noexcept;

    /// Parks @p waiter for the next agent message.
    /// @param waiter The coroutine to resume.
    /// @pre No agent waiter is parked (the single-waiter invariant again).
    void parkOnAgent(std::coroutine_handle<> waiter);

    /// Releases @p waiter's agent park. Idempotent.
    /// @param waiter The coroutine that was parked.
    void releaseAgentWaiter(std::coroutine_handle<> waiter) noexcept;

    /// Forgets @p waiter, whose `await_resume` has now run. Called by every awaiter, through
    /// @c releaseInputWaiter and @c releaseAgentWaiter.
    /// @param waiter The coroutine that has resumed.
    void forgetHandedToLoop(std::coroutine_handle<> waiter) noexcept;

    /// Unparks @p waiter and queues it so it resumes and observes its own cancellation.
    ///
    /// **This is what core-cpp#18 was missing.** Without it a cancelled input awaiter stayed in
    /// its slot until input happened to arrive — which, after EOF on stdin, is never — so the
    /// race that cancelled it never resolved and the next flow to ask for input found the slot
    /// taken. The loop's own awaitables have had the equivalent since they existed; these are the
    /// two parks the loop does not hold, so they need their own.
    /// @param waiter The parked coroutine to resume for cancellation.
    void requestCancelWaiter(std::coroutine_handle<> waiter) noexcept;

    /// @}

  private:
    /// How many source flows a runtime can have: one per handle it knows about -- terminal input,
    /// resize, the interrupt wakeup and the POSIX signal fd -- and the end probe, which watches no
    /// handle. Another flow is this number plus one -- `startSourceFlow` asserts rather than overruns.
    static constexpr std::size_t SourceFlowCount = 5;

    /// Starts the source flows for whichever handles @c _input and @c _options name.
    void startSourceFlows();

    /// Submits @p flow to the loop and keeps its frame here. The frame is THIS object's, not the
    /// loop's: `spawn` would hand it over, and the loop has no way to give it back at teardown.
    /// @param flow The source flow to run.
    void startSourceFlow(async::Task<void> flow);

    /// The flow parked on terminal input: decodes what is ready and delivers it.
    [[nodiscard]] async::Task<void> inputFlow();

    /// The flow parked on the resize channel.
    [[nodiscard]] async::Task<void> resizeFlow();

    /// The flow parked on the interrupt wakeup: runs the interrupt policy for each SIGINT.
    [[nodiscard]] async::Task<void> interruptFlow();

    /// The flow parked on the POSIX signal fd: a reaped job is non-input activity.
    [[nodiscard]] async::Task<void> signalFlow();

    /// Asks the input whether it has ended where no readiness reports it (@c InputSource::probeEnd),
    /// now and then every @c TuiRuntimeOptions::endProbe while it answers @c EndProbe::Open. Ends the
    /// input stream once it answers @c EndProbe::Ended, and returns at once for a source that answers
    /// @c EndProbe::NotNeeded. A flow rather than a timer, so the first question is asked on the loop's
    /// thread in a turn, not by the constructor, and what the source throws propagates as any flow's
    /// exception does.
    [[nodiscard]] async::Task<void> endProbeFlow();

    /// Files @p events: dispatches the terminal's protocol responses out of them, buffers what is
    /// left as application input, and resumes whoever was waiting.
    /// @param events The decoded events (consumed).
    void routeDecoded(std::vector<InputEvent> events);

    /// Records that the input has ended and resumes the input waiter, if one is parked, so that it
    /// takes what is buffered or ends in a cancellation.
    void endInput();

    /// Queues @p waiter for resumption on the loop, and remembers that it is out there.
    ///
    /// **The remembering is not bookkeeping for its own sake.** Both of the things that happen to
    /// a queued waiter next read THIS runtime: the awaiter's `await_resume`, which asks for the
    /// slot, the deadline and the buffer, and its cancellation callback, which names this object.
    /// A waiter left on the loop's queue when this runtime dies is resumed into freed storage, and
    /// a cancellation callback left armed is called there -- which is exactly what `~EventLoop`
    /// does to every token it still holds.
    /// @param waiter The coroutine to resume.
    void handToLoop(std::coroutine_handle<> waiter);

    /// Unwinds everything this runtime parked, from the destructor, while it is still alive.
    void unwindParkedWaiters();

    /// Resumes a parked input waiter if an event is now available.
    void deliverInput();

    /// Resumes a parked input waiter that can report "nothing happened" — a focus change, a
    /// reaped job, an elapsed deadline. A @c InputWake::EventOnly waiter is left alone.
    void notifyActivity();

    /// Takes the input waiter out of its slot, retiring its deadline and escape flush.
    /// @return The coroutine that was parked, or an empty handle.
    [[nodiscard]] std::coroutine_handle<> takeInputWaiter() noexcept;

    /// Arms the escape-sequence flush, so a lone ESC is delivered rather than held. Idempotent:
    /// re-arming an armed flush leaves the earlier deadline, which is the one that matters.
    void armEscapeFlush();

    /// Retires @p timer if it is armed, and clears it.
    /// @param timer The timer to retire.
    void retireTimer(net::TimerId& timer) noexcept;

    /// Runs the interrupt policy: the installed handler, or a stop request on the loop.
    void runInterruptPolicy();

    /// The deadline of a timed input wait. @param state The runtime, as a `void*`.
    static void onInputDeadline(void* state) noexcept;

    /// The escape-sequence flush deadline. @param state The runtime, as a `void*`.
    static void onEscapeFlush(void* state) noexcept;

    net::EventLoop& _loop; ///< The scheduler. Every scheduling member here forwards to it.

    /// The adapter over a @c Terminal, for the convenience constructor; empty otherwise.
    ///
    /// **Declared before @c _input, which the convenience constructor binds to it.** Members are
    /// initialized in declaration order, so the other order would bind a reference to an optional
    /// that has not been engaged yet — and bind it successfully, because taking the address of
    /// storage that is about to hold the object looks like nothing at all.
    std::optional<TerminalInputSource> _ownedInput;

    InputSource& _input;        ///< Where input comes from.
    TuiRuntimeOptions _options; ///< Fixed at construction.

    platform::NativeHandle _inputHandle;  ///< Read once from @c _input, at construction.
    platform::NativeHandle _resizeHandle; ///< Read once from @c _input, at construction.

    /// The source flows' frames, owned HERE. Retired in the destructor, one by one.
    std::array<async::Task<void>, SourceFlowCount> _sources;
    std::size_t _sourceCount = 0; ///< How many of @c _sources were started.

    std::deque<InputEvent> _inputBuffer;         ///< Decoded input awaiting a consumer.
    std::coroutine_handle<> _inputWaiter;        ///< The one flow parked on input, if any.
    InputWake _inputWake = InputWake::EventOnly; ///< What that flow may be resumed for.
    net::TimerId _inputDeadline {};              ///< Its timed wait's deadline, if it has one.
    net::TimerId _escapeFlush {};                ///< The pending partial-escape flush, if any.

    std::coroutine_handle<> _agentWaiter; ///< The one flow parked on nextAgentReady(), if any.
    bool _agentPending = false;           ///< A message is waiting; only an agent waiter takes it.

    /// Waiters queued on the loop whose `await_resume` has not run yet. Holds zero or one entry
    /// almost always -- a waiter is normally resumed by the same drain that queued it -- but a
    /// second flow may park while the first is still queued, so it is a list rather than a slot.
    std::vector<std::coroutine_handle<>> _handedToLoop;

    std::function<void()> _onInterrupt; ///< Interrupt policy; the default stops the loop.

    /// Set by the destructor, read by every source flow after it resumes. A flow taken back off
    /// the loop is resumed one last time so its `await_resume` can unregister its park, and this
    /// is what stops that resumption from re-entering the body of a runtime that is going away.
    bool _stopping = false;

    /// Set once the input source reported its end, or the loop refused its handle. From then on
    /// the input flow has returned, and the input awaiters decline to park.
    bool _inputClosed = false;

    /// Whether this runtime is currently running code it does not own -- the interrupt handler, or
    /// an @c InputSource member. Read only by the destructor's precondition; see @c CalloutScope.
    bool _inCallout = false;

    /// Marks the window in which this runtime has handed control to code it does not own.
    ///
    /// **It exists for one assertion, and that assertion is a precondition the destructor cannot
    /// serve.** A source flow calls out to the consumer inline -- `_onInterrupt()`, and every
    /// @c InputSource member -- so a consumer that owns its runtime can reach `~TuiRuntime` from
    /// inside a flow that is still executing. That is a FOURTH state a source flow can be in at
    /// teardown, RUNNING, and unlike the other three it has no right answer: `cancelPending`
    /// correctly declines it (the loop is not holding a running frame), and then `~Task` destroys
    /// a frame whose body resumes the moment the call returns. Nothing the destructor could do
    /// helps, because the object the resumed body reads is gone either way. Destroy the runtime
    /// after the call returns -- `loop.post(...)` is the usual way -- and the flow is parked by
    /// then, which is state 2.
    ///
    /// It restores the previous value rather than clearing it, so a callout that nests another --
    /// an @c InputSource reached from inside the interrupt handler -- does not end the outer one.
    class CalloutScope
    {
      public:
        /// @param runtime The runtime handing control out.
        explicit CalloutScope(TuiRuntime& runtime) noexcept:
            _runtime(runtime), _outer(std::exchange(runtime._inCallout, true))
        {
        }
        CalloutScope(CalloutScope const&) = delete;
        CalloutScope(CalloutScope&&) = delete;
        CalloutScope& operator=(CalloutScope const&) = delete;
        CalloutScope& operator=(CalloutScope&&) = delete;
        ~CalloutScope() { _runtime._inCallout = _outer; }

      private:
        TuiRuntime& _runtime;
        bool _outer;
    };

    /// Runs @p call -- code this runtime does not own -- inside a @c CalloutScope.
    /// @param call The callout.
    /// @return Whatever @p call returns.
    template <typename Call>
    decltype(auto) callOut(Call&& call)
    {
        auto const scope = CalloutScope { *this };
        return std::forward<Call>(call)();
    }
};

/// Awaitable yielding the next input event, or throwing @c core::async::OperationCancelled if the
/// awaiting flow is cancelled while parked.
class NextInputEventAwaiter
{
  public:
    /// @param runtime The runtime whose input this reads.
    explicit NextInputEventAwaiter(TuiRuntime& runtime) noexcept: _runtime(runtime) {}

    /// @return False. Whether an event is already buffered is @c await_suspend's question: the
    ///         answer is a call, and MSVC 19.44's ARM64 code generator drops the enclosing `try` of
    ///         a `co_await` on a temporary awaiter whose `await_ready` makes one
    ///         ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)).
    ///         The three awaiters below answer the same, for the same reason.
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// Asks for a buffered event BEFORE reading the token, which keeps the answer the one
    /// `await_ready` gave when it held the question: a buffered event is delivered even to a
    /// stopped flow. Not `noexcept`: asking is what takes the events a terminal query handed back,
    /// and taking them allocates.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) if an event is buffered or the flow is already cancelled, true
    ///         to park as the input waiter.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_runtime.hasBufferedInput())
            return false;
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested() || _runtime.isStopping() || _runtime.inputClosed())
            return false;
        _waiter = awaiting;
        _runtime.parkOnInput(awaiting, InputWake::EventOnly, std::nullopt);
        // **Armed AFTER the park, which is the opposite of what `.agent/rules/async-and-net.md`
        // prescribes, so here is the argument on that rule's own terms.** The rule guards one
        // hazard: a token that is ALREADY stopped runs the callback inside the `StopCallback`
        // constructor, on this thread, and if the park is already published that callback hands
        // the handle to an executor which may resume a coroutine whose `await_suspend` has not
        // returned.
        //
        // Neither half of that can happen here. The already-stopped case never reaches this
        // line: every awaiter in this file tests `_token.stop_requested()` and returns false
        // before it parks or emplaces, so the constructor cannot run the callback. And a stop
        // arriving later reaches `requestCancelWaiter`, which ENQUEUES through `handToLoop` ->
        // `resumeSoon` and never resumes inline, so no callback of ours can resume a frame
        // mid-`await_suspend` whatever the ordering. A cross-thread stop trips that function's
        // own thread assertion rather than racing this.
        //
        // What the order buys is the converse obligation: the park is a slot in this runtime
        // rather than a kernel registration, so a throwing emplace leaves nothing attached that
        // the destructor does not already clear. The three awaiters below take this order for
        // the same reasons.
        _cancelReg.emplace(_token,
                           [&runtime = _runtime, awaiting] { runtime.requestCancelWaiter(awaiting); });
        return true;
    }

    /// @return The next buffered input event.
    /// @throws core::async::OperationCancelled if the flow was cancelled while parked, or the
    ///         input has ended (@c TuiRuntime::inputClosed) with nothing buffered.
    [[nodiscard]] InputEvent await_resume()
    {
        _cancelReg.reset();
        _runtime.releaseInputWaiter(_waiter);
        if (_token.stop_requested() || _runtime.isStopping() || !_runtime.hasBufferedInput())
            throw async::OperationCancelled {};
        return _runtime.popBufferedInput();
    }

  private:
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    TuiRuntime& _runtime;
    std::coroutine_handle<> _waiter;
    async::StopToken _token;
};

/// Awaitable yielding the next input event, or std::nullopt if the wait elapses or non-input
/// activity occurs first. Throws @c core::async::OperationCancelled if cancelled while parked.
class NextEventForAwaiter
{
  public:
    /// @param runtime The runtime whose input this reads.
    /// @param deadline When to resume with nothing.
    NextEventForAwaiter(TuiRuntime& runtime, platform::SteadyTimePoint deadline) noexcept:
        _runtime(runtime), _deadline(deadline)
    {
    }

    /// @return False, for @c NextInputEventAwaiter::await_ready's reason.
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// Asks for a buffered event before reading the token, as @c NextInputEventAwaiter does.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) if an event is buffered or the flow is already cancelled, true
    ///         to park with a deadline.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_runtime.hasBufferedInput())
            return false;
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested() || _runtime.isStopping() || _runtime.inputClosed())
            return false;
        _waiter = awaiting;
        _runtime.parkOnInput(awaiting, InputWake::OrNothing, _deadline);
        _cancelReg.emplace(_token,
                           [&runtime = _runtime, awaiting] { runtime.requestCancelWaiter(awaiting); });
        return true;
    }

    /// @return The next buffered input event, or std::nullopt where the wait produced none.
    /// @throws core::async::OperationCancelled if the flow was cancelled while parked, or the
    ///         input has ended (@c TuiRuntime::inputClosed) with nothing buffered.
    [[nodiscard]] std::optional<InputEvent> await_resume()
    {
        _cancelReg.reset();
        _runtime.releaseInputWaiter(_waiter);
        if (_token.stop_requested() || _runtime.isStopping())
            throw async::OperationCancelled {};
        if (_runtime.hasBufferedInput())
            return _runtime.popBufferedInput();
        if (_runtime.inputClosed())
            throw async::OperationCancelled {};
        return std::nullopt;
    }

  private:
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    TuiRuntime& _runtime;
    platform::SteadyTimePoint _deadline;
    std::coroutine_handle<> _waiter;
    async::StopToken _token;
};

/// Awaitable that resumes on the first of: an input event, an agent message, or a timeout.
class NextActivityAwaiter
{
  public:
    /// @param runtime The runtime whose input and agent channel this reads.
    /// @param deadline When to resume reporting a timeout.
    NextActivityAwaiter(TuiRuntime& runtime, platform::SteadyTimePoint deadline) noexcept:
        _runtime(runtime), _deadline(deadline)
    {
    }

    /// @return False, for @c NextInputEventAwaiter::await_ready's reason.
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// Asks for a buffered event or a pending agent message before reading the token, as
    /// @c NextInputEventAwaiter does.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) if an event or an agent message is already there or the flow is
    ///         already cancelled, true to park (input + agent + deadline).
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_runtime.hasBufferedInput() || _runtime.agentPending())
            return false;
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested() || _runtime.isStopping() || _runtime.inputClosed())
            return false;
        _waiter = awaiting;
        _runtime.parkOnInput(awaiting, InputWake::OrAgent, _deadline);
        _cancelReg.emplace(_token,
                           [&runtime = _runtime, awaiting] { runtime.requestCancelWaiter(awaiting); });
        return true;
    }

    /// @return The first activity observed (event, agent-ready, or timeout).
    /// @throws core::async::OperationCancelled if the flow was cancelled while parked, or the
    ///         input has ended (@c TuiRuntime::inputClosed) with nothing buffered.
    [[nodiscard]] Activity await_resume()
    {
        _cancelReg.reset();
        _runtime.releaseInputWaiter(_waiter);
        if (_token.stop_requested() || _runtime.isStopping())
            throw async::OperationCancelled {};
        if (_runtime.hasBufferedInput())
            return Activity { .kind = ActivityKind::Event, .event = _runtime.popBufferedInput() };
        if (_runtime.agentPending())
        {
            _runtime.consumeAgentPending();
            return Activity { .kind = ActivityKind::AgentReady, .event = std::nullopt };
        }
        if (_runtime.inputClosed())
            throw async::OperationCancelled {};
        return Activity { .kind = ActivityKind::Timeout, .event = std::nullopt };
    }

  private:
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    TuiRuntime& _runtime;
    platform::SteadyTimePoint _deadline;
    std::coroutine_handle<> _waiter;
    async::StopToken _token;
};

/// Awaitable that resumes when an agent message is pending; the consumer then drains its own
/// typed message queue.
class NextAgentReadyAwaiter
{
  public:
    /// @param runtime The runtime whose agent channel this waits on.
    explicit NextAgentReadyAwaiter(TuiRuntime& runtime) noexcept: _runtime(runtime) {}

    /// @return False, for @c NextInputEventAwaiter::await_ready's reason.
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// Asks for a pending message before reading the token, as @c NextInputEventAwaiter does.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) if a message is already pending or the flow is already
    ///         cancelled, true to park as the agent waiter.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_runtime.agentPending())
            return false;
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested() || _runtime.isStopping())
            return false;
        _waiter = awaiting;
        _runtime.parkOnAgent(awaiting);
        _cancelReg.emplace(_token,
                           [&runtime = _runtime, awaiting] { runtime.requestCancelWaiter(awaiting); });
        return true;
    }

    /// @throws core::async::OperationCancelled if the flow was cancelled while parked.
    void await_resume()
    {
        _cancelReg.reset();
        _runtime.releaseAgentWaiter(_waiter);
        if (_token.stop_requested() || _runtime.isStopping())
            throw async::OperationCancelled {};
        _runtime.consumeAgentPending();
    }

  private:
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    TuiRuntime& _runtime;
    std::coroutine_handle<> _waiter;
    async::StopToken _token;
};

} // namespace core::tui::runtime
