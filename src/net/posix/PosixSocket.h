// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32

    #include <cstddef>
    #include <span>
    #include <string>

    #include <coro/Task.hpp>
    #include <net/EventLoop.h>
    #include <net/ISocket.h>

namespace net
{

/// A reactor-driven, non-blocking POSIX stream socket. read/write try the syscall
/// and, on EAGAIN, park the calling coroutine on the loop's
/// waitReadable/waitWritable until the fd is ready, then retry — so a slow peer
/// suspends the coroutine rather than blocking the thread.
class PosixSocket final: public ISocket
{
  public:
    /// Wraps an already-connected, non-blocking fd.
    /// @param loop The loop whose reactor drives readiness (not owned).
    /// @param fd The connected socket fd (ownership transferred; closed on close()).
    /// @param peerAddress Printable peer address, or "" if unknown.
    PosixSocket(EventLoop& loop, int fd, std::string peerAddress = {}) noexcept;
    ~PosixSocket() override;

    PosixSocket(PosixSocket const&) = delete;
    PosixSocket& operator=(PosixSocket const&) = delete;
    PosixSocket(PosixSocket&&) = delete;
    PosixSocket& operator=(PosixSocket&&) = delete;

    [[nodiscard]] coro::Task<IoResult> read(std::span<std::byte> buffer) override;
    [[nodiscard]] coro::Task<IoResult> write(std::span<std::byte const> buffer) override;

    [[nodiscard]] std::string peerAddress() const override { return _peerAddress; }

    void close() noexcept override;

    [[nodiscard]] bool isClosed() const noexcept override { return _closed; }

    /// @return The underlying fd (for diagnostics/tests).
    [[nodiscard]] int native() const noexcept { return _fd; }

  private:
    EventLoop& _loop;
    int _fd;
    bool _plainFd = false; ///< Set on first ENOTSOCK: a PTY/pipe fd, served via read/write.
    std::string _peerAddress;
    bool _closed = false;
};

} // namespace net

#endif // !_WIN32
