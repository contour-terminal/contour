// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalInput.hpp>

#include <core/platform/Wakeup.hpp>
#include <core/tui/TerminalProtocols.hpp>
#include <core/tui/posix/PosixIO.hpp>

#include <sys/ioctl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

/// @file
/// The POSIX @c TerminalInput: `poll(2)` over stdin, the SIGWINCH self-pipe and the wakeup;
/// `termios` for raw mode. What neither platform's state reaches is in the shared
/// `TerminalInput.cpp`.

namespace core::tui
{

namespace
{
    /// The errors a non-blocking read answers "nothing yet" with. Every other one is the handle's
    /// end: the next read would fail the same way, and so would every read after it.
    constexpr auto NothingYet = std::array { EAGAIN, EWOULDBLOCK };

    /// Whether a read of @p fd that returned @p count, with @p error when it failed, found the
    /// handle at its end rather than merely empty.
    ///
    /// A terminal that hung up answers EIO (a pty whose master closed, on Linux) or an end of file
    /// (a hung-up controlling terminal), and a pipe whose writer closed answers an end of file.
    /// But an end of file on a terminal is also what a raw-mode read (VMIN 0, VTIME 0) returns when
    /// nothing is pending, and what canonical mode returns for Ctrl+D, so on a terminal it counts
    /// only when poll(2) confirms the hangup.
    ///
    /// EIO is also what a read from a BACKGROUND process group answers when `SIGTTIN` is ignored or
    /// blocked, and it would latch the end for good although the terminal is alive. A process that
    /// leaves `SIGTTIN` at its default is stopped instead and never sees it -- endo and tuidu do,
    /// and `core::platform::SignalHandler` ignores only `SIGTTOU` -- so a consumer that ignores
    /// `SIGTTIN` and reads from the background must not rely on this.
    [[nodiscard]] bool inputHasEnded(int fd, ssize_t count, int error) noexcept
    {
        if (count > 0)
            return false;
        if (count < 0)
            return !std::ranges::contains(NothingYet, error);
        if (::isatty(fd) == 0)
            return true;
        auto watch = pollfd { .fd = fd, .events = POLLIN, .revents = 0 };
        return ::poll(&watch, 1, 0) > 0 && (watch.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
} // namespace

/// The descriptors, the saved terminal attributes and the self-pipe a size change is announced on.
struct TerminalInput::NativeState
{
    int fd = STDIN_FILENO;                    ///< The input descriptor, set by initialize().
    int outFd = STDOUT_FILENO;                ///< Where a protocol sequence is written.
    struct termios originalTermios {};        ///< Restored by disableRawMode().
    std::array<int, 2> resizePipe { -1, -1 }; ///< Self-pipe for SIGWINCH; { read, write }.
};

TerminalInput::TerminalInput(): _native(std::make_unique<NativeState>())
{
}

TerminalInput::~TerminalInput()
{
    shutdown();
}

auto TerminalInput::initialize() -> VoidResult
{
    _native->fd = STDIN_FILENO;

    // Create self-pipe for SIGWINCH notification
    if (pipe(_native->resizePipe.data()) == -1)
        return makeError(ErrorCode::IoError, "Failed to create resize notification pipe");

    // Both ends are non-blocking: the read end so draining it in poll() cannot stall, and the
    // write end because notifyResize() runs from the SIGWINCH handler. A pipe filled with resize
    // bytes nobody has drained yet would otherwise block ::write() inside a signal context, with
    // the interrupted mainline unable to drain it. Losing a byte there costs nothing -- one byte
    // in the pipe already says "the size changed", and poll() re-queries the size anyway.
    for (auto const end: _native->resizePipe)
    {
        if (auto const flags = fcntl(end, F_GETFL, 0); flags != -1)
            fcntl(end, F_SETFL, flags | O_NONBLOCK);
    }

    enableRawMode();
    enableProtocols();

    return {};
}

void TerminalInput::shutdown()
{
    if (_rawMode)
    {
        disableProtocols();
        disableRawMode();
    }

    if (_native->resizePipe[0] != -1)
    {
        close(_native->resizePipe[0]);
        close(_native->resizePipe[1]);
        _native->resizePipe = { -1, -1 };
    }
}

auto TerminalInput::poll(int timeoutMs) -> std::vector<InputEvent>
{
    // Events a terminal query read ahead of its reply were read before anything still waiting
    // on the handle, so they are delivered first, and without waiting.
    if (auto pending = takePending(); !pending.empty())
        return pending;

    auto fds = std::array<struct pollfd, 3> {};
    fds[0] = { .fd = _native->fd, .events = POLLIN, .revents = 0 };
    fds[1] = { .fd = _native->resizePipe[0], .events = POLLIN, .revents = 0 };
    fds[2] = { .fd = _wakeup ? _wakeup->nativeHandle() : -1, .events = POLLIN, .revents = 0 };

    auto nfds = nfds_t { 1 };
    if (_native->resizePipe[0] != -1)
        nfds = 2;
    if (_wakeup)
        nfds = 3;

    auto const pollResult = ::poll(fds.data(), nfds, timeoutMs);

    // A hangup with nothing to read beside it: the read below would not run, and the caller would
    // poll again and be answered at once, for ever (core-cpp#49).
    if (pollResult > 0 && (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0
        && (fds[0].revents & POLLIN) == 0)
        _inputClosed = true;

    if (pollResult <= 0)
    {
        // Timeout or error — check for pending partial sequences
        if (pollResult == 0)
            return parserTimeout();
        return {};
    }

    auto events = std::vector<InputEvent> {};

    // Check resize pipe
    if (nfds >= 2 && (fds[1].revents & POLLIN) != 0)
        if (auto const resize = drainResize())
            events.emplace_back(*resize);

    // Check wakeup fd — just drain it; the actual messages are in the MessageQueue.
    if (nfds >= 3 && (fds[2].revents & POLLIN) != 0)
        _wakeup->reset();

    // Check stdin
    if ((fds[0].revents & POLLIN) != 0)
    {
        auto parsed = readReadyInput();
        events.insert(
            events.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    }

    return events;
}

auto TerminalInput::inputNativeHandle() const noexcept -> core::platform::NativeHandle
{
    return _native->fd;
}

auto TerminalInput::resizeNativeHandle() const noexcept -> core::platform::NativeHandle
{
    return _native->resizePipe[0];
}

auto TerminalInput::probeEnd() -> EndProbe
{
    // A terminal that hangs up, and a pipe whose writer closed, make the descriptor readable, and
    // readReadyInput finds the end behind that readiness.
    return EndProbe::NotNeeded;
}

auto TerminalInput::readReadyInput() -> std::vector<InputEvent>
{
    auto events = std::vector<InputEvent> {};
    auto buf = std::array<char, 512> {};
    auto const n = safeRead(_native->fd, buf.data(), buf.size());
    if (n > 0)
    {
        auto parsed = _parser.feed(std::string_view(buf.data(), static_cast<size_t>(n)));
        events.insert(
            events.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    }
    else if (inputHasEnded(_native->fd, n, errno))
        _inputClosed = true;
    return events;
}

auto TerminalInput::drainResize() -> std::optional<ResizeEvent>
{
    // Drain the self-pipe so the next resize re-arms the notification.
    auto buf = char {};
    while (safeRead(_native->resizePipe[0], &buf, 1) > 0)
        ;

    auto ws = winsize {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        return ResizeEvent { .columns = ws.ws_col, .rows = ws.ws_row };
    return std::nullopt;
}

void TerminalInput::notifyResize(int /*cols*/, int /*rows*/)
{
    if (_native->resizePipe[1] != -1)
    {
        auto const byte = char { 1 };
        safeWrite(_native->resizePipe[1], &byte, 1);
    }
}

auto TerminalInput::resizePipeReadFd() const noexcept -> int
{
    return _native->resizePipe[0];
}

void TerminalInput::enableRawMode()
{
    tcgetattr(_native->fd, &_native->originalTermios);
    auto raw = _native->originalTermios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(_native->fd, TCSAFLUSH, &raw);
    _rawMode = true;
}

void TerminalInput::disableRawMode()
{
    if (_rawMode)
    {
        tcsetattr(_native->fd, TCSAFLUSH, &_native->originalTermios);
        _rawMode = false;
    }
}

void TerminalInput::writeProtocol(std::string_view data) const
{
    safeWrite(_native->outFd, data.data(), data.size());
}

void TerminalInput::enableProtocols()
{
    writeProtocol(protocols::EnableCsiU);
    writeProtocol(
        protocols::EnablePassiveMouseTracking); // Implicitly enables SGR (1006) + button tracking (1002)
    writeMouseTrackingChange(MouseTracking::Off, mouseTracking());
    writeProtocol(protocols::EnableBracketedPaste);
    writeProtocol(protocols::EnableColorSchemeNotify);
    writeProtocol(protocols::QueryColorScheme);
    writeProtocol(protocols::EnableFocusTracking);
}

void TerminalInput::disableProtocols()
{
    writeProtocol(protocols::DisableFocusTracking);
    writeProtocol(protocols::DisableColorSchemeNotify);
    writeProtocol(protocols::DisableBracketedPaste);
    writeMouseTrackingChange(mouseTracking(), MouseTracking::Off);
    writeProtocol(protocols::DisablePassiveMouseTracking); // Also clears implicit mouse modes
    writeProtocol(protocols::DisableCsiU);
}

} // namespace core::tui
