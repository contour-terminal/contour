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
#include <vtpty/LinuxPty.h>
#include <vtpty/Process.h>
#include <vtpty/UnixUtils.h>

#include <crispy/deferred.h>
#include <crispy/escape.h>
#include <crispy/logstore.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <unistd.h>

#include "crispy/BufferObject.h"

#if !defined(FLATPAK)
    #include <utempter.h>
#endif

using std::array;
using std::max;
using std::min;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::runtime_error;
using std::scoped_lock;
using std::string_view;
using std::tuple;

using namespace std::string_literals;

namespace terminal
{

namespace
{
    LinuxPty::PtyHandles createLinuxPty(PageSize const& windowSize, optional<crispy::image_size> pixels)
    {
        // See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
        assert(*windowSize.lines <= numeric_limits<unsigned short>::max());
        assert(*windowSize.columns <= numeric_limits<unsigned short>::max());

        winsize const ws { unbox<unsigned short>(windowSize.lines),
                           unbox<unsigned short>(windowSize.columns),
                           unbox<unsigned short>(pixels.value_or(crispy::image_size {}).width),
                           unbox<unsigned short>(pixels.value_or(crispy::image_size {}).height) };

#if defined(__APPLE__)
        winsize* wsa = const_cast<winsize*>(&ws);
#else
        winsize const* wsa = &ws;
#endif

        // TODO: termios term{};
        int masterFd {};
        int slaveFd {};
        if (openpty(&masterFd, &slaveFd, nullptr, /*&term*/ nullptr, (winsize*) wsa) < 0)
            throw runtime_error { "Failed to open PTY. "s + strerror(errno) };

        PtyLog()("PTY opened. master={}, slave={}", masterFd, slaveFd);

        return { PtyMasterHandle::cast_from(masterFd), PtySlaveHandle::cast_from(slaveFd) };
    }

    char const* hostnameForUtmp()
    {
        for (auto const* env: { "DISPLAY", "WAYLAND_DISPLAY" })
            if (auto const* value = std::getenv(env))
                return value;

        return nullptr;
    }

} // namespace

// {{{ LinuxPty::Slave
LinuxPty::Slave::~Slave()
{
    close();
}

PtySlaveHandle LinuxPty::Slave::handle() const noexcept
{
    return PtySlaveHandle::cast_from(_slaveFd);
}

void LinuxPty::Slave::close()
{
    detail::saveClose(&_slaveFd);
}

bool LinuxPty::Slave::isClosed() const noexcept
{
    return _slaveFd == -1;
}

bool LinuxPty::Slave::configure() noexcept
{
    auto const tio = detail::constructTerminalSettings(_slaveFd);
    if (tcsetattr(_slaveFd, TCSANOW, &tio) == 0)
        tcflush(_slaveFd, TCIOFLUSH);
    return true;
}

bool LinuxPty::Slave::login()
{
    if (_slaveFd < 0)
        return false;

    if (!configure())
        return false;

    sigset_t signals;
    sigemptyset(&signals);
    sigprocmask(SIG_SETMASK, &signals, nullptr);

    // clang-format off
    struct sigaction act {};
    act.sa_handler = SIG_DFL;
    // clang-format on

    for (auto const signo: { SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM })
        sigaction(signo, &act, nullptr);

    // This is doing what login_tty() is doing, too.
    // But doing it ourselfs allows for a little more flexibility.
    // return login_tty(_slaveFd) == 0;

    setsid();

#if defined(TIOCSCTTY)
    // Set controlling terminal.
    // However, Flatpak is having issues with that, so we sadly have to avoid that then.
    if (!Process::isFlatpak())
    {
        if (ioctl(_slaveFd, TIOCSCTTY, nullptr) == -1)
            return false;
    }
#endif

    for (int const fd: { 0, 1, 2 })
    {
        if (_slaveFd != fd)
            ::close(fd);
        detail::saveDup2(_slaveFd, fd);
    }

    if (_slaveFd > 2)
        detail::saveClose(&_slaveFd);

    return true;
}

int LinuxPty::Slave::write(std::string_view text) noexcept
{
    if (_slaveFd < 0)
    {
        errno = ENODEV;
        return -1;
    }

    auto const rv = ::write(_slaveFd, text.data(), text.size());
    return static_cast<int>(rv);
}
// }}}

LinuxPty::LinuxPty(PageSize pageSize, optional<crispy::image_size> pixels):
    _pageSize { pageSize }, _pixels { pixels }
{
}

void LinuxPty::start()
{
    auto const handles = createLinuxPty(_pageSize, _pixels);
    _masterFd = unbox<int>(handles.master);
    _slave = std::make_unique<Slave>(handles.slave);

    if (!detail::setFileFlags(_masterFd, O_CLOEXEC | O_NONBLOCK))
        throw runtime_error { "Failed to configure PTY. "s + strerror(errno) };

    detail::setFileFlags(_stdoutFastPipe.reader(), O_NONBLOCK);
    PtyLog()("stdout fastpipe: reader {}, writer {}", _stdoutFastPipe.reader(), _stdoutFastPipe.writer());

    _eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_eventFd < 0)
        throw runtime_error { "Failed to create eventfd. "s + strerror(errno) };

