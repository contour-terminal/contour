/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#pragma once

#include <cerrno>

#if defined(__APPLE__)
    #include <util.h>
#else
    #include <termios.h>
#endif

#include <fcntl.h>
#include <unistd.h>

namespace terminal::detail
{

termios getTerminalSettings(int fd) noexcept;
termios constructTerminalSettings(int fd) noexcept;
bool applyTerminalSettings(int fd, termios const& tio);
bool setFileFlags(int fd, int flags) noexcept;
void saveClose(int* fd) noexcept;
void saveDup2(int a, int b) noexcept;

// {{{ impl
inline termios getTerminalSettings(int fd) noexcept
{
    termios tio {};
    tcgetattr(fd, &tio);
    return tio;
}

inline bool applyTerminalSettings(int fd, termios const& tio)
{
    if (tcsetattr(fd, TCSANOW, &tio) == 0)
        tcflush(fd, TCIOFLUSH);
    return true;
}

inline termios constructTerminalSettings(int fd) noexcept
{
    auto tio = getTerminalSettings(fd);

    // input flags
#if defined(IUTF8)
    // Input is UTF-8; this allows character-erase to be properly applied in cooked mode.
    tio.c_iflag |= IUTF8;
#endif

    // special characters
    tio.c_cc[VMIN] = 1;  // Report as soon as 1 character is available.
    tio.c_cc[VTIME] = 0; // Disable timeout (no need).
    // tio.c_iflag &= ~ISTRIP; // Disable stripping 8th bit off the bytes.
    // tio.c_iflag &= ~INLCR;  // Disable NL-to-CR mapping
    // tio.c_iflag &= ~ICRNL;  // Disable CR-to-NL mapping
    // tio.c_iflag &= ~IXON;   // Disable control flow
    // tio.c_iflag &= ~BRKINT; // disable signal-break handling

    return tio;
}

inline bool setFileFlags(int fd, int flags) noexcept
{
    int currentFlags {};
    if (fcntl(fd, F_GETFL, &currentFlags) < 0)
        return false;
    if (fcntl(fd, F_SETFL, currentFlags | flags) < 0)
        return false;
    return true;
}

inline void setFileBlocking(int fd, bool blocking) noexcept
{
    setFileFlags(fd, blocking ? 0 : O_NONBLOCK);
}

inline void saveClose(int* fd) noexcept
{
    if (fd && *fd != -1)
    {
        ::close(*fd);
        *fd = -1;
    }
}

inline void saveDup2(int a, int b) noexcept
{
    while (dup2(a, b) == -1 && (errno == EBUSY || errno == EINTR))
        ;
}
// }}}

} // namespace terminal::detail
