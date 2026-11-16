// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The production @c InputSource: a @c Terminal, seen as handles and a decode.
///
/// It is what `TerminalEventSource` became. That class was a multiplexer with two platform bodies
/// — `poll(2)` over stdin, the resize pipe, the agent wakeup, the interrupt wakeup, the signal fd
/// and a user fd registry on POSIX; `WaitForMultipleObjects` over the same set on Windows — and
/// **both are gone, not ported**: @c core::net::EventLoop already waits on handles, on every
/// platform, and does it without the 64-handle cliff the Windows body fell off
/// ([core-cpp#19](https://github.com/contour-terminal/core-cpp/issues/19)).
///
/// What is left has no platform body at all. Every question this answers, @c TerminalInput already
/// answers portably, so `core::tui` no longer has a single translation unit that picks its
/// platform with an `#ifdef` — the rule in `.agent/rules/platform.md` holds here by construction
/// rather than by review.

#include <core/platform/Types.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/runtime/InputSource.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace core::tui::runtime
{

/// Adapts a @c Terminal to the runtime's @c InputSource.
///
/// Protocol responses (colour-scheme / focus / cursor-position / cell-size) are dispatched through
/// @c Terminal::consumeProtocolReports exactly as @c Terminal::poll does, so they never surface as
/// application input and the terminal's focus and colour-scheme handlers keep firing.
class TerminalInputSource: public InputSource
{
  public:
    /// @param terminal The terminal to read (not owned; outlives this source).
    ///
    ///        **Initialize it first.** The runtime registers this source's handles once, at its
    ///        own construction, and an uninitialized terminal has none to give — on Windows the
    ///        console handles are opened by @c Terminal::initialize. A terminal with no input
    ///        handle yields a runtime that never delivers input, which is the headless case and
    ///        is legitimate; it is not a terminal that starts working later.
    explicit TerminalInputSource(Terminal& terminal) noexcept: _terminal(terminal) {}

    [[nodiscard]] platform::NativeHandle inputHandle() const noexcept override
    {
        return _terminal.input().inputNativeHandle();
    }

    [[nodiscard]] platform::NativeHandle resizeHandle() const noexcept override
    {
        return _terminal.input().resizeNativeHandle();
    }

    [[nodiscard]] std::vector<InputEvent> takePending() override { return _terminal.input().takePending(); }

    [[nodiscard]] std::vector<InputEvent> readReady() override { return _terminal.input().readReadyInput(); }

    [[nodiscard]] bool inputClosed() const noexcept override { return _terminal.input().inputClosed(); }

    [[nodiscard]] EndProbe probeEnd() override { return _terminal.input().probeEnd(); }

    [[nodiscard]] std::optional<InputEvent> readResize() override
    {
        auto const resize = _terminal.input().drainResize();
        if (!resize)
            return std::nullopt;
        return InputEvent { *resize };
    }

    [[nodiscard]] std::vector<InputEvent> flushPartial() override
    {
        return _terminal.input().parserTimeout();
    }

    [[nodiscard]] bool consumeReports(std::vector<InputEvent>& events) override
    {
        return _terminal.consumeProtocolReports(events);
    }

  private:
    Terminal& _terminal; ///< Provides the input handles, the decode and the report handlers.
};

} // namespace core::tui::runtime
