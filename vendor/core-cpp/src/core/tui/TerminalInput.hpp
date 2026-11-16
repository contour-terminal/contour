// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Types.hpp>
#include <core/tui/EndProbe.hpp>
#include <core/tui/Error.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/MouseTracking.hpp>
#include <core/tui/VtParser.hpp>

#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace core::platform
{
class Wakeup;
}

namespace core::tui
{

/// @brief Handles raw terminal input, polling, and event production.
///
/// Manages raw mode, enables Kitty keyboard protocol, SGR mouse reporting,
/// and bracketed paste. Uses poll() for non-blocking reads with timeout.
/// On POSIX, SIGWINCH is handled via a self-pipe pattern for thread-safe resize notification.
/// On Windows, WINDOW_BUFFER_SIZE_EVENT is used for resize detection.
class TerminalInput
{
  public:
    TerminalInput();
    ~TerminalInput();

    TerminalInput(TerminalInput const&) = delete;
    auto operator=(TerminalInput const&) -> TerminalInput& = delete;
    TerminalInput(TerminalInput&&) = delete;
    auto operator=(TerminalInput&&) -> TerminalInput& = delete;

    /// @brief Initializes raw mode and enables terminal protocols.
    /// @return Success or an IoError on failure.
    [[nodiscard]] auto initialize() -> VoidResult;

    /// @brief Restores the original terminal state.
    void shutdown();

    /// @brief Polls for input events with optional timeout.
    /// @param timeoutMs -1 = block indefinitely, 0 = non-blocking, >0 = timeout in milliseconds.
    /// @return Vector of parsed events (empty on timeout or no data).
    [[nodiscard]] auto poll(int timeoutMs = -1) -> std::vector<InputEvent>;

