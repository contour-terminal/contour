// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixPty.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/UnixUtils.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Deferred.hpp>
#include <core/Environment.hpp>
#include <core/Escape.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <tracy/Tracy.hpp>

#if defined(__APPLE__) || defined(__OpenBSD__)
    #include <util.h>
#elifdef __FreeBSD__
    #include <libutil.h>
#else
    #include <pty.h>
#endif

#include <fcntl.h>
#ifndef __FreeBSD__
    #include <utmp.h>
#endif
#include <sys/ioctl.h>
#include <sys/select.h>
#ifndef __APPLE__
    #include <poll.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>

#include <pthread.h>
#include <pwd.h>
#include <unistd.h>

#if defined(__linux__) && defined(UTEMPTER)
    #include <utempter.h>
#endif

using std::make_unique;
using std::nullopt;
using std::optional;
using std::runtime_error;
using std::scoped_lock;
using std::string_view;

using namespace std::string_literals;

namespace vtpty
{

namespace
{
    struct PtyHandles
    {
        PtyMasterHandle master;
        PtySlaveHandle slave;
    };

    PtyHandles createUnixPty(PageSize const& windowSize, optional<ImageSize> pixels)
    {
        // See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
        assert(std::cmp_less_equal(unbox(windowSize.lines), std::numeric_limits<unsigned short>::max()));
        assert(std::cmp_less_equal(unbox(windowSize.columns), std::numeric_limits<unsigned short>::max()));

        winsize const ws { .ws_row = unbox<unsigned short>(windowSize.lines),
                           .ws_col = unbox<unsigned short>(windowSize.columns),
                           .ws_xpixel = unbox<unsigned short>(pixels.value_or(ImageSize {}).width),
                           .ws_ypixel = unbox<unsigned short>(pixels.value_or(ImageSize {}).height) };

#ifdef __APPLE__
        auto* wsa = const_cast<winsize*>(&ws);
#else
        winsize const* wsa = &ws;
#endif

        // TODO: termios term{};
        int masterFd {};
        int slaveFd {};
        if (openpty(&masterFd, &slaveFd, nullptr, /*&term*/ nullptr, (winsize*) wsa) < 0)
            throw runtime_error { "Failed to open PTY. "s + std::generic_category().message(errno) };

        ptyLog()("PTY opened. master={}, slave={}", masterFd, slaveFd);

        return { .master = PtyMasterHandle::cast_from(masterFd),
                 .slave = PtySlaveHandle::cast_from(slaveFd) };
    }

    /// Waits until @p fd takes a write, or @p closeSignal turns readable.
    /// @param fd The master descriptor, as it was when the caller last held the lock close() takes: if it
    ///        has been closed since, @p closeSignal is readable already.
    /// @param closeSignal What close() writes to.
    /// @return False once @p closeSignal is readable, or the wait failed.
    [[nodiscard]] bool waitWritable(int fd, int closeSignal) noexcept
    {
#ifdef __APPLE__
        // macOS's poll() reports a terminal device invalid, so select() waits here, as the read
        // selector's does.
        while (true)
        {
            auto writers = fd_set {};
            auto readers = fd_set {};
            FD_ZERO(&writers);
            FD_ZERO(&readers);
            FD_SET(fd, &writers);
            FD_SET(closeSignal, &readers);
            auto const rv = ::select(std::max(fd, closeSignal) + 1, &readers, &writers, nullptr, nullptr);
            if (rv < 0 && errno == EINTR)
                continue;
            return rv > 0 && !FD_ISSET(closeSignal, &readers);
        }
#else
        auto fds = std::array { pollfd { .fd = fd, .events = POLLOUT, .revents = 0 },
                                pollfd { .fd = closeSignal, .events = POLLIN, .revents = 0 } };
        while (true)
        {
            auto const rv = ::poll(fds.data(), fds.size(), -1);
            if (rv < 0 && errno == EINTR)
                continue;
            return rv > 0 && (fds[1].revents & POLLIN) == 0;
        }
#endif
    }

#if defined(__linux__) && defined(UTEMPTER)
    /// @return The display name to record in utmp, or empty if this session has none.
    std::string hostnameForUtmp()
    {
        for (auto const* env: { "DISPLAY", "WAYLAND_DISPLAY" })
            if (auto value = core::defaultEnvironment().get(env))
                return std::move(*value);

        return {};
    }
#endif
} // namespace

// {{{ UnixPty::Slave
UnixPty::Slave::~Slave()
{
    close();
}

PtySlaveHandle UnixPty::Slave::handle() const noexcept
{
    return PtySlaveHandle::cast_from(_slaveFd.get());
}

void UnixPty::Slave::close()
{
    _slaveFd.close();
}

void UnixPty::waitForClosed()
{
    crispy::ReadSelector selector;
    selector.wantRead(_masterFd);
    while (true)
    {
        selector.waitOne();
        if (isClosed())
            break;

        std::this_thread::yield();
    }
}

bool UnixPty::Slave::isClosed() const noexcept
{
    return _slaveFd == -1;
}

bool UnixPty::Slave::configure() noexcept
{
    auto const tio = util::constructTerminalSettings(_slaveFd);
    if (tcsetattr(_slaveFd, TCSANOW, &tio) == 0)
        tcflush(_slaveFd, TCIOFLUSH);
    return true;
}

bool UnixPty::Slave::login()
{
    if (_slaveFd < 0)
        return false;

    if (!configure())
        return false;

    // This is doing what login_tty() is doing, too.
    // But doing it ourselves allows for a little more flexibility.
    // return login_tty(_slaveFd) == 0;

    sigset_t signals;
    sigemptyset(&signals);
    pthread_sigmask(SIG_SETMASK, &signals, nullptr);

    // clang-format off
    struct sigaction act {};
    act.sa_handler = SIG_DFL;
    // clang-format on

    for (auto const signo: { SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM })
        sigaction(signo, &act, nullptr);

    setsid();

#ifdef TIOCSCTTY
    // Set the controlling terminal, unless we are running inside a flatpak.
    // Because flatpak does not allow setting the controlling terminal.
    // - https://github.com/flatpak/flatpak/issues/3697
    // - https://github.com/flatpak/flatpak/issues/3285
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
        util::saveDup2(_slaveFd, fd);
    }

