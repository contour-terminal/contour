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
#include <terminal/pty/UnixPty.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include <iostream>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <fcntl.h>
#include <utmp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

using crispy::Size;
using std::runtime_error;
using std::numeric_limits;
using std::optional;
using std::max;
using namespace std::string_literals;

namespace terminal {

namespace
{
    static termios getTerminalSettings(int fd)
    {
        termios tio{};
        tcgetattr(fd, &tio);
        return tio;
    }

    static termios constructTerminalSettings(int fd)
    {
        auto tio = getTerminalSettings(fd);

        // input flags
        tio.c_iflag |= IGNBRK;    // Ignore Break condition on input.
#if defined(IUTF8)
        tio.c_iflag |= IUTF8;     // Input is UTF-8; this allows character-erase to be properly applied in cooked mode.
#endif
#if defined(IUCLC)
        tio.c_iflag &= IUCLC;     // Map uppercase characters to lowercase on input (not in POSIX).
#endif
        tio.c_iflag &= ~IXON;     // Disable CTRL-S / CTRL-Q on output.
        tio.c_iflag &= ~IXOFF;    // Disable CTRL-S / CTRL-Q on input.
        tio.c_iflag &= ~ICRNL;    // Ensure CR isn't translated to NL.
        tio.c_iflag &= ~INLCR;    // Ensure NL isn't translated to CR.
        tio.c_iflag &= ~IGNCR;    // Ensure CR isn't ignored.
        tio.c_iflag &= ~IMAXBEL;  // Ensure beeping on full input buffer isn't enabled.
        tio.c_iflag &= ~ISTRIP;   // Ensure stripping of 8th bit on input isn't enabled.

        // output flags
        tio.c_oflag &= ~OPOST;   // Don't enable implementation defined output processing.
        tio.c_oflag &= ~ONLCR;   // Don't map NL to CR-NL.
        tio.c_oflag &= ~OCRNL;   // Don't map CR to NL.
        tio.c_oflag &= ~ONLRET;  // Don't output CR.

        // control flags

        // local flags
        tio.c_lflag &= ~IEXTEN;  // Don't enable implementation defined input processing.
        tio.c_lflag &= ~ICANON;  // Don't enable line buffering (Canonical mode).
        tio.c_lflag &= ~ECHO;    // Don't echo input characters.
        tio.c_lflag &= ~ISIG;    // Don't generate signal upon receiving characters for
                                    // INTR, QUIT, SUSP, DSUSP.

        // special characters
        tio.c_cc[VMIN] = 1;   // Report as soon as 1 character is available.
        tio.c_cc[VTIME] = 0;  // Disable timeout (no need).

        return tio;
    }
}

UnixPty::UnixPty(Size const& _windowSize, optional<Size> _pixels) :
    size_{ _windowSize }
{
    // See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
    assert(_windowSize.height <= numeric_limits<unsigned short>::max());
    assert(_windowSize.width <= numeric_limits<unsigned short>::max());

    winsize const ws{
        static_cast<unsigned short>(_windowSize.height),
        static_cast<unsigned short>(_windowSize.width),
        static_cast<unsigned short>(_pixels.value_or(Size{}).width),
        static_cast<unsigned short>(_pixels.value_or(Size{}).height)
    };

#if defined(__APPLE__)
    winsize* wsa = const_cast<winsize*>(&ws);
#else
    winsize const* wsa = &ws;
#endif

    // TODO: termios term{};
    if (openpty(&master_, &slave_, nullptr, /*&term*/ nullptr, wsa) < 0)
        throw runtime_error{ "Failed to open PTY. "s + strerror(errno) };

#if defined(__linux__)
    if (pipe2(pipe_.data(), O_NONBLOCK /* | O_CLOEXEC | O_NONBLOCK*/) < 0)
        throw runtime_error{ "Failed to create PTY pipe. "s + strerror(errno) };
#else
    if (pipe(pipe_.data()) < 0)
        throw runtime_error{ "Failed to create PTY pipe. "s + strerror(errno) };
    for (auto const fd: pipe_)
    {
        int currentFlags{};
        if (fcntl(fd, F_GETFL, &currentFlags) < 0)
            break;
        if (fcntl(fd, F_SETFL, currentFlags | O_CLOEXEC | O_NONBLOCK) < 0)
            break;
    }
#endif
}

UnixPty::~UnixPty()
{
    close();
}

void UnixPty::close()
{
    for (auto* fd: {&pipe_.at(0), &pipe_.at(1), &master_, &slave_})
    {
        if (*fd < 0)
            continue;

        ::close(*fd);
        *fd = -1;
    }
}

void UnixPty::wakeupReader()
{
    char dummy{};
    auto const rv = ::write(pipe_[1], &dummy, sizeof(dummy));
    (void) rv;
}

int UnixPty::read(char* _buf, size_t _size, std::chrono::milliseconds _timeout)
{
    timeval tv{};
    tv.tv_sec = _timeout.count() / 1000;
    tv.tv_usec = (_timeout.count() % 1000) * 1000;

    for (;;)
    {
        fd_set rfd, wfd, efd;
        FD_ZERO(&rfd);
        FD_ZERO(&wfd);
        FD_ZERO(&efd);
        FD_SET(master_, &rfd);
        FD_SET(pipe_[0], &rfd);
        auto const nfds = 1 + max(master_, pipe_[0]);
        int rv = select(nfds, &rfd, &wfd, &efd, &tv);

        if (rv == 0)
        {
            errno = EAGAIN;
            return -1;
        }

        if (rv < 0)
            return -1;

        bool piped = false;
        if (FD_ISSET(pipe_[0], &rfd))
        {
            piped = true;
            int n = 0;
            for (bool done = false; !done; )
            {
                char dummy[256];
                rv = ::read(pipe_[0], dummy, sizeof(dummy));
                done = rv > 0;
                n += max(rv, 0);
            }
        }

        if (FD_ISSET(master_, &rfd))
            return static_cast<int>(::read(master_, _buf, _size));

        if (piped)
        {
            errno = EINTR;
            return -1;
        }
    }
}

int UnixPty::write(char const* buf, size_t size)
{
    ssize_t rv = ::write(master_, buf, size);
    return static_cast<int>(rv);
}

Size UnixPty::screenSize() const noexcept
{
    return size_;
}

void UnixPty::resizeScreen(Size _cells, std::optional<Size> _pixels)
{
    auto w = winsize{};
    w.ws_col = _cells.width;
    w.ws_row = _cells.height;

    if (_pixels.has_value())
    {
        w.ws_xpixel = _pixels.value().width;
        w.ws_ypixel = _pixels.value().height;
    }

    if (ioctl(master_, TIOCSWINSZ, &w) == -1)
        throw runtime_error{strerror(errno)};

    size_ = _cells;
}

void UnixPty::prepareParentProcess()
{
    if (slave_ < 0)
        return;

    ::close(slave_);
    slave_ = -1;
}

void UnixPty::prepareChildProcess()
{
    if (master_ < 0)
        return;

    ::close(master_);
    master_ = -1;

    auto const tio = constructTerminalSettings(slave_);
    if (tcsetattr(slave_, TCSANOW, &tio) == 0)
        tcflush(slave_, TCIOFLUSH);

    if (login_tty(slave_) < 0)
        _exit(EXIT_FAILURE);
}

}  // namespace terminal
