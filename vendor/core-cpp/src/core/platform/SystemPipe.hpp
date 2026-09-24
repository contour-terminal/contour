// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file SystemPipe.hpp
/// @brief A cross-platform, in-process byte channel whose read end is *waitable*
///        by an event loop on every platform.
///
/// Unlike an anonymous OS pipe for talking to a child process, @c SystemPipe is
/// built so its read end can be multiplexed by an event loop
/// (poll(2) on POSIX, WaitForMultipleObjects on Windows). Anonymous Windows pipes
/// are NOT waitable objects, so on Windows the channel is a loopback TCP socket
/// pair with the read end mapped to a waitable event via WSAEventSelect; on POSIX
/// it is a socketpair(2) whose read fd polls directly. Both expose a
/// @c waitHandle() the loop can register and a read/write fd for the bytes.
///
/// This is what lets the same `co_await loop.waitReadable(pipe.waitHandle())`
/// readiness test (and any cross-thread wakeup-style channel, such as an event
/// loop's own post() self-pipe) work identically on Linux, macOS, and Windows.

#include <core/platform/PlatformError.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

namespace core::platform
{

/// What one read of a channel produced: bytes, nothing yet, or the end of the stream.
///
/// The three are distinct, and none of them is a failure:
/// - **Bytes**: @c bytesRead() bytes were read into the caller's buffer.
/// - **Empty**: the channel held nothing to read, and a blocking read would have waited. The
///   writer is still there, so more may come; wait for readiness and read again.
/// - **End of stream**: the writer has closed its end and every byte it wrote has been read.
///   No read will produce more, so stop waiting on the channel.
///
/// A read that fails reports a @c PlatformError instead, beside this type in a
/// `std::expected`.
class ChannelResult
{
  public:
    /// An empty result: nothing to read yet.
    constexpr ChannelResult() noexcept = default;

    /// @param count The number of bytes read. Zero makes the empty result.
    /// @return A result for a read that produced @p count bytes.
    [[nodiscard]] static constexpr ChannelResult bytes(std::size_t count) noexcept
    {
        return ChannelResult { count, Stream::Open };
    }

    /// @return A result for a read that found the writer closed and nothing left to read.
    [[nodiscard]] static constexpr ChannelResult endOfStream() noexcept
    {
        return ChannelResult { 0, Stream::Ended };
    }

    /// @return True if the channel held nothing to read, but may later. False for bytes and for
    ///         the end of the stream.
    [[nodiscard]] constexpr bool empty() const noexcept { return _bytesRead == 0 && _stream == Stream::Open; }

    /// @return True if the writer has closed and nothing is left to read.
    [[nodiscard]] constexpr bool isEndOfStream() const noexcept { return _stream == Stream::Ended; }

    /// @return The number of bytes read; zero if the result is empty or the end of the stream.
    ///         Not a `size()`, because a container's `size()` is zero exactly when it is
    ///         `empty()`, and the end of the stream is neither.
    [[nodiscard]] constexpr std::size_t bytesRead() const noexcept { return _bytesRead; }

    constexpr bool operator==(ChannelResult const&) const noexcept = default;

  private:
    enum class Stream : std::uint8_t
    {
        Open,
        Ended,
    };

    constexpr ChannelResult(std::size_t count, Stream stream) noexcept:
        _bytesRead { count }, _stream { stream }
    {
    }

    std::size_t _bytesRead = 0;
    Stream _stream = Stream::Open;
};

/// A connected, in-process byte channel with a waitable read end.
///
/// Move-only RAII: closes all owned handles on destruction. The read and write
/// ends are connected — bytes written to @c writeFd() become readable on
/// @c readFd(), and @c waitHandle() signals when @c readFd() has data (or the
/// peer closed).
///
/// On POSIX both ends are non-blocking and close-on-exec. A producer never stalls on
/// a full channel: @c write() reports a write the full buffer refused as done, because
/// the bytes already pending wake the reader just the same, which is all a wakeup
/// channel's byte signals. And a drain after readiness never parks the loop on a
/// spurious wakeup: @c read() of an empty channel returns an empty @c ChannelResult
/// instead of blocking, on Windows as on POSIX.
class SystemPipe
{
  public:
    virtual ~SystemPipe() = default;

    SystemPipe() = default;
    SystemPipe(SystemPipe const&) = delete;
    SystemPipe& operator=(SystemPipe const&) = delete;
    SystemPipe(SystemPipe&&) = default;
    SystemPipe& operator=(SystemPipe&&) = default;

    /// @return The native handle the loop watches for read-readiness. On POSIX
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
    /// @return Bytes written, or a @c PlatformError on failure. On POSIX, a write the
    ///         full channel refused reports @p size: see the class documentation.
    [[nodiscard]] virtual std::expected<std::size_t, PlatformError> write(void const* data,
                                                                          std::size_t size) = 0;

    /// Reads the bytes the channel holds, without blocking. On Windows this also resets the
    /// readiness event.
    /// @param data Destination buffer.
    /// @param size Maximum bytes to read. A read of zero bytes returns the empty result without
    ///             touching the channel.
    /// @return The bytes read, an empty result if the channel holds none (an interrupted read
    ///         included), or the end of the stream once the writer has closed and nothing is
    ///         left: see @c ChannelResult. A @c PlatformError (@c IoError) only when the read
    ///         fails.
    [[nodiscard]] virtual std::expected<ChannelResult, PlatformError> read(void* data, std::size_t size) = 0;

    /// @return True if both ends and the wait handle are valid.
    [[nodiscard]] virtual bool good() const noexcept = 0;
};

/// Creates a connected @c SystemPipe.
/// @return A unique pointer to the channel on success, or a @c PlatformError.
[[nodiscard]] std::expected<std::unique_ptr<SystemPipe>, PlatformError> createSystemPipe();

} // namespace core::platform