    if (_slaveFd > 2)
        _slaveFd.close();

    return true;
}

int UnixPty::Slave::write(std::string_view text) noexcept
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

UnixPty::UnixPty(PageSize pageSize, optional<ImageSize> pixels): _pageSize { pageSize }, _pixels { pixels }
{
}

StartResult UnixPty::start()
{
    auto const handles = createUnixPty(_pageSize, _pixels);
    _masterFd = crispy::FileDescriptor::fromNative(unbox<int>(handles.master));
    _masterState.store(MasterState::Open, std::memory_order_release);
    _slave = make_unique<Slave>(handles.slave);

    // Close-on-exec, so that no process this one spawns inherits the master: our own children close every
    // descriptor above stderr, but a QProcess or a URL opener does not. The stdout fast pipe stays
    // inheritable, as the child writes to it.
    if (auto const configured =
            util::setCloseOnExec(_masterFd)
                .and_then([this] { return util::setNonBlocking(_masterFd); })
                .and_then([this] { return util::setNonBlocking(_stdoutFastPipe.reader()); });
        !configured)
        return std::unexpected(
            StartFailure { .error = StartError::PtyAllocationFailed,
                           .detail = "Failed to configure PTY. "s + configured.error().message() });
    ptyLog()("stdout fastpipe: reader {}, writer {}", _stdoutFastPipe.reader(), _stdoutFastPipe.writer());

    _readSelector.wantRead(_masterFd);
    _readSelector.wantRead(_stdoutFastPipe.reader());

#if defined(__linux__) && defined(UTEMPTER)
    // Held in a local: utempter wants a C string, and a temporary's would dangle the moment the
    // full expression ended.
    auto const utmpHostname = hostnameForUtmp();
    utempter_add_record(_masterFd, utmpHostname.empty() ? nullptr : utmpHostname.c_str());
#endif

    return {};
}

UnixPty::~UnixPty()
{
    ptyLog()("PTY destroying master (file descriptor {}).", _masterFd);
}

PtySlave& UnixPty::slave() noexcept
{
    assert(started());
    return *_slave;
}

PtyMasterHandle UnixPty::handle() const noexcept
{
    return PtyMasterHandle::cast_from(_masterFd.get());
}

