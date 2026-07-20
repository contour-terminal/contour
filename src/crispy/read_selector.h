// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <vector>

#ifndef _WIN32
    #include <crispy/file_descriptor.h>

    #include <sys/select.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

#ifdef __linux__
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
#endif

namespace crispy
{

/// Implements waiting for a set of file descriptors to become readable.
///
class posix_read_selector
{
  public:
    posix_read_selector()
    {
        int pfd[2];
        int const rv = pipe(pfd);
        Require(rv == 0);
        _breakPipeReader = file_descriptor::from_native(pfd[0]);
        _breakPipeWriter = file_descriptor::from_native(pfd[1]);

        int currentFlags = 0;
        for (int const fd: pfd)
        {
            fcntl(fd, F_GETFL, &currentFlags);
            fcntl(fd, F_SETFL, currentFlags | O_NONBLOCK);
        }
    }

    void want_read(int fd) noexcept
    {
        auto const _ = std::scoped_lock { _fdsMutex };
        assert(fd >= 0);
        assert(std::count(_fds.begin(), _fds.end(), fd) == 0);
        _fds.push_back(fd);
        std::ranges::sort(_fds);
    }

    [[nodiscard]] size_t size() const noexcept
    {
        auto const _ = std::scoped_lock { _fdsMutex };
        return _fds.size();
    }

    void cancel_read(int fd) noexcept
    {
        auto const _ = std::scoped_lock { _fdsMutex };
        assert(std::count(_fds.begin(), _fds.end(), fd) == 1);
        auto gc = std::ranges::remove(_fds, fd);
        _fds.erase(gc.begin(), gc.end());
    }

    void wakeup() noexcept
    {
        if (_breakPipeWriter.is_open())
        {
            auto written = write(_breakPipeWriter, "x", 1);
            if (written == -1)
                errorLog()("Writing to break-pipe failed. {}", strerror(errno));
        }
    }

    /// Reports whether @p fd is still a registered read-interest.
    ///
    /// Used by the caller to re-validate, after wait_one() returned, that a concurrent cancel_read()
    /// (e.g. from close() on another thread) has not removed the fd in the meantime — reading from a
    /// cancelled/closed fd must be avoided.
    /// @param fd The file descriptor to test.
    /// @return true iff @p fd is currently registered for read interest.
    [[nodiscard]] bool is_wanted(int fd) const noexcept
    {
        auto const _ = std::scoped_lock { _fdsMutex };
        return std::ranges::find(_fds, fd) != _fds.end();
    }

    std::optional<int> wait_one(std::optional<std::chrono::milliseconds> timeout = std::nullopt) noexcept
    {
        if (auto const fd = try_pop_pending(); fd.has_value())
            return fd;

        FD_ZERO(&_reader);
        FD_ZERO(&_writer);
        FD_ZERO(&_except);

        // Snapshot the registered fds under the lock, then run the blocking select() WITHOUT holding it:
        // a concurrent cancel_read()/want_read() mutates the shared _fds vector, so iterating it here
        // unsynchronized is a data race (garbage/dangling fd handed to select, or a crash). The lock must
        // not span select() itself, or close()'s cancel_read() would block for the whole wait, defeating
        // the independent break-pipe wakeup. A cancel_read() that lands after this snapshot is still
        // handled: the break-pipe wakeup returns select(), and the caller re-checks is_wanted().
        int maxfd = _breakPipeReader.get();
        FD_SET(_breakPipeReader.get(), &_reader);
        {
            auto const _ = std::scoped_lock { _fdsMutex };
            for (auto const fd: _fds)
            {
                FD_SET(fd, &_reader);
                maxfd = std::max(maxfd, fd);
            }
        }

        auto tv = std::unique_ptr<timeval>();
        if (timeout.has_value())
        {
            tv = std::make_unique<timeval>(
                timeval { .tv_sec = timeout->count() / 1000,
                          .tv_usec = static_cast<int>((timeout->count() % 1000) * 1000) });
        }

        auto const result = ::select(maxfd + 1, &_reader, &_writer, &_except, tv.get());

        if (result <= 0)
            return std::nullopt;

        if (FD_ISSET(_breakPipeReader, &_reader))
        {
            // Drain the pipe.
            char buf[256];
            while (read(_breakPipeReader, buf, sizeof(buf)) > 0)
                ;
        }

        {
            auto const _ = std::scoped_lock { _fdsMutex };
            for (int const fd: _fds)
                if (FD_ISSET(fd, &_reader))
                    _pending.push_back(fd);
        }

        return try_pop_pending();
    }

  private:
    std::optional<int> try_pop_pending() noexcept
    {
        if (_pending.empty())
        {
            errno = EAGAIN;
            return std::nullopt;
        }

        auto const fd = _pending.front();
        _pending.pop_front();
        return fd;
    }

