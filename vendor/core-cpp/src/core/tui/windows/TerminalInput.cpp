// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalInput.hpp>

#include <core/platform/Wakeup.hpp>
#include <core/tui/TerminalProtocols.hpp>
#include <core/tui/detail/Utf16ToUtf8.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

/// @file
/// The Windows @c TerminalInput: `WaitForMultipleObjects` over the console input handle and the
/// resize event, console input records decoded as UTF-8 and fed to the same VT parser, and console
/// modes and code pages for raw mode. What neither platform's state reaches is in the shared
/// `TerminalInput.cpp`.

namespace core::tui
{

/// The console handles, the modes and code pages raw mode replaces, and the resize event.
struct TerminalInput::NativeState
{
    HANDLE stdinHandle = INVALID_HANDLE_VALUE;  ///< Console input, set by initialize().
    HANDLE stdoutHandle = INVALID_HANDLE_VALUE; ///< Console output, where a protocol write goes.
    DWORD originalInputMode = 0;                ///< Restored by disableRawMode().
    DWORD originalOutputMode = 0;               ///< Restored by disableRawMode().
    UINT originalOutputCp = 0;                  ///< Console output code page (0 = not saved).
    UINT originalInputCp = 0;                   ///< Console input code page (0 = not saved).
    HANDLE resizeEvent = nullptr;               ///< Manual-reset event for resize notification.

    /// Console input's UTF-16 to UTF-8. Kept across reads, because the two surrogates of one
    /// character are two key events and can arrive in two reads (core-cpp#20).
    detail::Utf16ToUtf8 utf16;
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
    _native->stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    _native->stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    if (_native->stdinHandle == INVALID_HANDLE_VALUE || _native->stdoutHandle == INVALID_HANDLE_VALUE)
        return makeError(ErrorCode::IoError, "Failed to get console handles");

    // Save original console modes
    if (!GetConsoleMode(_native->stdinHandle, &_native->originalInputMode))
        return makeError(ErrorCode::IoError, "Failed to get console input mode");
    if (!GetConsoleMode(_native->stdoutHandle, &_native->originalOutputMode))
        return makeError(ErrorCode::IoError, "Failed to get console output mode");

    // Save the original console code pages so raw mode can switch them to UTF-8 and shutdown
    // can restore them. We emit UTF-8 throughout, so the console must interpret our output as
    // UTF-8 (otherwise multibyte sequences render as mojibake under the default OEM code page).
    _native->originalOutputCp = GetConsoleOutputCP();
    _native->originalInputCp = GetConsoleCP();

    // Create a manual-reset event for resize notification
    _native->resizeEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (_native->resizeEvent == nullptr)
        return makeError(ErrorCode::IoError, "Failed to create resize event");

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

    if (_native->resizeEvent != nullptr)
    {
        CloseHandle(_native->resizeEvent);
        _native->resizeEvent = nullptr;
    }
}

auto TerminalInput::poll(int timeoutMs) -> std::vector<InputEvent>
{
    // Events a terminal query read ahead of its reply were read before anything still waiting
    // on the handle, so they are delivered first, and without waiting.
    if (auto pending = takePending(); !pending.empty())
        return pending;

    // Wait on both stdin and the resize event
    auto const handles = std::array<HANDLE, 2> { _native->stdinHandle, _native->resizeEvent };
    auto const handleCount = (_native->resizeEvent != nullptr) ? 2U : 1U;

    // A console whose host died is never signalled again, so one wait on it would last its whole
    // timeout -- for ever, for poll(-1). The wait is cut into slices no longer than ProbeSliceMs
    // instead, and the console asked between them whether it is still there (probeEnd).
    constexpr auto ProbeSliceMs = DWORD { 1000 };
    auto remaining = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
    auto waitResult = DWORD { WAIT_TIMEOUT };
    while (true)
    {
        auto const slice = (remaining == INFINITE) ? ProbeSliceMs : std::min(remaining, ProbeSliceMs);
        waitResult = WaitForMultipleObjects(handleCount, handles.data(), FALSE, slice);
        if (waitResult != WAIT_TIMEOUT)
            break;
        if (probeEnd() == EndProbe::Ended)
            return {};
        if (remaining != INFINITE)
        {
            remaining -= slice;
            if (remaining == 0)
                return parserTimeout();
        }
    }

    // The wait refused its handles: the console input handle is no longer valid (the console was
    // closed or this process detached from it). Every later wait would fail the same way at once.
    if (waitResult == WAIT_FAILED)
    {
        _inputClosed = true;
        return {};
    }

    auto events = std::vector<InputEvent> {};

    if (auto const resize = drainResize())
        events.emplace_back(*resize);

    auto parsed = readReadyInput();
    events.insert(
        events.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));