    _epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (_epollFd < 0)
        throw runtime_error { "Failed to create epoll handle. "s + strerror(errno) };

    auto ev = epoll_event {};
    ev.events = EPOLLIN;
    ev.data.fd = _masterFd;
    if (epoll_ctl(_epollFd, EPOLL_CTL_ADD, _masterFd, &ev) < 0)
        throw runtime_error { "epoll setup failed to add PTY master fd. "s + strerror(errno) };

    ev.data.fd = _eventFd;
    if (epoll_ctl(_epollFd, EPOLL_CTL_ADD, _eventFd, &ev) < 0)
        throw runtime_error { "epoll setup failed to add eventfd. "s + strerror(errno) };

    ev.data.fd = _stdoutFastPipe.reader();
    if (epoll_ctl(_epollFd, EPOLL_CTL_ADD, _stdoutFastPipe.reader(), &ev) < 0)
        throw runtime_error { "epoll setup failed to add stdout-fastpipe. "s + strerror(errno) };

#if !defined(FLATPAK)
    utempter_add_record(_masterFd, hostnameForUtmp());
#endif
}

LinuxPty::~LinuxPty()
{
    PtyLog()("PTY destroying master (file descriptor {}).", _masterFd);
#if !defined(FLATPAK)
    utempter_remove_record(_masterFd);
#endif
    detail::saveClose(&_eventFd);
    detail::saveClose(&_epollFd);
    detail::saveClose(&_masterFd);
}

PtySlave& LinuxPty::slave() noexcept
{
    assert(_slave);
    return *_slave;
}

PtyMasterHandle LinuxPty::handle() const noexcept
{
    return PtyMasterHandle::cast_from(_masterFd);
}

void LinuxPty::close()
{
    PtyLog()("PTY closing master (file descriptor {}).", _masterFd);
    detail::saveClose(&_masterFd);
    wakeupReader();
}

bool LinuxPty::isClosed() const noexcept
{
    return _masterFd == -1;
}

void LinuxPty::wakeupReader() noexcept
{
    uint64_t dummy {};
    auto const rv = ::write(_eventFd, &dummy, sizeof(dummy));
    (void) rv;
}

optional<string_view> LinuxPty::readSome(int fd, char* target, size_t n) noexcept
{
    auto const rv = static_cast<int>(::read(fd, target, n));
    if (rv < 0)
    {
        if (errno != EAGAIN && errno != EINTR)
            errorlog()("{} read failed: {}", fd == _masterFd ? "master" : "stdout-fastpipe", strerror(errno));
        return nullopt;
    }

    if (PtyInLog)
        PtyInLog()("{} received: \"{}\"",
                   fd == _masterFd ? "master" : "stdout-fastpipe",
                   crispy::escape(target, target + rv));

    if (rv == 0 && fd == _stdoutFastPipe.reader())
    {
        PtyInLog()("Closing stdout-fastpipe.");
        _stdoutFastPipe.closeReader();
        errno = EAGAIN;
        return nullopt;
    }

    return string_view { target, static_cast<size_t>(rv) };
}

