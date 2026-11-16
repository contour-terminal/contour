// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalOutput.hpp>

#include <core/tui/detail/Utf16ToUtf8.hpp>
#include <core/tui/detail/XtVersion.hpp>

#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <windows.h>

/// @file
/// What @c TerminalOutput does through the operating system on Windows: the write
/// itself, the console size, the XTVERSION probe and whether standard output is a
/// console. Everything that only composes bytes is in the shared
/// `TerminalOutput.cpp`.

namespace core::tui
{

namespace
{
    /// @brief Queries the terminal for XTVERSION and returns the response.
    ///
    /// Sends CSI > q and reads the DCS response with a short timeout.
    /// Response format: DCS > | <terminal-name-and-version> ST
    ///
    /// @param timeoutMs Timeout in milliseconds to wait for response.
    /// @return Terminal identification string, or empty if not supported/timeout.
    auto queryXtVersion(int timeoutMs = 100) -> std::string
    {
        auto const hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
        auto const hStdin = GetStdHandle(STD_INPUT_HANDLE);

        if (hStdout == INVALID_HANDLE_VALUE || hStdin == INVALID_HANDLE_VALUE)
            return {};

        // Save current console mode and set raw mode for reliable reading
        DWORD origMode = 0;
        GetConsoleMode(hStdin, &origMode);

        DWORD const rawMode = ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(hStdin, rawMode);

        // Send XTVERSION query: CSI > q
        static constexpr auto Query = "\033[>q";
        DWORD written = 0;
        WriteFile(hStdout, Query, static_cast<DWORD>(std::strlen(Query)), &written, nullptr);

        std::string response;
        auto utf16 = detail::Utf16ToUtf8 {};

        // Wait for response with timeout, processing input records
        auto const deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);

        while (true)
        {
            auto const now = GetTickCount64();
            if (now >= deadline)
                break;

            auto const remaining = static_cast<DWORD>(deadline - now);
            auto const waitResult = WaitForSingleObject(hStdin, remaining);

            if (waitResult != WAIT_OBJECT_0)
                break;

            DWORD numEvents = 0;
            if (!GetNumberOfConsoleInputEvents(hStdin, &numEvents) || numEvents == 0)
                break;

            auto records = std::vector<INPUT_RECORD>(numEvents);
            DWORD eventsRead = 0;
            if (!ReadConsoleInput(hStdin, records.data(), numEvents, &eventsRead))
                break;

            for (auto const& record: std::span(records).first(eventsRead))
            {
                if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
                {
                    auto const wc = record.Event.KeyEvent.uChar.UnicodeChar;
                    if (wc != 0)
                        utf16.append(response, static_cast<char16_t>(wc));
                }
            }

            // Check for ST (String Terminator): ESC \ or 0x9C
            if (response.contains("\033\\") || response.contains('\x9C'))
                break;
        }

        // Restore console mode
        SetConsoleMode(hStdin, origMode);

        return response;
    }
} // namespace

auto TerminalOutput::initialize() -> VoidResult
{
    updateDimensions();
    detectCapabilities();
    return {};
}

void TerminalOutput::detectCapabilities()
{
    _unscrollSupported = detail::supportsUnscroll(detail::parseXtVersionName(queryXtVersion()));
}

void TerminalOutput::writeToDestination(std::string_view bytes)
{
    auto const hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(hStdout, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

bool TerminalOutput::isTerminal() const noexcept
{
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

void TerminalOutput::updateDimensions()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi {};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
    {
        _cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        _rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
}

} // namespace core::tui
