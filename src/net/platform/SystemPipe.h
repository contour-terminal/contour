// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A cross-platform, in-process byte channel whose read end is *waitable*
/// by the event-loop reactor on every platform.
///
/// The channel is built so its read end can be multiplexed by the reactor
/// (poll(2) on POSIX, WaitForMultipleObjects on Windows). Anonymous Windows pipes
/// are NOT waitable objects, so on Windows the channel is a loopback TCP socket
/// pair with the read end mapped to a waitable event via WSAEventSelect; on POSIX
/// it is a socketpair(2) whose read fd polls directly. Both expose a
/// @c waitHandle() the reactor can register and a read/write fd for the bytes.
///
/// This is what lets the same `co_await loop.waitReadable(pipe.waitHandle())`
/// readiness test (and any cross-thread wakeup-style channel, such as
/// EventLoop::post's self-pipe) work identically on Linux, macOS, and Windows.

#include <cstddef>
#include <expected>
#include <memory>

#include <net/IoResult.h>
#include <net/platform/NativeHandle.h>

namespace net
{

/// A connected, in-process byte channel with a reactor-waitable read end.
///
/// Move-only RAII: closes all owned handles on destruction. The read and write
/// ends are connected — bytes written to @c writeFd() become readable on
/// @c readFd(), and @c waitHandle() signals when @c readFd() has data (or the
/// peer closed).
class SystemPipe
{
  public:
    virtual ~SystemPipe() = default;

    SystemPipe() = default;
    SystemPipe(SystemPipe const&) = delete;
    SystemPipe& operator=(SystemPipe const&) = delete;
    SystemPipe(SystemPipe&&) = default;
    SystemPipe& operator=(SystemPipe&&) = default;

    /// @return The native handle the reactor watches for read-readiness. On POSIX
    ///         this equals @c readFd(); on Windows it is a WSAEVENT associated with
    ///         the read socket via WSAEventSelect.
    [[nodiscard]] virtual NativeHandle waitHandle() const noexcept = 0;

    /// @return The native handle to read bytes from.
    [[nodiscard]] virtual NativeHandle readFd() const noexcept = 0;

    /// @return The native handle to write bytes to.
    [[nodiscard]] virtual NativeHandle writeFd() const noexcept = 0;

    /// Writes bytes into the channel. Thread-safe with respect to a concurrent
    /// reader on the other end (it is a socket send).
    /// @param data Pointer to the bytes to send.
    /// @param size Number of bytes to send.
    /// @return Bytes written, or a @c NetError on failure.
    [[nodiscard]] virtual IoResult write(void const* data, std::size_t size) = 0;

    /// Reads available bytes from the channel (non-blocking once @c waitHandle()
    /// has signalled readiness). On Windows this also resets the readiness event.
    /// @param data Destination buffer.
    /// @param size Maximum bytes to read.
    /// @return Bytes read (0 on peer close), or a @c NetError on failure.
    [[nodiscard]] virtual IoResult read(void* data, std::size_t size) = 0;

    /// @return True if both ends and the wait handle are valid.
    [[nodiscard]] virtual bool good() const noexcept = 0;
};

/// Creates a connected @c SystemPipe.
/// @return A unique pointer to the channel on success, or a @c NetError.
[[nodiscard]] std::expected<std::unique_ptr<SystemPipe>, NetError> createSystemPipe();

} // namespace net