    return events;
}

auto TerminalInput::inputNativeHandle() const noexcept -> core::platform::NativeHandle
{
    return _native->stdinHandle;
}

auto TerminalInput::resizeNativeHandle() const noexcept -> core::platform::NativeHandle
{
    return _native->resizeEvent;
}

auto TerminalInput::drainResize() -> std::optional<ResizeEvent>
{
    if (_native->resizeEvent == nullptr || WaitForSingleObject(_native->resizeEvent, 0) != WAIT_OBJECT_0)
        return std::nullopt;

    ResetEvent(_native->resizeEvent);

    CONSOLE_SCREEN_BUFFER_INFO csbi {};
    if (GetConsoleScreenBufferInfo(_native->stdoutHandle, &csbi))
    {
        auto const cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        auto const rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return ResizeEvent { .columns = static_cast<int>(cols), .rows = static_cast<int>(rows) };
    }
    return std::nullopt;
}

auto TerminalInput::probeEnd() -> EndProbe
{
    // A console whose host process died is never signalled again, so no wait on it returns and
    // readReadyInput below is never reached to find the end. Every call on the handle fails at once
    // instead -- ERROR_PIPE_NOT_CONNECTED, measured against the in-box conhost on Windows 11 --
    // and asking for the pending event count is the cheapest of them: it reads nothing.
    //
    // Only the answers that say the console is gone end the input, because this is asked every
    // second for the life of the session and the end is for good: any other failure is taken as
    // passing, and asked again next time.
    constexpr auto ConsoleGone = std::array<DWORD, 3> {
        ERROR_PIPE_NOT_CONNECTED, // its host process died
        ERROR_BROKEN_PIPE,        // its host is going away
        ERROR_INVALID_HANDLE,     // the console was closed, or this process detached from it
    };
    if (_inputClosed)
        return EndProbe::Ended;
    auto pending = DWORD { 0 };
    if (GetNumberOfConsoleInputEvents(_native->stdinHandle, &pending) != 0)
        return EndProbe::Open;
    if (std::ranges::find(ConsoleGone, GetLastError()) == ConsoleGone.end())
        return EndProbe::Open;
    _inputClosed = true;
    return EndProbe::Ended;
}

auto TerminalInput::readReadyInput() -> std::vector<InputEvent>
{
    auto events = std::vector<InputEvent> {};

    // Process console input records. A console handle that can no longer be asked is the POSIX
    // hangup's equivalent (core-cpp#49): the console was closed, or this process detached from it
    // with FreeConsole(), and the handle it held is invalid. The wait on it fails at once rather
    // than blocking, and the backend reports that failure as readiness, so a caller that did not
    // stop here would be woken for it every turn.
    DWORD numEvents = 0;
    if (!GetNumberOfConsoleInputEvents(_native->stdinHandle, &numEvents))
    {
        _inputClosed = true;
        return events;
    }
    if (numEvents == 0)
        return events;

    auto inputRecords = std::vector<INPUT_RECORD>(numEvents);
    DWORD eventsRead = 0;
    if (!ReadConsoleInput(_native->stdinHandle, inputRecords.data(), numEvents, &eventsRead))
    {
        _inputClosed = true;
        return events;
    }

    // Accumulate character data from KEY_EVENT records, then feed to VT parser.
    // With ENABLE_VIRTUAL_TERMINAL_INPUT, Windows Terminal sends CSI escape sequences
    // identical to Linux terminals, so the VtParser pipeline works unchanged.
    auto vtData = std::string {};
    for (auto const& rec: std::span(inputRecords).first(eventsRead))
    {
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
        {
            auto const wc = rec.Event.KeyEvent.uChar.UnicodeChar;
            if (wc != 0)
                _native->utf16.append(vtData, static_cast<char16_t>(wc));
        }
        else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT)
        {
            // Also handle resize events from input records
            CONSOLE_SCREEN_BUFFER_INFO csbi {};
            if (GetConsoleScreenBufferInfo(_native->stdoutHandle, &csbi))
            {
                auto const cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
                auto const rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
                events.emplace_back(
                    ResizeEvent { .columns = static_cast<int>(cols), .rows = static_cast<int>(rows) });
            }
        }
    }

    // Feed accumulated VT data to the parser
    if (!vtData.empty())
    {
        auto parsed = _parser.feed(vtData);
        events.insert(
            events.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    }

    return events;
}

