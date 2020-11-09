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

using std::runtime_error;
using std::numeric_limits;
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

UnixPty::UnixPty(Size const& _windowSize) :
    size_{ _windowSize }
{
    // See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
    assert(_windowSize.height <= numeric_limits<unsigned short>::max());
    assert(_windowSize.width <= numeric_limits<unsigned short>::max());

    winsize const ws{
        static_cast<unsigned short>(_windowSize.height),
        static_cast<unsigned short>(_windowSize.width),
        0,
        0
    };

#if defined(__APPLE__)
    winsize* wsa = const_cast<winsize*>(&ws);
#else
    winsize const* wsa = &ws;
#endif

    // TODO: termios term{};
    if (openpty(&master_, &slave_, nullptr, /*&term*/ nullptr, wsa) < 0)
        throw runtime_error{ "Failed to open PTY. "s + strerror(errno) };
}

UnixPty::~UnixPty()
{
    close();
}

void UnixPty::close()
{
    if (master_ >= 0)
    {
        ::close(master_);
        master_ = -1;
    }

    if (slave_ >= 0)
    {
        ::close(slave_);
        slave_ = -1;
    }
}

int UnixPty::read(char* buf, size_t size)
{
    ssize_t rv = ::read(master_, buf, size);
    if (rv < 0 || rv >= static_cast<decltype(rv)>(size))
        return rv;
#if 1
    //size_t const cap = size;
    ssize_t nread = rv;
    int i = 1;
    size -= rv;
    buf += rv;

    auto const oldFlags = fcntl(master_, F_GETFL);
    fcntl(master_, F_SETFL, oldFlags | O_NONBLOCK);

    fd_set in, out, err;
    FD_ZERO(&in);
    FD_ZERO(&out);
    FD_ZERO(&err);
    FD_SET(master_, &in);

    while (size > 0)
    {
        ++i;
        timeval tv{0, 0};
        int const selected = select(master_ + 1, &in, &out, &err, &tv);
        if (selected <= 0)
            break;
        rv = ::read(master_, buf, size);
        if (rv < 0)
            break;
        nread += rv;
        size -= rv;
        buf += rv;
    }

    fcntl(master_, F_SETFL, oldFlags);
    //printf("pty.read: %zu/%zu bytes (%ld%%, #%d)\n", nread, cap, nread * 100 / cap, i);
    return nread;
#else
    return rv;
#endif
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

    auto const tio = constructTerminalSettings(master_);
    if (tcsetattr(master_, TCSANOW, &tio) == 0)
        tcflush(master_, TCIOFLUSH);

    if (login_tty(slave_) < 0)
        _exit(EXIT_FAILURE);
}

}  // namespace terminal
