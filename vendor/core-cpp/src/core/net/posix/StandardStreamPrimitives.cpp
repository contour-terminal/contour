// SPDX-License-Identifier: Apache-2.0

/// @file
/// The POSIX standard-stream primitives: `read`, `write` with `SIGPIPE` held off, and a zero-timeout
/// `poll` to confirm a readiness before a read of a descriptor that may be blocking.

#include <core/net/detail/StandardStreamPrimitives.hpp>

#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/WouldBlock.hpp>

#include <sys/select.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

namespace core::net::detail
{

namespace
{
    /// @return The error of a failed stream call, classified by the one socket-error table.
    [[nodiscard]] NetError streamError(int code, char const* what)
    {
        return socketError(code, std::string { what });
    }

    /// Blocks until @p fd is ready for @p events, for a descriptor its parent made non-blocking.
    void waitFor(int fd, short events) noexcept
    {
        auto entry = pollfd { .fd = fd, .events = events, .revents = 0 };
        while (::poll(&entry, 1, -1) < 0 && errno == EINTR)
        {
        }
    }

    /// Asks with `select` what `poll` could not: macOS's `poll` answers `POLLNVAL` for an open
    /// terminal, which `select` supports. A descriptor `select` cannot hold (`FD_SETSIZE` and above)
    /// is answered Ready, and the read then decides, as it would for any other.
    [[nodiscard]] ReadReadiness readReadinessBySelect(int fd) noexcept
    {
        if (fd >= FD_SETSIZE)
            return ReadReadiness::Ready;
        while (true)
        {
            auto readable = fd_set {};
            FD_ZERO(&readable);
            FD_SET(fd, &readable);
            auto zero = timeval {};
            auto const ready = ::select(fd + 1, &readable, nullptr, nullptr, &zero);
            if (ready == 0)
                return ReadReadiness::NotReady;
            if (ready > 0 || errno != EINTR)
                return ReadReadiness::Ready;
        }
    }

    /// Holds `SIGPIPE` off the calling thread for one write, and consumes one the write raised.
    ///
    /// A write to a pipe whose reader is gone raises `SIGPIPE`, directed at the writing thread, and
    /// its default action ends the process. There is no `MSG_NOSIGNAL` for a pipe and no flag to
    /// set on a descriptor the parent shares, so the signal is blocked here, and one that became
    /// pending during the write -- and was not pending before it -- is taken with `sigwait` before
    /// the old mask returns. `sigwait` rather than `sigtimedwait`, which macOS lacks; it is only
    /// called with the signal known to be pending, so it does not wait.
    class SigpipeHold
    {
      public:
        SigpipeHold() noexcept
        {
            sigemptyset(&_pipe);
            sigaddset(&_pipe, SIGPIPE);
            auto pending = sigset_t {};
            sigpending(&pending);
            _wasPending = sigismember(&pending, SIGPIPE) == 1 ? Pending::Yes : Pending::No;
            pthread_sigmask(SIG_BLOCK, &_pipe, &_previous);
        }

        ~SigpipeHold()
        {
            auto pending = sigset_t {};
            sigpending(&pending);
            if (_wasPending == Pending::No && sigismember(&pending, SIGPIPE) == 1)
            {
                auto taken = 0;
                sigwait(&_pipe, &taken);
            }
            pthread_sigmask(SIG_SETMASK, &_previous, nullptr);
        }

        SigpipeHold(SigpipeHold const&) = delete;
        SigpipeHold& operator=(SigpipeHold const&) = delete;
        SigpipeHold(SigpipeHold&&) = delete;
        SigpipeHold& operator=(SigpipeHold&&) = delete;

      private:
        /// Whether a `SIGPIPE` was pending before the write: then it is somebody else's to take.
        enum class Pending : std::uint8_t
        {
            No,
            Yes,
        };

        sigset_t _pipe {};
        sigset_t _previous {};
        Pending _wasPending = Pending::No;
    };

    /// Where the system raises a pipe's `SIGPIPE` at the whole process rather than at the writing
    /// thread (macOS: XNU's write path calls `psignal` on the process), blocking it on the writing
    /// thread is not enough: the kernel hands it to any thread that has it unblocked, and its default
    /// action ends the process. There the descriptor's own flag (`F_SETNOSIGPIPE`) is set at the first
    /// write and **stays set for the life of the process**, so every process sharing the description
    /// -- a parent that handed down its stdout -- writes to it without `SIGPIPE` from then on, and
    /// sees `EPIPE` instead. A save and a restore around each write would race with every other writer
    /// of the description, one restoring the flag while another is mid-write. Nothing on a system
    /// without the flag, whose `SIGPIPE` goes to the writing thread.
    void keepSigpipeOffDescription([[maybe_unused]] int fd) noexcept
    {
#ifdef F_SETNOSIGPIPE
        if (::fcntl(fd, F_GETNOSIGPIPE) == 0)
            ::fcntl(fd, F_SETNOSIGPIPE, 1);
#endif
    }
} // namespace

std::expected<std::size_t, NetError> readStream(platform::NativeHandle handle,
                                                std::span<std::byte> into,
                                                ReadWait wait)
{
    while (true)
    {
        auto const got = ::read(handle, into.data(), into.size());
        if (got >= 0)
            return static_cast<std::size_t>(got);
        auto const code = errno;
        if (code == EINTR)
            continue;
        if (isWouldBlock(code) && wait == ReadWait::Block)
        {
            waitFor(handle, POLLIN);
            continue;
        }
        if (isWouldBlock(code))
            return std::unexpected(makeNetError(NetErrorCode::WouldBlock, code, "read: nothing to read yet"));
        return std::unexpected(streamError(code, "read"));
    }
}

std::expected<std::size_t, NetError> writeStream(platform::NativeHandle handle,
                                                 std::span<std::byte const> bytes)
{
    [[maybe_unused]] auto const hold = SigpipeHold {};
    keepSigpipeOffDescription(handle);
    auto written = std::size_t { 0 };
    while (written < bytes.size())
    {
        auto const rest = bytes.subspan(written);
        auto const put = ::write(handle, rest.data(), rest.size());
        if (put >= 0)
        {
            written += static_cast<std::size_t>(put);
            continue;
        }
        auto const code = errno;
        if (code == EINTR)
            continue;
        if (isWouldBlock(code))
        {
            waitFor(handle, POLLOUT);
            continue;
        }
        if (written > 0)
            return written;
        return std::unexpected(streamError(code, "write"));
    }
    return written;
}

ReadReadiness readReadinessNow(platform::NativeHandle handle) noexcept
{
    auto entry = pollfd { .fd = handle, .events = POLLIN, .revents = 0 };
    while (true)
    {
        auto const ready = ::poll(&entry, 1, 0);
        if (ready > 0 && (entry.revents & POLLNVAL) != 0 && ::fcntl(handle, F_GETFD) >= 0)
            return readReadinessBySelect(handle); // an open descriptor poll cannot ask: a macOS tty
        if (ready > 0)
            return ReadReadiness::Ready; // POLLIN, POLLHUP, POLLERR, and a closed descriptor's POLLNVAL
        if (ready == 0)
            return ReadReadiness::NotReady;
        if (errno != EINTR)
            return ReadReadiness::Ready; // let the read report what poll could not
    }
}

ReadingThread::ReadingThread() noexcept = default;

ReadingThread::~ReadingThread() = default;

void ReadingThread::interrupt() noexcept
{
    // Nothing to do: see the class comment. The stop has already handed the waiter back.
}

} // namespace core::net::detail
