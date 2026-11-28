// SPDX-License-Identifier: Apache-2.0

/// @file
/// The Windows standard-stream primitives: synchronous `ReadFile` and `WriteFile`, and the
/// `CancelSynchronousIo` a stop sends to a worker blocked in a read.

#include <core/net/detail/StandardStreamPrimitives.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <span>
#include <string>

#include <windows.h>

namespace core::net::detail
{

namespace
{
    /// One Win32 error with a class of its own; every other one is @c NetErrorCode::SystemError.
    struct StreamErrorRow
    {
        DWORD error;
        NetErrorCode code;
    };

    constexpr auto StreamErrorTable = std::array {
        StreamErrorRow { .error = ERROR_INVALID_HANDLE, .code = NetErrorCode::BadHandle },
        StreamErrorRow { .error = ERROR_ACCESS_DENIED, .code = NetErrorCode::PermissionDenied },
        StreamErrorRow { .error = ERROR_NOT_ENOUGH_MEMORY, .code = NetErrorCode::ResourceExhausted },
        StreamErrorRow { .error = ERROR_OUTOFMEMORY, .code = NetErrorCode::ResourceExhausted },
        StreamErrorRow { .error = ERROR_NO_SYSTEM_RESOURCES, .code = NetErrorCode::ResourceExhausted },
        // Only `ReadingThread::interrupt` cancels a stream's synchronous call.
        StreamErrorRow { .error = ERROR_OPERATION_ABORTED, .code = NetErrorCode::Cancelled },
    };

    /// The errors that end a read's input rather than fail it: a pipe whose writer closed, a file
    /// read at its end.
    constexpr auto EndOfInputErrors = std::array<DWORD, 2> { ERROR_BROKEN_PIPE, ERROR_HANDLE_EOF };

    /// @return @p error, classified by the table.
    [[nodiscard]] NetError streamError(DWORD error, char const* what)
    {
        auto const row = std::ranges::find(StreamErrorTable, error, &StreamErrorRow::error);
        auto const code = row == StreamErrorTable.end() ? NetErrorCode::SystemError : row->code;
        return makeNetError(code, static_cast<int>(error), std::string { what });
    }

    /// @return @p size, as much of it as one call takes.
    [[nodiscard]] DWORD chunkOf(std::size_t size) noexcept
    {
        return static_cast<DWORD>(std::min<std::size_t>(size, std::numeric_limits<DWORD>::max()));
    }
} // namespace

std::expected<std::size_t, NetError> readStream(platform::NativeHandle handle,
                                                std::span<std::byte> into,
                                                [[maybe_unused]] ReadWait wait)
{
    // A synchronous handle has no "nothing yet": every read blocks, whatever @p wait says.
    auto got = DWORD { 0 };
    if (ReadFile(handle, into.data(), chunkOf(into.size()), &got, nullptr) != 0)
        return static_cast<std::size_t>(got);
    auto const error = GetLastError();
    if (std::ranges::find(EndOfInputErrors, error) != EndOfInputErrors.end())
        return std::size_t { 0 };
    return std::unexpected(streamError(error, "ReadFile"));
}

std::expected<std::size_t, NetError> writeStream(platform::NativeHandle handle,
                                                 std::span<std::byte const> bytes)
{
    auto written = std::size_t { 0 };
    while (written < bytes.size())
    {
        auto const rest = bytes.subspan(written);
        auto put = DWORD { 0 };
        if (WriteFile(handle, rest.data(), chunkOf(rest.size()), &put, nullptr) == 0)
        {
            auto const error = GetLastError();
            if (written > 0)
                return written;
            return std::unexpected(streamError(error, "WriteFile"));
        }
        // A success that moved nothing would loop for ever: a pipe in PIPE_NOWAIT mode with a full
        // buffer answers so. Reported as what was written, or as the failure it is.
        if (put == 0)
        {
            if (written > 0)
                return written;
            return std::unexpected(
                makeNetError(NetErrorCode::SystemError, 0, "WriteFile: the handle took no bytes"));
        }
        written += put;
    }
    return written;
}

ReadReadiness readReadinessNow([[maybe_unused]] platform::NativeHandle handle) noexcept
{
    // No Windows handle is Pollable (windows/StreamKindTable.cpp), so nothing asks.
    return ReadReadiness::Ready;
}

ReadingThread::ReadingThread() noexcept
{
    auto thread = HANDLE {};
    // THREAD_TERMINATE is the access CancelSynchronousIo needs.
    if (DuplicateHandle(
            GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, THREAD_TERMINATE, FALSE, 0)
        != 0)
        _thread = thread;
}

ReadingThread::~ReadingThread()
{
    if (_thread != platform::InvalidHandle)
        CloseHandle(_thread);
}

void ReadingThread::interrupt() noexcept
{
    // ERROR_NOT_FOUND when the read has not started or has just returned: nothing to cancel.
    if (_thread != platform::InvalidHandle)
        CancelSynchronousIo(_thread);
}

} // namespace core::net::detail