    /// @brief Hands events back, to be delivered before any input not yet read.
    ///
    /// A terminal query reads input while it waits for its reply, and whatever it read that was
    /// not the reply comes back here. So a key typed while a probe was in flight still reaches the
    /// application: @c poll() delivers pending events first, without waiting, and the coroutine
    /// runtime takes them through @c core::tui::runtime::InputSource::takePending.
    /// @param events Events in arrival order, appended after any already pending.
    void unread(std::vector<InputEvent> events)
    {
        _pending.insert(
            _pending.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
    }

    /// @brief Removes and returns every event handed back by @c unread(), in arrival order.
    /// @return The pending events; empty when there are none.
    [[nodiscard]] auto takePending() -> std::vector<InputEvent> { return std::exchange(_pending, {}); }

    /// @brief Injects a synthetic resize event.
    ///
    /// On POSIX, writes to the self-pipe to wake poll().
    /// On Windows, signals a manual-reset event.
    /// @param cols New column count.
    /// @param rows New row count.
    void notifyResize(int cols, int rows);

    /// @brief Returns the file descriptor for the resize notification pipe (read end).
    /// On Windows, returns -1 (resize is event-based).
    [[nodiscard]] auto resizePipeReadFd() const noexcept -> int;

    /// @brief Returns the native handle for the input stream (stdin / console input).
    ///
    /// Used by the coroutine runtime's multiplexer to wait on input alongside
    /// other sources, rather than calling the all-in-one poll().
    [[nodiscard]] auto inputNativeHandle() const noexcept -> core::platform::NativeHandle;

    /// @brief Returns the native handle that becomes ready on terminal resize.
    /// @return The resize pipe read end (POSIX) or resize event (Windows);
    ///         @c InvalidHandle if not yet initialized.
    [[nodiscard]] auto resizeNativeHandle() const noexcept -> core::platform::NativeHandle;

    /// @brief Reads and parses input currently ready on the input handle (non-blocking).
    ///
    /// A read that finds the handle at its END rather than empty sets @c inputClosed(): on POSIX
    /// a read error other than "nothing yet" (EIO from a terminal that hung up), or an end of file
    /// on a pipe or a file, or on a terminal that @c poll(2) reports hung up; on Windows a console
    /// input handle that can no longer be read (the console was closed or detached).
    /// @return Parsed events (may be empty if no decodable input was ready).
    [[nodiscard]] auto readReadyInput() -> std::vector<InputEvent>;

    /// @brief Whether the input handle has reached its end, as a read or a wait has found it.
    ///
    /// Sticky: nothing more will be read from a handle at its end, and it stays readable for ever,
    /// so a caller that waits for readiness again after this answers true is waiting on a handle
    /// that answers at once and yields nothing -- the 100% CPU spin of
    /// [core-cpp#49](https://github.com/contour-terminal/core-cpp/issues/49).
    /// @return true once the input handle has hung up, closed or been detached.
    [[nodiscard]] auto inputClosed() const noexcept -> bool { return _inputClosed; }

    /// @brief Asks, without reading, whether the input has ended where no wait would report it.
    ///
    /// On POSIX a terminal that hangs up makes its descriptor readable, so the end is found by
    /// @c readReadyInput and this answers @c EndProbe::NotNeeded. On Windows a console whose host
    /// process died never signals its input handle again, so this asks the console for its pending
    /// event count: a console that can no longer answer has ended, and @c inputClosed() answers
    /// true from then on. Never blocks.
    /// @return @c EndProbe::NotNeeded on POSIX; @c EndProbe::Open or @c EndProbe::Ended on Windows.
    [[nodiscard]] auto probeEnd() -> EndProbe;

    /// @brief Drains a pending resize notification and queries the new size.
    /// @return The resize event if one was pending, else std::nullopt.
    [[nodiscard]] auto drainResize() -> std::optional<ResizeEvent>;

    /// @brief Flushes a timed-out partial escape sequence into completed events.
    /// @return Parsed events (may be empty).
    [[nodiscard]] auto parserTimeout() -> std::vector<InputEvent>;

    /// @brief Suspends terminal protocols and raw mode for external command execution.
    ///
    /// Call this before executing external commands to restore the terminal to
    /// a normal state that programs expect. Call resume() after the command completes.
    void suspend();

    /// @brief Resumes terminal protocols and raw mode after external command execution.
    ///
    /// Call this after an external command completes to restore the shell's
    /// terminal configuration.
    void resume();

    /// @brief Returns whether the terminal is currently suspended.
    [[nodiscard]] auto isSuspended() const noexcept -> bool;

    /// @brief Raises the mouse tracking mode to any-motion (mode 1003) while @p enabled, for hover
    /// tooltips.
    ///
    /// Call it only after confirming passive mouse tracking (mode 2029) via DECRQPM, so that a terminal
    /// without it is not asked to report every motion. While raised, @c mouseTracking() answers
    /// @c MouseTracking::AnyMotion whatever @c setMouseTracking() requested; lowered, the requested
    /// mode applies again. The change is written at once when the protocols are enabled.
    /// @param enabled True to raise the mode, false to return to the requested one.
    void setAnyMotionTracking(bool enabled);

    /// @brief Sets how much mouse input the terminal is asked to report.
    ///
    /// Effective at the next enableProtocols() -- @c initialize() or @c resume() -- and at once when
    /// the protocols are enabled now, by writing only the change from the current mode. The default
    /// is @c MouseTracking::Off: a terminal that reports the mouse stops selecting text on a
    /// click-and-drag, so an application opts in.
    /// @param mode The mode the application asks for.
    void setMouseTracking(MouseTracking mode);

    /// @brief Returns the mode the terminal is asked for.
    /// @return The mode @c setMouseTracking() requested, or @c MouseTracking::AnyMotion while
    ///         @c setAnyMotionTracking() raises it.
    [[nodiscard]] auto mouseTracking() const noexcept -> MouseTracking;

    /// @brief Sets a cross-platform wakeup handle for poll() integration.
    ///
    /// When set, poll() will also monitor the wakeup's native handle alongside
    /// stdin and the resize pipe/event. This allows a background thread to wake
    /// the poll() call by signaling the wakeup.
    /// @param wakeup Pointer to the wakeup primitive (must outlive this object), or nullptr to disable.
    void setWakeup(core::platform::Wakeup* wakeup);

  private:
    /// The handles, saved modes and resize channel this platform needs, defined by
    /// `posix/TerminalInput.cpp` (descriptors, the saved `termios`, the SIGWINCH self-pipe) and by
    /// `windows/TerminalInput.cpp` (the console handles, the saved console modes and code pages,
    /// the resize event). Opaque here, so that this header names no `<termios.h>` or `<windows.h>`
    /// type and a consumer including it gets neither (.agent/rules/platform.md, Ruling R41). It is
    /// what keeps the size and layout of this class out of the header, hence the indirection.
    struct NativeState;

    VtParser _parser;
    bool _rawMode = false;
    bool _suspended = false;         ///< True when suspended for external command execution.
    bool _inputClosed = false;       ///< True once the input handle was found at its end.
    bool _anyMotionTracking = false; ///< True while hover tracking raises the mode to any-motion (1003).
    MouseTracking _mouseTracking = MouseTracking::Off; ///< The mode setMouseTracking() requested.

    std::unique_ptr<NativeState> _native;      ///< Never null; the platform's own state.
    core::platform::Wakeup* _wakeup = nullptr; ///< Optional cross-thread wakeup handle.
    std::vector<InputEvent> _pending;          ///< Events handed back by unread(), delivered first.

    void enableRawMode();
    void disableRawMode();
    void enableProtocols();
    void disableProtocols();
    void writeProtocol(std::string_view data) const;

    /// @brief Writes the sequences that move the terminal from mouse tracking @p from to @p to.
    /// @param from The mode the terminal is in.
    /// @param to The mode it is to be in.
    void writeMouseTrackingChange(MouseTracking from, MouseTracking to) const;

    /// @brief Stores the hover flag and the requested mode, and writes the resulting change when the
    /// protocols are enabled.
    /// @param anyMotion Whether hover raises the mode to any-motion.
    /// @param requested The mode the application asked for.
    void updateMouseTracking(bool anyMotion, MouseTracking requested);
};

} // namespace core::tui