  private:
    fd_set _reader {};
    fd_set _writer {};
    fd_set _except {};
    mutable std::mutex _fdsMutex; //!< Guards @c _fds against concurrent cancel_read()/want_read() while
                                  //!< wait_one() snapshots it; the epoll backend delegates this to the
                                  //!< kernel, the posix backend must synchronize the vector itself.
    std::vector<int> _fds;
    std::deque<int> _pending;
    file_descriptor _breakPipeReader;
    file_descriptor _breakPipeWriter;
};

// {{{ epoll_read_selector, implements waiting for a set of file descriptors to become readable.
#ifdef __linux__

/// Implements waiting for a set of file descriptors to become readable.
///
class epoll_read_selector
{
  public:
    epoll_read_selector();
    ~epoll_read_selector() = default;

    void want_read(int fd) noexcept;
    void cancel_read(int fd) noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool is_wanted(int fd) const noexcept;

    void wakeup() const noexcept;
    std::optional<int> wait_one(std::optional<std::chrono::milliseconds> timeout = std::nullopt) noexcept;

  private:
    std::optional<int> try_pop_pending() noexcept;

  private:
    file_descriptor _epollFd;
    file_descriptor _eventFd;
    mutable std::mutex _fdsMutex; //!< Guards @c _fds against concurrent cancel_read()/want_read().
    std::vector<int> _fds;
    std::deque<int> _pending;
};

inline epoll_read_selector::epoll_read_selector()
{
    _epollFd = file_descriptor::from_native(epoll_create1(EPOLL_CLOEXEC));
    _eventFd = file_descriptor::from_native(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));

    auto event = epoll_event {};
    event.events = EPOLLIN;
    event.data.fd = _eventFd;
    epoll_ctl(_epollFd, EPOLL_CTL_ADD, _eventFd, &event);
}

// NOLINTNEXTLINE(readability-make-member-function-const)
inline void epoll_read_selector::want_read(int fd) noexcept
{
    auto event = epoll_event {};
    event.events = EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(_epollFd, EPOLL_CTL_ADD, fd, &event);
    auto const _ = std::scoped_lock { _fdsMutex };
    _fds.push_back(fd);
}

// NOLINTNEXTLINE(readability-make-member-function-const)
inline void epoll_read_selector::cancel_read(int fd) noexcept
{
    auto event = epoll_event {};
    event.events = EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(_epollFd, EPOLL_CTL_DEL, fd, &event);
    auto const _ = std::scoped_lock { _fdsMutex };
    auto gc = std::ranges::remove(_fds, fd);
    _fds.erase(gc.begin(), gc.end());
}

inline size_t epoll_read_selector::size() const noexcept
{
    auto const _ = std::scoped_lock { _fdsMutex };
    return _fds.size();
}

inline bool epoll_read_selector::is_wanted(int fd) const noexcept
{
    auto const _ = std::scoped_lock { _fdsMutex };
    return std::ranges::find(_fds, fd) != _fds.end();
}

inline void epoll_read_selector::wakeup() const noexcept
{
    auto const value = eventfd_t { 1 };
    auto written = write(_eventFd, &value, sizeof(value));
    if (written == -1)
        errorLog()("Writing to eventFd failed. {}", strerror(errno));
}

inline std::optional<int> epoll_read_selector::try_pop_pending() noexcept
{
    if (_pending.empty())
        return std::nullopt;

    auto const fd = _pending.front();
    _pending.pop_front();
    return fd;
}

inline std::optional<int> epoll_read_selector::wait_one(
    std::optional<std::chrono::milliseconds> timeout) noexcept
{
    if (auto const fd = try_pop_pending(); fd.has_value())
        return fd;

    auto events = std::array<epoll_event, 64> { {} };
    for (;;)
    {
        auto const result = epoll_wait(_epollFd,
                                       events.data(),
                                       events.size(),
                                       timeout.has_value() ? static_cast<int>(timeout.value().count()) : -1);
        if (result == 0)
        {
            errno = EAGAIN;
            return std::nullopt;
        }

        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            return std::nullopt;
        }

        bool piped = false;
        for (auto const i: std::views::iota(0zu, static_cast<size_t>(result)))
        {
            if (events[i].data.fd == _eventFd)
            {
                eventfd_t dummy {};
                piped = ::read(_eventFd, &dummy, sizeof(dummy)) > 0;
            }
            else
                _pending.push_back(events[i].data.fd);
        }

        if (auto fd = try_pop_pending(); fd.has_value())
            return fd;

        errno = piped ? EINTR : EAGAIN;
        return std::nullopt;
    }
}

#endif // }}}

/// Implements waiting for a set of file descriptors to become readable.
#ifdef __linux__
using read_selector = epoll_read_selector;
#else
using read_selector = posix_read_selector;
#endif

} // namespace crispy
