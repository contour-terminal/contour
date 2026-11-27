// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: the handles the standard-stream cases build their streams over, made by the test
/// itself -- never the test process's own stdio. Defined per platform, in
/// `posix/StandardStreamsTestSupport.cpp` and `windows/StandardStreamsTestSupport.cpp`, so the
/// portable cases carry no `#ifdef`.

#include <core/net/StandardStreams.hpp>
#include <core/platform/Types.hpp>

#include <cstdint>
#include <filesystem>
#include <utility>

namespace core::net::streams_test
{

/// A handle the test made, closed when this goes.
class OwnedHandle
{
  public:
    OwnedHandle() = default;
    /// @param handle The handle to own; `platform::InvalidHandle` owns nothing.
    explicit OwnedHandle(platform::NativeHandle handle) noexcept: _handle { handle } {}
    ~OwnedHandle() { reset(); }
    OwnedHandle(OwnedHandle const&) = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept:
        _handle { std::exchange(other._handle, platform::InvalidHandle) }
    {
    }
    OwnedHandle& operator=(OwnedHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _handle = std::exchange(other._handle, platform::InvalidHandle);
        }
        return *this;
    }

    /// @return The handle; `platform::InvalidHandle` when none is owned.
    [[nodiscard]] platform::NativeHandle get() const noexcept { return _handle; }

    /// Closes the handle now.
    void reset() noexcept
    {
        if (_handle != platform::InvalidHandle)
            platform::platformClose(std::exchange(_handle, platform::InvalidHandle));
    }

  private:
    platform::NativeHandle _handle = platform::InvalidHandle;
};

/// Both ends of an anonymous pipe: blocking, and on Windows not overlapped -- what a child
/// inherits as its standard handles.
struct TestPipe
{
    OwnedHandle read;  ///< The end a stream reads.
    OwnedHandle write; ///< The end the test writes.
};

/// @return A new anonymous pipe; the case fails where the OS refuses one.
[[nodiscard]] TestPipe makeTestPipe();

/// @param path An existing file.
/// @return @p path opened for reading.
[[nodiscard]] OwnedHandle openForReading(std::filesystem::path const& path);

/// @param path The file to create, or to truncate.
/// @return @p path opened for writing.
[[nodiscard]] OwnedHandle openForWriting(std::filesystem::path const& path);

/// @return The null device (`/dev/null`, `NUL`) opened for reading.
[[nodiscard]] OwnedHandle openNullDevice();

/// What a stop does to a worker already blocked in its read.
enum class RunningReadStop : std::uint8_t
{
    Waits,   ///< Nothing: the read keeps its thread until input arrives (POSIX).
    Cancels, ///< `CancelSynchronousIo` ends it, and its thread is free at once (Windows).
};

/// @return What a stop does to a running worker read on this platform.
[[nodiscard]] RunningReadStop runningReadStop() noexcept;

/// @return What this platform's table says an anonymous pipe is: @c StreamKind::Pollable on POSIX,
///         @c StreamKind::Blocking on Windows.
[[nodiscard]] StreamKind pipeStreamKind() noexcept;

} // namespace core::net::streams_test
