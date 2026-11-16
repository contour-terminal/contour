// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `InputSource` — where the TUI runtime's input comes from, and nothing else.
///
/// This is what is left of the runtime's old `EventSource` once the waiting is the event loop's.
/// That interface was a *multiplexer*: it owned a blocking `wait(timeoutMs)`, folded terminal
/// input, resize, the agent and interrupt wakeups and an arbitrary fd registry into one native
/// wait set, and reported what it found. Every one of those jobs now belongs to
/// @c core::net::EventLoop, which does them for sockets and timers too — so what a terminal is
/// still uniquely able to answer is only this: **which handles become ready when there is input,
/// and what the bytes behind them decode to.**
///
/// The split is what makes the seam small enough to fake honestly. A test double for the old
/// interface had to impersonate a scheduler, so a case written against it proved things about the
/// double rather than about the runtime. Here readiness is scripted at the backend
/// (@c core::net::testing::ScriptedBackend) and decoding at this interface
/// (@c core::tui::runtime::testing::ScriptedInputSource), which are genuinely different concerns:
/// one is "the kernel says this handle is ready", the other is "those bytes are the letter k".

#include <core/platform/Types.hpp>
#include <core/tui/EndProbe.hpp>
#include <core/tui/InputEvent.hpp>

#include <optional>
#include <vector>

namespace core::tui::runtime
{

/// Where terminal input comes from: the handles to watch, and the decode behind them.
///
/// Every read here is NON-BLOCKING and is called only from the loop's thread, after the loop has
/// reported the corresponding handle ready. An implementation that blocked would block the loop
/// and every other flow on it.
class InputSource
{
  public:
    InputSource() = default;
    virtual ~InputSource() = default;

    InputSource(InputSource const&) = delete;
    InputSource& operator=(InputSource const&) = delete;
    InputSource(InputSource&&) = delete;
    InputSource& operator=(InputSource&&) = delete;

    /// @return The handle that becomes readable when terminal input is available, or
    ///         @c platform::InvalidHandle for a source that never delivers input. Must not change
    ///         for this source's lifetime: the runtime registers it once, at construction.
    [[nodiscard]] virtual platform::NativeHandle inputHandle() const noexcept = 0;

    /// @return The handle that becomes readable when the terminal is resized, or
    ///         @c platform::InvalidHandle where this source has no resize channel. Must not change
    ///         for this source's lifetime, for @c inputHandle's reason.
    [[nodiscard]] virtual platform::NativeHandle resizeHandle() const noexcept = 0;

    /// Takes the events a terminal query read but did not consume.
    ///
    /// They were read off the input handle BEFORE anything still on it, so they are delivered
    /// without waiting — a key typed while a DA1 probe was in flight reaches the application
    /// rather than being reordered behind input that arrived later.
    /// @return The handed-back events in arrival order; empty when there are none.
    [[nodiscard]] virtual std::vector<InputEvent> takePending() = 0;

    /// Reads and decodes whatever is ready on @c inputHandle now. Never blocks.
    /// @return The decoded events; empty where the ready bytes decoded to nothing (a partial
    ///         escape sequence, or a console record that is not input).
    [[nodiscard]] virtual std::vector<InputEvent> readReady() = 0;

    /// Whether the input has ended for good: the last @c readReady found the handle at its end
    /// (a terminal that hung up, a pipe whose writer closed, a console that went away) rather than
    /// merely empty.
    ///
    /// The runtime asks after every @c readReady and, on true, stops watching the handle and ends
    /// its input stream. A handle at its end stays readable for ever, so a runtime that could not
    /// tell "ended" from "nothing yet" re-parked on it every turn and spun at 100% CPU
    /// ([core-cpp#49](https://github.com/contour-terminal/core-cpp/issues/49)).
    ///
    /// Not pure, and false by default, so a source written before 0.2.1 still compiles -- and
    /// still spins on a hung-up handle until it answers.
    /// @return true once the input handle has reached its end.
    [[nodiscard]] virtual bool inputClosed() const noexcept { return false; }

    /// Asks, without reading, whether the input has ended in a way its handle never reports as
    /// readiness -- a Windows console whose host died (see @c EndProbe). Never blocks.
    ///
    /// The runtime asks in its first turn, and again every @c TuiRuntimeOptions::endProbe for as
    /// long as the answer is @c EndProbe::Open. @c EndProbe::NotNeeded stops the asking, so a
    /// source whose end does arrive as readiness costs no timer; @c EndProbe::Ended ends the input
    /// stream exactly as an @c inputClosed read does.
    ///
    /// Not pure, and @c EndProbe::NotNeeded by default, for @c inputClosed's reason -- which means
    /// a source that wraps another one must forward this too, or it turns the question off.
    /// @return Whether the input is still there, has ended, or need not be asked.
    [[nodiscard]] virtual EndProbe probeEnd() { return EndProbe::NotNeeded; }

    /// Drains a pending resize notification and reports the new size. Never blocks.
    /// @return The resize event, or nothing where none was pending.
    [[nodiscard]] virtual std::optional<InputEvent> readResize() = 0;

    /// Completes a partial escape sequence whose continuation is not coming.
    ///
    /// A lone ESC is a prefix of every arrow and function key, so a decoder cannot tell Escape
    /// from an unfinished sequence without a clock. The runtime arms
    /// @c TuiRuntimeOptions::escapeFlush after any read that decoded to nothing and calls this
    /// when it elapses.
    /// @return The events the flush completed; empty where nothing was pending.
    [[nodiscard]] virtual std::vector<InputEvent> flushPartial() = 0;

    /// Dispatches the protocol responses in @p events to whoever handles them and removes them,
    /// so an internal report never surfaces as application input.
    /// @param events The decoded events, filtered in place.
    /// @return Whether a focus change was dispatched — a non-input wake an idle flow still wants,
    ///         because focus chrome has to redraw.
    [[nodiscard]] virtual bool consumeReports(std::vector<InputEvent>& events) = 0;
};

} // namespace core::tui::runtime