void TerminalInput::notifyResize(int /*cols*/, int /*rows*/)
{
    if (_native->resizeEvent != nullptr)
        SetEvent(_native->resizeEvent);
}

auto TerminalInput::resizePipeReadFd() const noexcept -> int
{
    return -1; // Not applicable on Windows
}

void TerminalInput::enableRawMode()
{
    // Enable VT input processing, disable line input, echo, and processed input.
    // With ENABLE_VIRTUAL_TERMINAL_INPUT, the console sends CSI escape sequences
    // for special keys, matching the behavior of Linux terminals.
    //
    // ENABLE_WINDOW_INPUT is the documented condition for the console to put WINDOW_BUFFER_SIZE_EVENT
    // records in the input buffer, and those records are the only resize path this arm has: nothing on
    // Windows signals the resize event, which is the POSIX arm's SIGWINCH. It is asked for so a host
    // that follows the documentation reports a size change too. It is NOT what makes resizing work on
    // the hosts measured: on Windows 11 (26200), conhost and Windows Terminal both delivered a window
    // resize to this arm with the flag cleared, in VT input mode.
    DWORD const inputMode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
    SetConsoleMode(_native->stdinHandle, inputMode);

    // Enable VT output processing and disable automatic newline translation
    DWORD const outputMode =
        ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(_native->stdoutHandle, outputMode);

    // Drive the console in UTF-8 so the UTF-8 bytes we write render correctly and typed
    // Unicode is delivered as UTF-8.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    _rawMode = true;
}

void TerminalInput::disableRawMode()
{
    if (_rawMode)
    {
        SetConsoleMode(_native->stdinHandle, _native->originalInputMode);
        SetConsoleMode(_native->stdoutHandle, _native->originalOutputMode);
        if (_native->originalOutputCp != 0)
            SetConsoleOutputCP(_native->originalOutputCp);
        if (_native->originalInputCp != 0)
            SetConsoleCP(_native->originalInputCp);
        _rawMode = false;
    }
}

void TerminalInput::writeProtocol(std::string_view data) const
{
    DWORD written = 0;
    WriteFile(_native->stdoutHandle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
}

void TerminalInput::enableProtocols()
{
    writeProtocol(protocols::EnableWin32InputMode);
    writeProtocol(protocols::EnableCsiU);
    writeProtocol(protocols::EnablePassiveMouseTracking);
    writeMouseTrackingChange(MouseTracking::Off, mouseTracking());
    writeProtocol(protocols::EnableBracketedPaste);
    writeProtocol(protocols::EnableColorSchemeNotify);
    writeProtocol(protocols::QueryColorScheme);
}

void TerminalInput::disableProtocols()
{
    writeProtocol(protocols::DisableColorSchemeNotify);
    writeProtocol(protocols::DisableBracketedPaste);
    writeMouseTrackingChange(mouseTracking(), MouseTracking::Off);
    writeProtocol(protocols::DisablePassiveMouseTracking);
    writeProtocol(protocols::DisableWin32InputMode);
    writeProtocol(protocols::DisableCsiU);
}

} // namespace core::tui