void UnixPty::close()
{
    auto const _ = std::scoped_lock { _mutex };
    if (_masterFd.isClosed())
    {
        ptyLog()("PTY closing master from thread {} (already closed).", core::threadName());
        return;
    }

    ptyLog()("PTY closing master from thread {} (file descriptor {}).", core::threadName(), _masterFd);
    _readSelector.cancelRead(_masterFd);
    _masterFd.close();
    _masterState.store(MasterState::Closed, std::memory_order_release);
    // Wakes a write waiting for the application to read, which then finds the descriptor closed. Never
    // drained, so that a write that only begins to wait after this returns at once as well.
    std::ignore = ::write(_closeSignal.writer(), "x", 1);
    wakeupReader();
}

InputEcho UnixPty::inputEcho() const noexcept
{
    if (_masterState.load(std::memory_order_acquire) == MasterState::Closed)
        return InputEcho::Unknown;
    auto const _ = std::scoped_lock { _mutex };
    if (_masterFd.isClosed())
        return InputEcho::Unknown;
    auto tio = termios {};
    if (::tcgetattr(_masterFd, &tio) != 0)
        return InputEcho::Unknown;
    return util::inputEchoOf(tio.c_lflag);
}

bool UnixPty::isClosed() const noexcept
{
    // Published by close(), rather than read under its lock: it is asked from the GUI thread, or a daemon's
    // loop, while a write may wait for an application that reads nothing.
    return _masterState.load(std::memory_order_acquire) == MasterState::Closed;
}

void UnixPty::wakeupReader() noexcept
{
    _readSelector.wakeup();
}

optional<string_view> UnixPty::readSome(int fd, char* target, size_t n) noexcept
{
    auto const rv = static_cast<int>(::read(fd, target, n));
    if (rv < 0)
    {
        // EAGAIN and EINTR mean "nothing to read right now". EIO means the far end is gone: a PTY
        // master whose last slave has closed reports that as EIO rather than a zero-length read, so it
        // is this layer's end-of-file and not a failure. Logging it as one puts an error in the log on
        // every normal child exit -- and made the VT conformance report differ between runs, depending
        // on whether the final read landed before or after the child was reaped.
        //
        // Callers do not need to distinguish: they already read `errno` themselves, and every value
        // other than EAGAIN/EINTR means the same thing to them (closed). @see Terminal::processInputOnce.
        if (errno != EAGAIN && errno != EINTR && errno != EIO)
            errorLog()("{} read failed: {}",
                       fd == _masterFd ? "master" : "stdout-fastpipe",
                       std::generic_category().message(errno));
        return nullopt;
    }

    if (ptyInLog)
        ptyInLog()("{} received: \"{}\"",
                   fd == _masterFd ? "master" : "stdout-fastpipe",
                   core::escape(target, target + rv));

    if (rv == 0 && fd == _stdoutFastPipe.reader())
    {
        ptyInLog()("Closing stdout-fastpipe.");
        _readSelector.cancelRead(fd);
        _stdoutFastPipe.closeReader();
        errno = EAGAIN;
        return nullopt;
    }

    return string_view { target, static_cast<size_t>(rv) };
}

std::optional<Pty::ReadResult> UnixPty::read(crispy::BufferObject<char>& storage,
                                             std::optional<std::chrono::milliseconds> timeout,
                                             size_t size)
{
    // This runs on the session's read thread while close() may run concurrently on the GUI thread
    // (e.g. when a GUI tab is closed). _readSelector is internally thread-safe: wantRead/cancelRead/
    // waitOne serialize their own access to the registered-fd set, so the blocking waitOne() is safe
    // to call unlocked -- and it MUST stay unlocked, or a concurrent close() would block trying to
    // cancelRead()/wakeup() and the teardown would deadlock. The wakeup writes a separate,
    // always-registered eventfd/break-pipe, so a blocked waitOne() is woken regardless of the
    // selector's fd contents.

    // Blocking wait. On an empty selector (master closed by a concurrent close() plus a prior
    // stdout-fastpipe EOF) waitOne() blocks on the break-pipe until the timeout or the next wakeup(),
    // rather than spinning: returning EAGAIN immediately here would make the caller's read loop busy-spin
    // a CPU core until the session is torn down.
    auto const fd = [&] {
        ZoneScopedN("pty.waitOne");
        return _readSelector.waitOne(timeout);
    }();
    if (!fd.has_value())
    {
        errno = EAGAIN;
        return std::nullopt;
    }

    // Locked read and bookkeeping. readSome() may cancelRead the stdout-fastpipe on EOF, so it mutates
    // shared state under _mutex. Re-validate first: if close() ran between the wait and here it removed
    // the master fd from the selector, so the returned fd is no longer wanted -- do not ::read() a
    // just-closed (and possibly already recycled) fd. A fastpipe fd that is still wanted remains
    // drainable. isWanted() reflects the current registration on both selector backends, so this
    // detects the concurrent close() that the stale `_masterFd.isClosed()` check could not.
    auto const _ = std::scoped_lock { _mutex };
    if (!_readSelector.isWanted(*fd))
    {
        errno = EAGAIN;
        return std::nullopt;
    }

    ZoneScopedN("pty.readSome");
    auto const l = scoped_lock { storage };
    if (auto x = readSome(*fd, storage.hotEnd(), std::min(size, storage.bytesAvailable())))
        return ReadResult { .data = x.value(), .fromStdoutFastPipe = *fd == _stdoutFastPipe.reader() };
    return std::nullopt;
}

