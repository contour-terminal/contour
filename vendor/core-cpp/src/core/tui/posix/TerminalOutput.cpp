// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalOutput.hpp>

#include <core/tui/detail/XtVersion.hpp>
#include <core/tui/posix/PosixIO.hpp>

#include <sys/ioctl.h>

#include <array>
#include <cstring>
#include <string>

#include <poll.h>
#include <unistd.h>

/// @file
/// What @c TerminalOutput does through the operating system on POSIX: the write
/// itself, the window size, the XTVERSION probe and whether standard output is a
/// terminal. Everything that only composes bytes is in the shared
/// `TerminalOutput.cpp`.

namespace core::tui
{

namespace
{
    /// @brief Queries the terminal for XTVERSION and returns the response.
    ///
    /// Sends CSI > q and reads the DCS response with a short timeout.
    /// Response format: DCS > | <terminal-name-and-version> ST
    /// Example: "\033P>|kitty(0.26.5)\033\\"
    ///
    /// @pre The terminal must already be in raw mode (ECHO off) before calling.
    ///
    /// @param timeoutMs Timeout in milliseconds to wait for response.
    /// @return Terminal identification string, or empty if not supported/timeout.
    auto queryXtVersion(int timeoutMs = 100) -> std::string
    {
        // Send XTVERSION query: CSI > q
        static constexpr auto Query = "\033[>q";
        safeWrite(STDOUT_FILENO, Query, std::strlen(Query));

        std::string response;
        std::array<char, 256> buffer {};

        // Poll for response with timeout
        auto pfd = pollfd { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };

        while (true)
        {
            int const ret = ::poll(&pfd, 1, timeoutMs);
            if (ret <= 0)
                break; // Timeout or error

            auto const n = safeRead(STDIN_FILENO, buffer.data(), buffer.size());
            if (n <= 0)
                break;

            response.append(buffer.data(), static_cast<std::size_t>(n));

            // Check for ST (String Terminator): ESC \ or 0x9C
            if (response.contains("\033\\") || response.contains('\x9C'))
                break;

            // Short timeout for subsequent reads
            timeoutMs = 10;
        }

        return response;
    }
} // namespace

auto TerminalOutput::initialize() -> VoidResult
{
    updateDimensions();
    return {};
}

void TerminalOutput::detectCapabilities()
{
    _unscrollSupported = detail::supportsUnscroll(detail::parseXtVersionName(queryXtVersion()));
}

void TerminalOutput::writeToDestination(std::string_view bytes)
{
    safeWrite(STDOUT_FILENO, bytes.data(), bytes.size());
}

bool TerminalOutput::isTerminal() const noexcept
{
    return ::isatty(STDOUT_FILENO) != 0;
}

void TerminalOutput::updateDimensions()
{
    auto ws = winsize {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
    {
        _cols = ws.ws_col;
        _rows = ws.ws_row;
    }
}

} // namespace core::tui
