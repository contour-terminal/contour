// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The process's standard streams as coroutines on an @c EventLoop: an input that reads what is
/// there, and two outputs that write in order, whatever kind of object each handle turns out to be.
///
/// What a standard handle is decides how it can be waited for, and the process does not choose it:
/// the parent does. A pipe, a socket or a terminal on POSIX can be waited for on the loop; a regular
/// file is always "readable" and `epoll` refuses it outright (`EPERM`), `/dev/null` likewise, and
/// on Windows a console handle cannot go on a completion port and an inherited pipe is not opened
/// overlapped. @c classifyStreamHandle sorts a handle into the two ways this header knows, and
/// @c openStandardStreams builds each stream the way its handle allows.
///
/// **No descriptor's flags are touched.** An inherited descriptor shares its open file description
/// with the parent, so `O_NONBLOCK` set here would be set in the parent's shell too, and a shell
/// whose stdin turned non-blocking under it reads `EAGAIN` as end of input. A @c StreamKind::Pollable
/// input therefore waits for readiness, confirms it, and then reads what is there with a read that
/// cannot block; everything else is read and written on the blocking executor.
///
/// **The handles are not owned.** A stream never closes its handle: the process's standard handles
/// belong to the process, and a test's to the test.

#include <core/async/IExecutor.hpp>
#include <core/async/Task.hpp>
#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace core::net
{

class EventLoop;

/// How a stream's handle can be driven without blocking the loop.
enum class StreamKind : std::uint8_t
{
    /// Waited for on the loop and then read without blocking: a POSIX pipe, socket or terminal.
    Pollable,
    /// Read or written on the blocking executor: a regular file, a device that is not a terminal
    /// (`/dev/null`), and on Windows every kind of handle, because neither a console nor an inherited
    /// anonymous pipe can go on the completion port.
    Blocking,
};

/// Sorts @p handle by what the OS says it is: `fstat`'s file type on POSIX, `GetFileType` on
/// Windows. One table row per type, in each platform's `StreamKindTable.cpp`.
///
/// A handle the OS cannot describe -- closed, invalid -- is @c StreamKind::Blocking, the kind that
/// assumes nothing: its first read then reports what is wrong with it.
/// @param handle The handle to classify; never read, written or changed.
/// @return How a stream over @p handle is driven.
[[nodiscard]] CORE_NET_API StreamKind classifyStreamHandle(platform::NativeHandle handle) noexcept;

/// Whether a read reached the end of its input.
enum class InputEnd : std::uint8_t
{
    More,    ///< The input may have more.
    Reached, ///< The writer closed it, or a file ended: this read returned no bytes.
};

/// What one @c IInputStream::read returned.
struct StreamRead
{
    std::vector<std::byte> bytes;  ///< What was read; empty exactly when @c end is reached, or when
                                   ///< zero bytes were asked for.
    InputEnd end = InputEnd::More; ///< Whether the input ended.
};

/// An input stream read by a flow on one @c EventLoop.
///
/// **One read in flight.** A second @c read while one is in flight -- from the call until its task
/// finishes or is destroyed unstarted -- answers @c NetErrorCode::BadHandle with systemCode 0 and
/// touches nothing.
///
/// **A stop loses no input.** It throws @c async::OperationCancelled. A worker read the stop
/// reaches while it is still queued never reads, and the next @c read starts afresh. One already
/// running cannot be taken back on POSIX, so it is left to finish and whatever it returns is what
/// the next @c read returns; on Windows the stop also cancels it (`CancelSynchronousIo`), and the
/// next @c read then reads afresh. A stop landing in the instant between the worker's start and its
/// `ReadFile` is not seen by that read, which then finishes like a POSIX one.
///
/// **A blocking read holds an executor thread until input arrives** -- every Windows stdin, a regular
/// file only briefly, and a macOS terminal its kqueue refuses for as long as nobody types. A stopped
/// read keeps holding it: the thread is the read's until the read returns. See
/// @c openStandardStreams for what that asks of the executor.
///
/// **It assumes it is its handle's only reader.** Another reader of the same handle could take the
/// bytes between the readiness and the read, and the read would then wait for more on the blocking
/// path's terms.
class IInputStream
{
  public:
    virtual ~IInputStream() = default;
    IInputStream() = default;
    IInputStream(IInputStream const&) = delete;
    IInputStream& operator=(IInputStream const&) = delete;
    IInputStream(IInputStream&&) = delete;
    IInputStream& operator=(IInputStream&&) = delete;

    /// Reads what is available, waiting until something is.
    /// @param maxBytes The most to return; clamped to the stream's `maxReadChunk`. Zero answers an
    ///        empty @c StreamRead at once, with @c InputEnd::More, and reads nothing.
    /// @return At least one byte with @c InputEnd::More, or no bytes with @c InputEnd::Reached at
    ///         the end of the input; a read error as a @c NetError (@c NetErrorCode::SystemError with
    ///         the OS's code, unless the code has a class of its own).
    /// @throws async::OperationCancelled where the awaiting flow is stopped first.
    [[nodiscard]] virtual async::Task<std::expected<StreamRead, NetError>> read(std::size_t maxBytes) = 0;
};

/// An output stream written by a flow on one @c EventLoop, always through the blocking executor.
///
/// **In order, one write in flight.** A second @c write while one is in flight answers
/// @c NetErrorCode::BadHandle with systemCode 0, so writes reach the handle in the order their
/// flow awaited them. A stop already requested when a write starts throws
/// @c async::OperationCancelled and writes nothing; a write that has started is finished, and
/// answered.
///
/// **A closed reader is an error, never a signal.** On POSIX `SIGPIPE` is blocked on the writing
/// thread for the write and a pending one consumed -- and on macOS, which raises a pipe's
/// `SIGPIPE` at the whole process, the description's `F_SETNOSIGPIPE` is set at the first write and
/// left set for the life of the process, which every process sharing the description then sees -- so
/// a write to a pipe nobody reads answers
/// @c NetErrorCode::SystemError with `EPIPE` rather than ending the process; on Windows it is
/// `ERROR_NO_DATA` or `ERROR_BROKEN_PIPE`.
class IOutputStream
{
  public:
    virtual ~IOutputStream() = default;
    IOutputStream() = default;
    IOutputStream(IOutputStream const&) = delete;
    IOutputStream& operator=(IOutputStream const&) = delete;
    IOutputStream(IOutputStream&&) = delete;
    IOutputStream& operator=(IOutputStream&&) = delete;

    /// Writes all of @p bytes.
    /// @param bytes What to write.
    /// @return How many bytes were written: all of them, or fewer where the handle failed part-way.
    ///         The failure itself is not reported then; a later write that meets it again does. A
    ///         failure before the first byte is the @c NetError.
    /// @throws async::OperationCancelled where the awaiting flow was stopped before the write.
    [[nodiscard]] virtual async::Task<std::expected<std::size_t, NetError>> write(
        std::vector<std::byte> bytes) = 0;
};

/// The three handles a set of standard streams is built over.
struct StandardStreamHandles
{
    platform::NativeHandle input;  ///< Read by @c StandardStreams::input.
    platform::NativeHandle output; ///< Written by @c StandardStreams::output.
    platform::NativeHandle error;  ///< Written by @c StandardStreams::error.
};

/// The process's own standard handles: `platform::standardInput()`, `standardOutput()` and
/// `standardError()`. Separate from @c openStandardStreams so a host or a test can build the
/// streams over handles of its own.
/// @return The handles; on Windows an absent one (a GUI process) is what `GetStdHandle` returns.
[[nodiscard]] CORE_NET_API StandardStreamHandles processStandardHandles() noexcept;

/// The three streams of a process, or of whatever handles @c openStandardStreams was given.
struct StandardStreams
{
    std::unique_ptr<IInputStream> input;   ///< Over @c StandardStreamHandles::input.
    std::unique_ptr<IOutputStream> output; ///< Over @c StandardStreamHandles::output.
    std::unique_ptr<IOutputStream> error;  ///< Over @c StandardStreamHandles::error.
};

/// Builds a stream over each of @p handles, each driven the way @c classifyStreamHandle says.
///
/// A @c StreamKind::Pollable input whose registration the loop's backend refuses -- macOS's kqueue
/// refuses some terminals with `EINVAL` -- turns into a @c StreamKind::Blocking one at that read,
/// and stays one.
///
/// Every stream, and every task it returns, is used on @p loop's thread. A stream may be destroyed
/// while a read it returned is still parked: that read finishes on its own, into state it shares.
///
/// **The thread budget of @p blocking.** A Blocking input's read occupies one of its threads until
/// input arrives, and a stopped read keeps it (see @c IInputStream). Writes run on the same
/// executor, so it needs at least two threads -- or the input a separate executor of its own --
/// or a write waits behind a read nobody answers. Stopping or joining the executor at exit waits for
/// that read too, which on a terminal means for the user to type. Passing the loop itself as
/// @p blocking (the single-threaded configuration) runs that read on the loop's thread and blocks
/// the whole loop until stdin has input.
///
/// **A handle stays open until every worker read on it has returned**, not only until the streams
/// are gone: a POSIX worker that finds a non-blocking descriptor empty polls it again by number, and
/// a number closed and reused meanwhile would be another file's.
/// @param loop Where the streams' flows run, and where a blocking call's answer comes back to;
///        must outlive the streams and their tasks.
/// @param blocking Where every blocking read and write runs; must outlive the streams and their
///        tasks, and any read still running on it.
/// @param handles The handles; not owned.
/// @param maxReadChunk The most one read returns, and so the most it allocates. Zero is taken as one.
/// @return The streams.
[[nodiscard]] CORE_NET_API StandardStreams openStandardStreams(EventLoop& loop,
                                                               async::IExecutor& blocking,
                                                               StandardStreamHandles handles,
                                                               std::size_t maxReadChunk = std::size_t { 64 }
                                                                                          * 1024);

} // namespace core::net
