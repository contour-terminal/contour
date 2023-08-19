// SPDX-License-Identifier: Apache-2.0
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

namespace terminal
{

struct UnixPipe
{
    int pfd[2];

    explicit UnixPipe(unsigned flags = 0);
    UnixPipe(UnixPipe&&) noexcept;
    UnixPipe& operator=(UnixPipe&&) noexcept;
    UnixPipe(UnixPipe const&) = delete;
    UnixPipe& operator=(UnixPipe const&) = delete;
    ~UnixPipe();

    [[nodiscard]] bool good() const noexcept { return pfd[0] != -1 && pfd[1] != -1; }

    [[nodiscard]] int reader() const noexcept { return pfd[0]; }
    [[nodiscard]] int writer() const noexcept { return pfd[1]; }

    void closeReader() noexcept;
    void closeWriter() noexcept;

    void close();
};

// {{{ UnixPipe
inline UnixPipe::UnixPipe(UnixPipe&& v) noexcept: pfd { v.pfd[0], v.pfd[1] }
{
    v.pfd[0] = -1;
    v.pfd[1] = -1;
}

inline UnixPipe& UnixPipe::operator=(UnixPipe&& v) noexcept
{
    close();
    pfd[0] = v.pfd[0];
    pfd[1] = v.pfd[1];
    v.pfd[0] = -1;
    v.pfd[1] = -1;
    return *this;
}

inline UnixPipe::~UnixPipe()
{
    close();
}

inline void UnixPipe::close()
{
    closeReader();
    closeWriter();
}

inline void UnixPipe::closeReader() noexcept
{
    detail::saveClose(&pfd[0]);
}

inline void UnixPipe::closeWriter() noexcept
{
    detail::saveClose(&pfd[1]);
}
// }}}

} // namespace terminal
