// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The platform half of the standard streams (`StandardStreams.cpp`): one blocking read, one
/// complete write, the check that a readable handle really is, and the hold a stop needs on the
/// thread a blocking read runs on. Defined in `posix/StandardStreamPrimitives.cpp` and
/// `windows/StandardStreamPrimitives.cpp`.

#include <core/net/NetError.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace core::net::detail
{

/// What a read of a descriptor with nothing to read yet does.
enum class ReadWait : std::uint8_t
{
    NoWait, ///< Answers @c NetErrorCode::WouldBlock: the caller waits on its loop.
    Block,  ///< Waits in the call: the caller is a blocking executor's thread.
};

/// Reads what @p handle has.
///
/// An interrupted call is retried. A descriptor that is non-blocking (its parent's choice) and has
/// nothing to read is waited for or answered @c NetErrorCode::WouldBlock, as @p wait says; a blocking
/// one blocks either way, so @c ReadWait::NoWait is for a handle known to be ready. A read the
/// thread's @c ReadingThread::interrupt cancelled answers @c NetErrorCode::Cancelled.
/// @param handle The handle to read.
/// @param into Where the bytes go; at least one byte long.
/// @param wait What a non-blocking descriptor with nothing to read does.
/// @return How many bytes were read; zero at the end of the input (a Windows broken pipe included).
[[nodiscard]] std::expected<std::size_t, NetError> readStream(platform::NativeHandle handle,
                                                              std::span<std::byte> into,
                                                              ReadWait wait);

/// Writes all of @p bytes to @p handle, blocking until it has.
///
/// A non-blocking descriptor is waited for. On POSIX `SIGPIPE` is blocked on the calling thread
/// for the call, and one the call raised is consumed before the mask is restored, so a reader that
/// went away is an error and never a signal. Where the system raises it at the process (macOS), the
/// description's `F_SETNOSIGPIPE` is set once and never cleared.
/// @param handle The handle to write.
/// @param bytes What to write.
/// @return How many bytes were written: all of them, or the count before a failure once one byte
///         or more is out. A failure before the first byte is the error.
[[nodiscard]] std::expected<std::size_t, NetError> writeStream(platform::NativeHandle handle,
                                                               std::span<std::byte const> bytes);

/// Whether a handle the loop called readable will answer a read without blocking.
enum class ReadReadiness : std::uint8_t
{
    NotReady, ///< A spurious readiness, or another reader took the bytes: wait again.
    Ready,    ///< Bytes, the end of the input, or an error are there to be read.
};

/// Asks @p handle, without waiting, whether a read would return at once.
///
/// What lets a @c StreamKind::Pollable input read a descriptor whose `O_NONBLOCK` it must not set:
/// a readiness the loop reported is confirmed before the read, so the loop's thread never blocks.
/// Windows has no Pollable stream; its definition answers @c ReadReadiness::Ready.
/// @param handle The handle to ask.
/// @return Whether a read would return at once.
[[nodiscard]] ReadReadiness readReadinessNow(platform::NativeHandle handle) noexcept;

/// The thread a blocking read is running on, held so a stop can cancel that read where the OS
/// allows: on Windows `CancelSynchronousIo`, on POSIX nothing, because a read that has started
/// there cannot be taken back without a signal handler the library does not own.
///
/// Made on the reading thread, just before the read, and destroyed on it, just after, both under
/// the lock the stopper takes to call @c interrupt -- so an interrupt can only ever reach the read
/// it was meant for, never whatever that pool thread runs next.
class ReadingThread
{
  public:
    /// Records the calling thread.
    ReadingThread() noexcept;
    ~ReadingThread();
    ReadingThread(ReadingThread const&) = delete;
    ReadingThread& operator=(ReadingThread const&) = delete;
    ReadingThread(ReadingThread&&) = delete;
    ReadingThread& operator=(ReadingThread&&) = delete;

    /// Cancels the recorded thread's synchronous read, if one is running; any thread may call it.
    void interrupt() noexcept;

  private:
    /// Windows: a duplicated handle of the reading thread. POSIX keeps nothing here.
    [[maybe_unused]] platform::NativeHandle _thread = platform::InvalidHandle;
};

} // namespace core::net::detail
