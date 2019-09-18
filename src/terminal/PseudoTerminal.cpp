/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/PseudoTerminal.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if !defined(_MSC_VER)
#include <pty.h>
#include <utmp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std;

namespace {
    string GetLastErrorAsString()
    {
#if defined(__unix__)
        return strerror(errno);
#else
        DWORD errorMessageID = GetLastError();
        if (errorMessageID == 0)
            return "";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorMessageID,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)& messageBuffer,
            0,
            nullptr
        );

        string message(messageBuffer, size);

        LocalFree(messageBuffer);

        return message;
#endif
    }
} // anonymous namespace

namespace terminal {

WindowSize currentWindowSize()
{
#if defined(__unix__)
    auto w = winsize{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
        throw runtime_error{strerror(errno)};

    return WindowSize{w.ws_col, w.ws_row};
#else
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    HANDLE hConsole{ GetStdHandle(STD_OUTPUT_HANDLE) };
    if (GetConsoleScreenBufferInfo(hConsole, &csbi))
        return WindowSize{
            static_cast<unsigned short>(csbi.srWindow.Right - csbi.srWindow.Left + 1),
            static_cast<unsigned short>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)
    };
    else
        throw runtime_error{GetLastErrorAsString()};
#endif
}

PseudoTerminal::PseudoTerminal(WindowSize const& _windowSize) :
    size_{ _windowSize }
{
#if defined(__unix__)
    // See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
    assert(_windowSize.rows <= numeric_limits<unsigned short>::max());
    assert(_windowSize.columns <= numeric_limits<unsigned short>::max());
    winsize const ws{
        static_cast<unsigned short>(_windowSize.rows),
        static_cast<unsigned short>(_windowSize.columns),
        0,
        0
    };
    // TODO: termios term{};
    if (openpty(&master_, &slave_, nullptr, /*&term*/ nullptr, &ws) < 0)
        throw runtime_error{ "Failed to open PTY. " + GetLastErrorAsString() };
#else
    master_ = INVALID_HANDLE_VALUE;
    input_ = INVALID_HANDLE_VALUE;
    output_ = INVALID_HANDLE_VALUE;

    HANDLE hPipePTYIn{ INVALID_HANDLE_VALUE };
    HANDLE hPipePTYOut{ INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPTY will connect to
    if (!CreatePipe(&hPipePTYIn, &output_, NULL, 0))
        throw runtime_error{ GetLastErrorAsString() };

    if (!CreatePipe(&input_, &hPipePTYOut, NULL, 0))
    {
        CloseHandle(hPipePTYIn);
        throw runtime_error{ GetLastErrorAsString() };
    }

    // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
    HRESULT hr = CreatePseudoConsole(
        { static_cast<SHORT>(_windowSize.columns), static_cast<SHORT>(_windowSize.rows) },
        hPipePTYIn,
        hPipePTYOut,
        0,
        &master_
    );

    if (hPipePTYIn != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYIn);

    if (hPipePTYOut != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYOut);

    if (hr != S_OK)
        throw runtime_error{ GetLastErrorAsString() };
#endif
}

PseudoTerminal::~PseudoTerminal()
{
    close();
}

void PseudoTerminal::close()
{
#if defined(__unix__)
    if (master_ >= 0)
    {
        ::close(master_);
        master_ = -1;
    }
#else
    if (master_ != INVALID_HANDLE_VALUE)
    {
        ClosePseudoConsole(master_);
        master_ = INVALID_HANDLE_VALUE;
    }
    if (input_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(input_);
        input_ = INVALID_HANDLE_VALUE;
    }
    if (output_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(output_);
        output_ = INVALID_HANDLE_VALUE;
    }
#endif
}

auto PseudoTerminal::read(char* buf, size_t size) -> ssize_t
{
#if defined(__unix__)
    return ::read(master_, buf, size);
#else
    DWORD nread{};
    if (ReadFile(input_, buf, static_cast<DWORD>(size), &nread, nullptr))
        return nread;
    else
        return -1;
#endif
}

auto PseudoTerminal::write(char const* buf, size_t size) -> ssize_t
{
#if defined(__unix__)
    return ::write(master_, buf, size);
#else
    DWORD nwritten{};
    if (WriteFile(output_, buf, static_cast<DWORD>(size), &nwritten, nullptr))
        return nwritten;
    else
        return -1;
#endif
}

WindowSize PseudoTerminal::size() const noexcept
{
    return size_;
}

void PseudoTerminal::resize(WindowSize const& _newWindowSize)
{
#if defined(__unix__)
    auto w = winsize{};
    w.ws_col = _newWindowSize.columns;
    w.ws_row = _newWindowSize.rows;

    if (ioctl(master_, TIOCSWINSZ, &w) == -1)
        throw runtime_error{strerror(errno)};
#elif defined(_MSC_VER)
    COORD coords;
    coords.X = _newWindowSize.columns;
    coords.Y = _newWindowSize.rows;
    HRESULT const result = ResizePseudoConsole(master_, coords);
    if (result != S_OK)
        throw runtime_error{ GetLastErrorAsString() };
#endif
}

}  // namespace terminal
