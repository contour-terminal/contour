// SPDX-License-Identifier: Apache-2.0
#include <core/tui/runtime/TuiRuntime.hpp>

#include <core/platform/SignalHandler.hpp>

#include <algorithm>
#include <cassert>
#include <utility>

namespace core::tui::runtime
{

TuiRuntime::TuiRuntime(net::EventLoop& loop, InputSource& input, TuiRuntimeOptions options):
    _loop(loop),
    _input(input),
    _options(options),
    _inputHandle(input.inputHandle()),
    _resizeHandle(input.resizeHandle())
{
    startSourceFlows();
}

TuiRuntime::TuiRuntime(net::EventLoop& loop, Terminal& terminal, TuiRuntimeOptions options):
    _loop(loop),
    _ownedInput(std::in_place, terminal),
    _input(*_ownedInput),
    _options(options),
    _inputHandle(_ownedInput->inputHandle()),
    _resizeHandle(_ownedInput->resizeHandle())
{
    startSourceFlows();
}

TuiRuntime::~TuiRuntime()
{
    // **Nothing may outlive this object still naming it**, and two different populations do: the
    // flows this runtime parked in its own slots, and the source flows it started. Both are
    // retired here, while the loop -- and this object -- are still alive.
    assert(_loop.teardownIsSerialisedWithDispatch()
           && "~TuiRuntime from a second thread while another is driving its loop: this clears "
              "scheduler state beside a turn that is reading it");
    // The FOURTH state a source flow can be in, RUNNING, and the one that is a precondition rather
    // than a case to handle -- see state 4 below, and TuiRuntime::CalloutScope.
    assert(!_inCallout
           && "~TuiRuntime from inside code this runtime called -- an interrupt handler, or an "
              "InputSource member reached from a source flow. That flow is still executing and is "
              "about to have its frame destroyed under it; destroy the runtime after the call "
              "returns instead, e.g. by posting the teardown to the loop");
    _stopping = true;

    unwindParkedWaiters();
    retireTimer(_escapeFlush);

    // The source flows. Each frame names THIS object, and the loop is holding it, so a resumption
    // after this object is gone reaches freed storage. `cancelPending` is the retrieval, and its
    // answer is an ownership transfer rather than a status: true means the loop no longer has it
    // -- nothing of it is left queued, parked or registered with the backend -- and this call may
    // destroy it.
    //
    // **A flow is in one of FOUR states here**, enumerated rather than tested because earlier
    // versions of this comment named two and then three, and each missing one was a real state:
    //
    //   1. SUBMITTED, NEVER STARTED. `async::Task` is lazy, so between the constructor and the
    //      first turn the frame sits at its initial suspend point in the inbound queue.
    //   2. PARKED on a handle. `cancelPending` takes the park and detaches the registration.
    //   3. QUEUED after readiness: the waiter was dispatched into the ready queue, `await_resume`
    //      has not run, and so its park is still filed and attached. `cancelPending` takes the
    //      park with the frame -- it did not until
    //      [core-cpp#41](https://github.com/contour-terminal/core-cpp/issues/41) was fixed, and
    //      this loop resumed every flow once so that `await_resume` would do it instead.
    //   4. RUNNING: this destructor was reached from inside a callout of that very flow -- the
    //      interrupt handler, or an `InputSource` member. `cancelPending` correctly answers false,
    //      the loop is not holding a running frame, and destroying it would free a frame whose
    //      body resumes the moment the callout returns. It has no right answer, so it is refused
    //      by the assertion above rather than handled here.
    //
    // For the first three, one action: destroy the frame NOW, while every member it names is still
    // alive, rather than leave it to `_sources`' own destructor after some of them have gone. A
    // suspended frame's destruction runs no body code -- only the awaiter's destructor, which
    // drops its stop registration.
    for (auto& source: _sources)
    {
        if (source.done())
            continue;
        if (_loop.cancelPending(source.handle()))
            source = {};
    }
}

void TuiRuntime::unwindParkedWaiters()
{
    // A flow awaiting this runtime is unwound HERE, from the destructor body, while every member
    // it is about to read is still alive. Handing it to the loop instead would have it resumed
    // afterwards, and its `await_resume` asks this object for the slot, the deadline and the
    // buffer. Worse, an awaiter that is never resumed never runs `_cancelReg.reset()`, so its
    // cancellation callback -- which names this runtime -- stays armed on a token the LOOP owns,
    // and `~EventLoop`'s own `request_stop()` then calls it against storage that is gone. That is
    // the failure AddressSanitizer reported: a stack-use-after-scope read of the runtime, from the
    // loop's teardown, one scope further out.
    //
    // The waiters the loop is already holding come back first, by the same retrieval the source
    // flows use. `cancelPending` answering false would mean the loop no longer has it, which
    // cannot happen while it is still listed here: a waiter leaves that list only through
    // `await_resume`, and `await_resume` only runs from a resumption.
    for (auto const waiter: std::exchange(_handedToLoop, {}))
        if (waiter && !waiter.done() && _loop.cancelPending(waiter))
            waiter.resume();

    // Then the parked ones. This terminates in at most two passes whatever the resumed bodies do:
    // `_stopping` is set, so `await_suspend` declines to park and `await_resume` throws, and a
    // flow that catches the cancellation and awaits again cannot get back into a slot.
    while (_inputWaiter || _agentWaiter)
    {
        auto const waiter = _inputWaiter ? takeInputWaiter() : std::exchange(_agentWaiter, {});
        if (waiter && !waiter.done())
            waiter.resume();
    }
}

void TuiRuntime::handToLoop(std::coroutine_handle<> waiter)
{
    if (!waiter || waiter.done())
        return;
    _handedToLoop.push_back(waiter);
    _loop.resumeSoon(async::ParkedWork { .resume = waiter });
}

void TuiRuntime::forgetHandedToLoop(std::coroutine_handle<> waiter) noexcept
{
    std::erase(_handedToLoop, waiter);
}

void TuiRuntime::startSourceFlows()
{
    if (_inputHandle != platform::InvalidHandle)
    {
        startSourceFlow(inputFlow());
        // Beside the input flow rather than in it: that flow is parked on a handle that, for the
        // input this asks about, is never signalled again, so it would never get to ask.
        if (_options.endProbe > std::chrono::milliseconds::zero())
            startSourceFlow(endProbeFlow());
    }
    if (_resizeHandle != platform::InvalidHandle)
        startSourceFlow(resizeFlow());
    if (_options.interruptWakeup != nullptr)
        startSourceFlow(interruptFlow());
    if (_options.signalFd != platform::InvalidHandle)
        startSourceFlow(signalFlow());
}

void TuiRuntime::startSourceFlow(async::Task<void> flow)
{
    assert(_sourceCount < _sources.size() && "TuiRuntime: more source flows than slots for them");
    // `submit` rather than `spawn`, and the difference is the whole of the teardown above: `spawn`
    // hands the frame to the loop, which offers no way to take one back, so a runtime destroyed
    // before its loop would leave the loop owning a frame that names freed storage. `submit`
    // BORROWS -- the frame stays here, in `_sources`, and the destructor is what makes that
    // borrow honest.
    _sources[_sourceCount] = std::move(flow);
    _loop.submit(_sources[_sourceCount].handle());
    ++_sourceCount;
}

async::Task<void> TuiRuntime::inputFlow()
{
    // `!_stopping` rather than `true`, checked BEFORE the first `co_await`: a flow must never park
    // on the loop once teardown has begun, whatever resumed it. See ~TuiRuntime.
    while (!_stopping)
    {
        try
        {
            co_await _loop.waitReadable(_inputHandle);
        }
        catch (async::OperationCancelled const&)
        {
            co_return;
        }
        catch (net::FdRegistrationFailed const&)
        {
            // The backend will not watch this handle -- it is closed, or the kernel refused the
            // filter. Returning is the only option that does not spin: re-parking would ask the
            // same refused question on every turn, forever. And it is the end of the input as
            // much as a hangup is: without saying so, a flow parked on nextEvent() would wait for
            // input that can no longer arrive.
            endInput();
            co_return;
        }
        // An end the probe found while this flow was parked: nothing behind the handle is input
        // any more, and the stream has already ended.
        if (_stopping || _inputClosed)
            co_return;

        auto decoded = callOut([this] { return _input.readReady(); });
        if (callOut([this] { return _input.inputClosed(); }))
        {
            // The handle is at its end: a terminal that hung up, a pipe whose writer closed, a
            // console that went away. It stays readable for ever and yields nothing, so parking on
            // it again resumes at once, every turn -- 100% CPU, and with SIGHUP ignored nothing
            // else ends the process (core-cpp#49). Deliver what this read decoded and what a
            // partial sequence completes to, since no continuation is coming, then end.
            routeDecoded(std::move(decoded));
            routeDecoded(callOut([this] { return _input.flushPartial(); }));
            endInput();
            co_return;
        }
        if (decoded.empty())
            // Bytes arrived and decoded to nothing: either a partial escape sequence, or a console
            // record that is not input. Arm the flush so a lone ESC is eventually delivered as
            // Escape rather than waiting for a continuation nobody is going to type.
            armEscapeFlush();
        else
            retireTimer(_escapeFlush);
        routeDecoded(std::move(decoded));
    }
}

void TuiRuntime::endInput()
{
    _inputClosed = true;
    retireTimer(_escapeFlush);
    // Whatever the waiter may be resumed for, the end is news to it: an event it can take, or the
    // cancellation its await_resume now throws. Left parked, a nextEvent() would never resume.
    if (_inputWaiter)
        handToLoop(takeInputWaiter());
}

async::Task<void> TuiRuntime::resizeFlow()
{
    // `!_stopping` rather than `true`, checked BEFORE the first `co_await`: a flow must never park
    // on the loop once teardown has begun, whatever resumed it. See ~TuiRuntime.
    while (!_stopping)
    {
        try
        {
            co_await _loop.waitReadable(_resizeHandle);
        }
        catch (async::OperationCancelled const&)
        {
            co_return;
        }
        catch (net::FdRegistrationFailed const&)
        {
            co_return;
        }
        if (_stopping)
            co_return;

        if (auto resize = callOut([this] { return _input.readResize(); }))
        {
            auto events = std::vector<InputEvent> {};
            events.push_back(std::move(*resize));
            routeDecoded(std::move(events));
        }
    }
}

async::Task<void> TuiRuntime::interruptFlow()
{
    // `!_stopping` rather than `true`, checked BEFORE the first `co_await`: a flow must never park
    // on the loop once teardown has begun, whatever resumed it. See ~TuiRuntime.
    while (!_stopping)
    {
        try
        {
            co_await _loop.waitReadable(_options.interruptWakeup->nativeHandle());
        }
        catch (async::OperationCancelled const&)
        {
            co_return;
        }
        catch (net::FdRegistrationFailed const&)
        {
            co_return;
        }
        if (_stopping)
            co_return;

        _options.interruptWakeup->reset();
        // The wakeup only says "look"; the flag is where the signal handler recorded WHAT. A
        // wakeup with no flag behind it is a spurious signal and is not an interrupt. Clearing it
        // here is what makes each interrupt observed exactly once.
        if (!platform::SignalHandler::hasPendingSigint())
            continue;
        platform::SignalHandler::clearPendingSigint();
        runInterruptPolicy();
    }
}

async::Task<void> TuiRuntime::signalFlow()
{
    // `!_stopping` rather than `true`, checked BEFORE the first `co_await`: a flow must never park
    // on the loop once teardown has begun, whatever resumed it. See ~TuiRuntime.
    while (!_stopping)
    {
        try
        {
            co_await _loop.waitReadable(_options.signalFd);
        }
        catch (async::OperationCancelled const&)
        {
            co_return;
        }
        catch (net::FdRegistrationFailed const&)
        {
            co_return;
        }
        if (_stopping)
            co_return;

        // A reaped job (SIGCHLD) is non-input activity: resume an idle flow so its owner reports
        // finished jobs promptly instead of at the next keypress.
        if (platform::SignalHandler::processSignalFd())
            notifyActivity();
    }
}

async::Task<void> TuiRuntime::endProbeFlow()
{
    // `!_stopping` rather than `true`, checked BEFORE every `co_await`: a flow must never park on
    // the loop once teardown has begun, whatever resumed it. See ~TuiRuntime.
    while (!_stopping && !_inputClosed)
    {
        switch (callOut([this] { return _input.probeEnd(); }))
        {
            case EndProbe::NotNeeded: co_return; // its end arrives as readiness; asking adds nothing
            case EndProbe::Ended:
                // The input flow is parked on a handle that will never be signalled, so it is not the
                // one to end the stream, as it is after an ended read: complete what a partial
                // sequence was holding, since no continuation is coming, and end the stream here.
                routeDecoded(callOut([this] { return _input.flushPartial(); }));
                endInput();
                co_return;
            case EndProbe::Open: break;
        }
        try
        {
            co_await _loop.delay(_options.endProbe);
        }
        catch (async::OperationCancelled const&)
        {
            co_return;
        }
    }
}

void TuiRuntime::runInterruptPolicy()
{
    if (_onInterrupt)
        callOut(_onInterrupt);
    else
        // Not `rootStopSource().request_stop()`, which cancels every flow and unparks none of
        // them: a flow parked on a socket would observe its cancellation only when that socket
        // next became readable. `requestStop` is the same request plus the unparking.
        _loop.requestStop();
}

void TuiRuntime::routeDecoded(std::vector<InputEvent> events)
{
    if (events.empty())
        return;

    // The source dispatches the terminal's own protocol responses (colour scheme, focus, cursor
    // position, cell size) and removes them. A focus change is reported back because it is
    // non-input activity an idle flow still wants: focus chrome has to redraw.
    auto const focusChanged = callOut([this, &events] { return _input.consumeReports(events); });

    for (auto& event: events)
        // A second filter over a source that is contracted to have applied the first. It is
        // deliberate: `isProtocolReport` is the policy, a source is only obliged to deliver
        // decoded events, and a report that reached an application as input would look like a
        // keystroke nobody typed. The cost is one predicate per event.
        if (!isProtocolReport(event))
            _inputBuffer.push_back(std::move(event));

    if (focusChanged)
        notifyActivity();
    deliverInput();
}

bool TuiRuntime::hasBufferedInput()
{
    if (_inputBuffer.empty())
        // A terminal query reads input while it waits for its reply and hands back whatever was
        // not the reply. Those events were read off the input handle BEFORE anything still on it,
        // so nothing is going to become readable on their account and no source flow will ever
        // notice them. Asking here is what delivers them, at the one moment it matters.
        for (auto& event: callOut([this] { return _input.takePending(); }))
            if (!isProtocolReport(event))
                _inputBuffer.push_back(std::move(event));
    return !_inputBuffer.empty();
}

InputEvent TuiRuntime::popBufferedInput()
{
    assert(!_inputBuffer.empty() && "TuiRuntime::popBufferedInput with nothing buffered");
    auto event = std::move(_inputBuffer.front());
    _inputBuffer.pop_front();
    return event;
}

void TuiRuntime::parkOnInput(std::coroutine_handle<> waiter,
                             InputWake wake,
                             std::optional<platform::SteadyTimePoint> deadline)
{
    assert(!_stopping && "TuiRuntime::parkOnInput during teardown: the awaiters decline to park");
    assert(!_inputWaiter && "TuiRuntime: an input waiter is already parked");
    _inputWaiter = waiter;
    _inputWake = wake;
    if (deadline)
        _inputDeadline = _loop.addTimer(*deadline, &TuiRuntime::onInputDeadline, this);
}

void TuiRuntime::releaseInputWaiter(std::coroutine_handle<> waiter) noexcept
{
    // An empty handle is an awaiter that declined to park -- it was cancelled before it got the
    // chance -- and it owns none of this state. Without this the comparison below would find an
    // empty slot "equal" and retire a deadline belonging to nobody.
    if (!waiter)
        return;
    // Idempotent, and usually a no-op on the slot: the resumption that brought this flow back is
    // what emptied it.
    //
    // **The deadline is retired where the waiter LEAVES the slot, not here.** `takeInputWaiter` is
    // that place and every path goes through it, so by the time this runs `_inputDeadline` may
    // already belong to a DIFFERENT flow -- one that parked while this one was queued. Retiring it
    // here disarmed that flow's timeout without firing it, so a `nextEventFor` or `nextActivity`
    // waited forever on a deadline it had asked for and silently lost. Retired only in the branch
    // where the slot still holds us, which is the only case in which it is ours.
    if (_inputWaiter == waiter)
    {
        _inputWaiter = {};
        _inputWake = InputWake::EventOnly;
        retireTimer(_inputDeadline);
    }
    forgetHandedToLoop(waiter);
}

void TuiRuntime::parkOnAgent(std::coroutine_handle<> waiter)
{
    assert(!_stopping && "TuiRuntime::parkOnAgent during teardown: the awaiters decline to park");
    assert(!_agentWaiter && "TuiRuntime: an agent waiter is already parked");
    _agentWaiter = waiter;
}

void TuiRuntime::releaseAgentWaiter(std::coroutine_handle<> waiter) noexcept
{
    if (!waiter)
        return;
    if (_agentWaiter == waiter)
        _agentWaiter = {};
    forgetHandedToLoop(waiter);
}

void TuiRuntime::requestCancelWaiter(std::coroutine_handle<> waiter) noexcept
{
    if (!waiter)
        return;
    // These two slots are this runtime's state, and this runtime's state is the loop thread's --
    // the same rule `EventLoop::registerPark` asserts, for the same reason: a token stopped from
    // another thread would write them beside a turn reading them. Cancel a TUI flow on the loop's
    // thread, or `post()` a call to whatever does it.
    assert(_loop.teardownIsSerialisedWithDispatch()
           && "TuiRuntime: a TUI flow was cancelled from a second thread while another is driving "
              "its loop; post() the cancellation to the loop instead");

    if (_inputWaiter == waiter)
        std::ignore = takeInputWaiter();
    else if (_agentWaiter == waiter)
        _agentWaiter = {};
    else
        // Already resumed -- by an event, by a deadline, or by an earlier cancellation. Queueing
        // it again would resume a frame the first resumption may already have destroyed.
        return;

    // BORROWS, which is what this is: the frame belongs to whoever is awaiting, it is suspended
    // at the `co_await` this cancels, and its `await_resume` is what runs next.
    handToLoop(waiter);
}

std::coroutine_handle<> TuiRuntime::takeInputWaiter() noexcept
{
    retireTimer(_inputDeadline);
    _inputWake = InputWake::EventOnly;
    return std::exchange(_inputWaiter, {});
}

void TuiRuntime::deliverInput()
{
    if (!_inputWaiter || !hasBufferedInput())
        return;
    handToLoop(takeInputWaiter());
}

void TuiRuntime::notifyActivity()
{
    // A `nextEvent()` waiter is deliberately left alone: its `await_resume` can produce an event
    // or throw, and nothing else, so resuming it with no event would report a focus change as a
    // cancellation -- which is how a focus change used to close an open modal.
    if (!_inputWaiter || _inputWake == InputWake::EventOnly)
        return;
    handToLoop(takeInputWaiter());
}

void TuiRuntime::notifyAgentReady()
{
    _agentPending = true;
    if (_agentWaiter)
    {
        handToLoop(std::exchange(_agentWaiter, {}));
        return;
    }
    if (_inputWaiter && _inputWake == InputWake::OrAgent)
        handToLoop(takeInputWaiter());
}

void TuiRuntime::armEscapeFlush()
{
    if (_escapeFlush)
        return; // the earlier deadline is the one that matters
    _escapeFlush =
        _loop.addTimer(_loop.clock().now() + _options.escapeFlush, &TuiRuntime::onEscapeFlush, this);
}

void TuiRuntime::retireTimer(net::TimerId& timer) noexcept
{
    if (timer)
        std::ignore = _loop.cancelTimer(timer);
    timer = net::TimerId::invalid();
}

void TuiRuntime::onInputDeadline(void* state) noexcept
{
    auto& self = *static_cast<TuiRuntime*>(state);
    // The timer has fired, so it is gone from the loop's table; clearing the id here is what stops
    // `releaseInputWaiter` from trying to cancel one that already ran.
    self._inputDeadline = net::TimerId::invalid();
    self.notifyActivity();
}

void TuiRuntime::onEscapeFlush(void* state) noexcept
{
    auto& self = *static_cast<TuiRuntime*>(state);
    self._escapeFlush = net::TimerId::invalid();
    self.routeDecoded(self.callOut([&self] { return self._input.flushPartial(); }));
}

NextInputEventAwaiter TuiRuntime::nextEvent() noexcept
{
    return NextInputEventAwaiter { *this };
}

NextEventForAwaiter TuiRuntime::nextEventFor(std::chrono::milliseconds timeout) noexcept
{
    return NextEventForAwaiter { *this, _loop.clock().now() + timeout };
}

NextActivityAwaiter TuiRuntime::nextActivity(std::chrono::milliseconds timeout) noexcept
{
    return NextActivityAwaiter { *this, _loop.clock().now() + timeout };
}

NextAgentReadyAwaiter TuiRuntime::nextAgentReady() noexcept
{
    return NextAgentReadyAwaiter { *this };
}

} // namespace core::tui::runtime