int LinuxPty::waitForReadable(std::chrono::milliseconds timeout) noexcept
{
    if (_masterFd < 0)
    {
        if (PtyInLog)
            PtyInLog()("read() called with closed PTY master.");
        errno = ENODEV;
        return -1;
    }

    auto epollEvents = array<epoll_event, 64> { {} };

    for (;;)
    {
        int const rv =
            epoll_wait(_epollFd, epollEvents.data(), epollEvents.size(), static_cast<int>(timeout.count()));

        if (rv == 0)
        {
            errno = EAGAIN;
            return -1;
        }

        if (rv < 0)
        {
            PtyInLog()("PTY read() failed. {}", strerror(errno));
            return -1;
        }

        bool piped = false;
        for (size_t i = 0; i < static_cast<size_t>(rv); ++i)
        {
            if (epollEvents[i].data.fd == _eventFd)
            {
                uint64_t dummy {};
                piped = ::read(_eventFd, &dummy, sizeof(dummy)) > 0;
            }

            if (epollEvents[i].data.fd == _stdoutFastPipe.reader())
                return _stdoutFastPipe.reader();

            if (epollEvents[i].data.fd == _masterFd)
                return _masterFd;
        }

        if (piped)
        {
            errno = EINTR;
            return -1;
        }
    }
}

Pty::ReadResult LinuxPty::read(crispy::buffer_object<char>& storage,
                               std::chrono::milliseconds timeout,
                               size_t size)
{
    if (int fd = waitForReadable(timeout); fd != -1)
    {
        auto const _l = scoped_lock { storage };
        if (auto x = readSome(fd, storage.hotEnd(), min(size, storage.bytesAvailable())))
            return { tuple { x.value(), fd == _stdoutFastPipe.reader() } };
    }

    return nullopt;
}

int LinuxPty::write(string_view data, bool blocking)
{
    auto const* buf = data.data();
    auto const size = data.size();

    if (blocking)
    {
        detail::setFileBlocking(_masterFd, true);
        auto const rv = ::write(_masterFd, buf, size);
        detail::setFileBlocking(_masterFd, false);
        if (PtyOutLog)
            PtyOutLog()("Sending bytes: \"{}\"", crispy::escape(buf, buf + rv));
        return static_cast<int>(rv);
    }

    timeval tv {};
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    fd_set rfd;
    fd_set wfd;
    fd_set efd;
    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    FD_ZERO(&efd);
    FD_SET(_masterFd, &wfd);
    FD_SET(_eventFd, &rfd);
    auto const nfds = 1 + max(_masterFd, _eventFd);

    if (select(nfds, &rfd, &wfd, &efd, &tv) < 0)
        return -1;

    if (!FD_ISSET(_masterFd, &wfd))
    {
        PtyOutLog()("PTY write of {} bytes timed out.\n", size);
        return 0;
    }

    ssize_t rv = ::write(_masterFd, buf, size);
    if (PtyOutLog)
    {
        if (rv >= 0)
            PtyOutLog()("Sending bytes: \"{}\"", crispy::escape(buf, buf + rv));

        if (rv < 0)
            // errorlog()("PTY write failed: {}", strerror(errno));
            PtyOutLog()("PTY write of {} bytes failed. {}\n", size, strerror(errno));
        else if (0 <= rv && static_cast<size_t>(rv) < size)
            // clang-format off
            PtyOutLog()("Partial write. {} bytes written and {} bytes left.",
                        rv,
                        size - static_cast<size_t>(rv));
        // clang-format on
    }

    return static_cast<int>(rv);
}

PageSize LinuxPty::pageSize() const noexcept
{
    return _pageSize;
}

void LinuxPty::resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels)
{
    if (_masterFd < 0)
        return;

    auto w = winsize {};
    w.ws_col = unbox<unsigned short>(cells.columns);
    w.ws_row = unbox<unsigned short>(cells.lines);

    if (pixels.has_value())
    {
        w.ws_xpixel = unbox<unsigned short>(pixels.value().width);
        w.ws_ypixel = unbox<unsigned short>(pixels.value().height);
    }

    if (ioctl(_masterFd, TIOCSWINSZ, &w) == -1)
        throw runtime_error { strerror(errno) };

    _pageSize = cells;
}

} // namespace terminal