int UnixPty::write(std::string_view data)
{
    // One write at a time, from its first byte to its last, so that two never interleave.
    auto const writing = std::scoped_lock { _writeMutex };

    auto const rv = writeAvailable(data);
    if (rv < 0 || std::cmp_equal(rv, data.size()))
        return rv;

    // A short write: the rest goes as the application reads, until all of it has or the PTY closes. The
    // waits release _mutex, so that close() -- the way to be rid of a program that reads nothing -- and
    // isClosed() never wait for that program; close() wakes them instead.
    auto written = static_cast<size_t>(rv);
    while (written < data.size() && awaitWritable())
    {
        auto const more = writeAvailable(data.substr(written));
        if (more > 0)
            written += static_cast<size_t>(more);
        else if (more < 0 && errno != EAGAIN && errno != EINTR)
            break;
    }
    return static_cast<int>(written);
}

int UnixPty::writeAvailable(std::string_view data)
{
    // Under the lock close() closes the descriptor under: never into a closed -- and possibly already
    // recycled -- descriptor, which would hand the bytes to another pane's PTY or an unrelated file.
    auto const _ = std::scoped_lock { _mutex };
    if (_masterFd.isClosed())
    {
        errno = EPIPE;
        return -1;
    }

    auto const* buf = data.data();
    auto const size = data.size();
    ssize_t const rv = ::write(_masterFd, buf, size);
    if (ptyOutLog)
    {
        if (rv >= 0)
            ptyOutLog()("Sending bytes: \"{}\"", core::escape(buf, buf + rv));

        if (rv < 0)
            ptyOutLog()("PTY write of {} bytes failed. {}\n", size, std::generic_category().message(errno));
        else if (std::cmp_less(rv, size))
            ptyOutLog()(
                "Partial write. {} bytes written and {} bytes left.", rv, size - static_cast<size_t>(rv));
    }
    return static_cast<int>(rv);
}

bool UnixPty::awaitWritable()
{
    auto const fd = [this] {
        auto const _ = std::scoped_lock { _mutex };
        return _masterFd.get();
    }();
    return fd >= 0 && waitWritable(fd, _closeSignal.reader());
}

PageSize UnixPty::pageSize() const noexcept
{
    return _pageSize;
}

void UnixPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    if (_masterFd < 0)
    {
        // Not started yet: remember the size rather than drop it, so start() can bake it into the
        // winsize the child is born with. Dropping it meant the child saw ws_xpixel = 0 until an
        // asynchronous SIGWINCH corrected it -- and an application that reads TIOCGWINSZ once at
        // startup, which is what img2sixel and chafa do, never sees the correction.
        ptyLog()("Recording pre-start terminal size: {}x{} / {}",
                 cells.columns,
                 cells.lines,
                 pixels.value_or(ImageSize {}));
        _pageSize = cells;
        if (pixels)
            _pixels = pixels;
        return;
    }

    ptyLog()("Sending terminal size: {}x{} / {}", cells.columns, cells.lines, pixels.value_or(ImageSize {}));

    auto w = winsize {};
    w.ws_col = unbox<unsigned short>(cells.columns);
    w.ws_row = unbox<unsigned short>(cells.lines);

    if (pixels.has_value())
    {
        w.ws_xpixel = unbox<unsigned short>(pixels.value().width);
        w.ws_ypixel = unbox<unsigned short>(pixels.value().height);
    }

    if (ioctl(_masterFd, TIOCSWINSZ, &w) == -1)
        throw runtime_error { std::generic_category().message(errno) };

    _pageSize = cells;
}

} // namespace vtpty
