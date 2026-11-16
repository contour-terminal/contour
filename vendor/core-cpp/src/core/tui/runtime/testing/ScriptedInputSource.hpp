// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A scripted @c InputSource for deterministic @c TuiRuntime unit tests.
///
/// It is half of what `MockEventSource` was, and the half that is missing is the point. That
/// double had to impersonate a scheduler — it owned `wait(timeoutMs)`, handed out fd registration
/// tokens and decided when a park resolved — so a case written against it proved things about the
/// double. Scheduling is now @c core::net::EventLoop's, and a case scripts READINESS at
/// @c core::net::testing::ScriptedBackend or gets it from a real kernel, while this scripts only
/// DECODING: which events the bytes behind a ready handle turn into.
///
/// Point it at a @c core::platform::SystemPipe and readiness becomes real on every platform, so
/// the same case runs over epoll, poll, kqueue and IOCP unchanged. Leave the pipe out and
/// the handle is inert, which is what a scripted backend wants — it names registrations by attach
/// order and never looks at the handle.

#include <core/platform/SystemPipe.hpp>
#include <core/platform/Types.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/runtime/InputSource.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace core::tui::runtime::testing
{

/// What a source with no pipe behind it reports as its input handle.
///
/// **Stated rather than implied.** This used to answer `platform::standardInput()` unconditionally,
/// so five cases silently depended on the test process having a usable stdin — and on Windows,
/// where `standardInput()` asks the OS rather than returning a constant, a closed stdin made the
/// runtime start no input flow at all and the case HUNG instead of failing. Which of the two a case
/// wants is a property of the case, so the case says it.
enum class HandleFor : std::uint8_t
{
    /// No input channel. The runtime starts no input flow, which is the headless case: a runtime
    /// whose input can only arrive through @c pushPending.
    Nothing,

    /// A handle that is merely NON-INVALID, so the runtime starts an input flow and the backend
    /// has a registration to name. Only meaningful with @c core::net::testing::ScriptedBackend,
    /// which never looks at the value; a real backend would refuse or watch the real stdin.
    Input,
};

/// An @c InputSource that returns a pre-written sequence of decoded events.
class ScriptedInputSource: public InputSource
{
  public:
    /// @param input The pipe whose readiness stands in for terminal input, or null for a case
    ///        whose readiness is scripted at the backend instead. Not owned; it must outlive this
    ///        source, and the runtime reads its handle once.
    /// @param resize The pipe whose readiness stands in for a terminal resize, or null for a
    ///        source with no resize channel. Not owned.
    /// @param withoutPipe What @c inputHandle answers when @p input is null; see @c HandleFor.
    explicit ScriptedInputSource(platform::SystemPipe* input = nullptr,
                                 platform::SystemPipe* resize = nullptr,
                                 HandleFor withoutPipe = HandleFor::Nothing) noexcept:
        _input(input), _resize(resize), _withoutPipe(withoutPipe)
    {
    }

    /// @name Scripting
    /// @{

    /// Appends one read's worth of events, and makes the input handle ready if this source has a
    /// real pipe behind it.
    ///
    /// **Callable from another thread**, which is what a case about input arriving WHILE the loop
    /// waits needs: nothing is ready when the flow parks, and the wait has to be ended by
    /// something outside it. The lock is what makes that legitimate rather than a data race the
    /// pipe's own ordering happens to hide.
    /// @param events What the next @c readReady returns.
    void pushEvents(std::vector<InputEvent> events)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _reads.push_back(std::move(events));
        }
        signal(_input);
    }

    /// Appends events as a terminal query hands them back: delivered without waiting, because
    /// they were read before anything still on the handle.
    /// @param events The handed-back events.
    void pushPending(std::vector<InputEvent> events)
    {
        auto const lock = std::scoped_lock { _mutex };
        for (auto& event: events)
            _pending.push_back(std::move(event));
    }

    /// Appends a resize notification, and makes the resize handle ready.
    /// @param resize The size change the next @c readResize reports.
    void pushResize(ResizeEvent resize)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _resizes.push_back(resize);
        }
        signal(_resize);
    }

    /// Appends what a partial-escape flush completes to — what a lone ESC decodes to once its
    /// continuation has not arrived.
    /// @param events The events the next @c flushPartial returns.
    void pushFlush(std::vector<InputEvent> events)
    {
        auto const lock = std::scoped_lock { _mutex };
        _flushes.push_back(std::move(events));
    }

    /// Ends the input: the first read that finds no scripted read left finds the handle at its end
    /// instead, as a terminal that hung up does, and @c inputClosed answers true from then on.
    /// Reads scripted before this are still delivered first, in order. With a pipe, it also makes
    /// the input handle readable, so the runtime reads; over a scripted backend, script the
    /// readiness (or the failure a hangup is) yourself.
    void closeInput()
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _closing = true;
        }
        signal(_input);
    }

    /// Appends one answer to @c probeEnd. Answers are given in order; once they run out, the source
    /// answers @c EndProbe::NotNeeded, as one whose end arrives as readiness does. An
    /// @c EndProbe::Ended answer also makes @c inputClosed answer true, as a real source's does.
    /// @param answer What the next @c probeEnd returns.
    void pushProbe(EndProbe answer)
    {
        auto const lock = std::scoped_lock { _mutex };
        _probes.push_back(answer);
    }

    /// @}

    /// @name Observation
    /// @{

    /// @return How many times @c probeEnd was called.
    [[nodiscard]] std::size_t probeCount() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _probeCount;
    }

    /// @return How many times @c readReady was called.
    [[nodiscard]] std::size_t readCount() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _readCount;
    }

    /// @return How many times @c flushPartial was called.
    [[nodiscard]] std::size_t flushCount() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _flushCount;
    }

    /// @}

    [[nodiscard]] platform::NativeHandle inputHandle() const noexcept override
    {
        if (_input != nullptr)
            return _input->waitHandle();
        return _withoutPipe == HandleFor::Input ? platform::standardInput() : platform::InvalidHandle;
    }

    [[nodiscard]] platform::NativeHandle resizeHandle() const noexcept override
    {
        return _resize != nullptr ? _resize->waitHandle() : platform::InvalidHandle;
    }

    [[nodiscard]] std::vector<InputEvent> takePending() override
    {
        auto const lock = std::scoped_lock { _mutex };
        return std::exchange(_pending, {});
    }

    [[nodiscard]] std::vector<InputEvent> readReady() override
    {
        drain(_input);
        auto const lock = std::scoped_lock { _mutex };
        ++_readCount;
        if (_reads.empty())
        {
            if (_closing)
                _closed.store(true, std::memory_order_relaxed);
            return {};
        }
        auto events = std::move(_reads.front());
        _reads.pop_front();
        return events;
    }

    [[nodiscard]] bool inputClosed() const noexcept override
    {
        return _closed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] EndProbe probeEnd() override
    {
        auto const lock = std::scoped_lock { _mutex };
        ++_probeCount;
        if (_probes.empty())
            return EndProbe::NotNeeded;
        auto const answer = _probes.front();
        _probes.pop_front();
        if (answer == EndProbe::Ended)
            _closed.store(true, std::memory_order_relaxed);
        return answer;
    }

    [[nodiscard]] std::optional<InputEvent> readResize() override
    {
        drain(_resize);
        auto const lock = std::scoped_lock { _mutex };
        if (_resizes.empty())
            return std::nullopt;
        auto const resize = _resizes.front();
        _resizes.pop_front();
        return InputEvent { resize };
    }

    [[nodiscard]] std::vector<InputEvent> flushPartial() override
    {
        auto const lock = std::scoped_lock { _mutex };
        ++_flushCount;
        if (_flushes.empty())
            return {};
        auto events = std::move(_flushes.front());
        _flushes.pop_front();
        return events;
    }

    /// Dispatches nothing and only reports, which is all a case needs: the runtime drops every
    /// protocol response itself, and what a real terminal additionally does with a focus report
    /// (notify its handlers) has no observable here.
    /// @param events The decoded events, filtered in place.
    /// @return Whether a focus report was among them.
    [[nodiscard]] bool consumeReports(std::vector<InputEvent>& events) override
    {
        auto focusChanged = false;
        auto kept = std::vector<InputEvent> {};
        kept.reserve(events.size());
        for (auto& event: events)
        {
            if (!isProtocolReport(event))
            {
                kept.push_back(std::move(event));
                continue;
            }
            if (std::holds_alternative<FocusEvent>(event))
                focusChanged = true;
        }
        events = std::move(kept);
        return focusChanged;
    }

  private:
    /// Makes @p pipe readable, so a real backend reports the readiness a script is standing in
    /// for. A source with no pipe has its readiness scripted at the backend instead.
    /// @param pipe The channel to signal, or null.
    static void signal(platform::SystemPipe* pipe)
    {
        if (pipe == nullptr)
            return;
        char const token = 'x';
        std::ignore = pipe->write(&token, 1);
    }

    /// Consumes one byte of @p pipe's readiness, so a level-triggered backend stops reporting it
    /// once every scripted batch has been read.
    /// @param pipe The channel to drain, or null.
    static void drain(platform::SystemPipe* pipe)
    {
        if (pipe == nullptr)
            return;
        char token = 0;
        std::ignore = pipe->read(&token, 1);
    }

    /// Guards every scripted container below, because @c pushEvents is the one member a case
    /// legitimately calls from a thread that is not the loop's.
    mutable std::mutex _mutex;

    platform::SystemPipe* _input;  ///< Input readiness, or null for a scripted backend.
    platform::SystemPipe* _resize; ///< Resize readiness, or null for no resize channel.
    HandleFor _withoutPipe;        ///< What @c inputHandle answers when there is no pipe.

    std::deque<std::vector<InputEvent>> _reads;   ///< One entry per scripted read.
    std::deque<std::vector<InputEvent>> _flushes; ///< One entry per scripted partial-escape flush.
    std::deque<ResizeEvent> _resizes;             ///< One entry per scripted resize.
    std::deque<EndProbe> _probes;                 ///< One entry per scripted end probe.
    std::vector<InputEvent> _pending;             ///< What a terminal query handed back.

    std::size_t _readCount = 0;  ///< How many reads happened.
    std::size_t _flushCount = 0; ///< How many flushes happened.
    std::size_t _probeCount = 0; ///< How many end probes happened.

    bool _closing = false;             ///< @c closeInput was called; the reads left are the last.
    std::atomic<bool> _closed = false; ///< A read found the end; atomic so the query can be noexcept.
};

} // namespace core::tui::runtime::testing
