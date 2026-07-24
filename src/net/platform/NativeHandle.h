// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Platform-agnostic native handle type and the minimal byte-I/O helpers the
/// reactor and socket layer need. Trimmed from Endo's platform/Types.hpp (see
/// src/coro/README.md for the port's provenance).

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/types.h>

    #include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>

namespace net
{

#ifdef _WIN32
/// Native file/pipe/socket handle type for the current platform.
using NativeHandle = HANDLE;

/// Invalid handle sentinel value.
/// Cannot be constexpr on Windows because INVALID_HANDLE_VALUE involves a cast.
inline NativeHandle const InvalidHandle = INVALID_HANDLE_VALUE;

/// Closes a native handle (no-op for the invalid sentinel).
/// @param h The native handle to close.
inline void platformClose(NativeHandle h) noexcept
{
    if (h != InvalidHandle)
        CloseHandle(h);
}

/// Writes bytes to a native handle.
/// @param h The native handle to write to.
/// @param data Pointer to data to write.
/// @param size Number of bytes to write.
/// @return Number of bytes written, or -1 on error.
inline auto platformWrite(NativeHandle h, void const* data, size_t size) -> intptr_t
{
    DWORD written = 0;
    if (!WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr))
        return -1;
    return static_cast<intptr_t>(written);
}

/// Reads bytes from a native handle.
///
/// Semantics match POSIX `read(2)`: returns the number of bytes read, 0 on
/// end-of-file (including the case where a pipe's write end has been closed
/// and the buffer is drained), or -1 on a genuine I/O error.
/// @param h The native handle to read from.
/// @param data Pointer to buffer to read into.
/// @param size Maximum number of bytes to read.
/// @return Bytes read, 0 on EOF / broken pipe, or -1 on error.
inline auto platformRead(NativeHandle h, void* data, size_t size) -> intptr_t
{
    DWORD bytesRead = 0;
    if (!ReadFile(h, data, static_cast<DWORD>(size), &bytesRead, nullptr))
    {
        // ERROR_BROKEN_PIPE / ERROR_HANDLE_EOF are stream terminators, not
        // errors — normalize them to POSIX-style EOF so the cross-platform
        // abstraction is uniform for callers that distinguish EOF from error.
        auto const err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return static_cast<intptr_t>(bytesRead);
}
#else
/// Native file/pipe/socket handle type for the current platform.
using NativeHandle = int;

/// Invalid handle sentinel value.
constexpr NativeHandle InvalidHandle = -1;

/// Closes a native handle (no-op for the invalid sentinel).
/// @param fd The file descriptor to close.
inline void platformClose(NativeHandle fd) noexcept
{
    if (fd != InvalidHandle)
        ::close(fd);
}

/// Writes bytes to a native handle.
/// @param fd The file descriptor to write to.
/// @param data Pointer to data to write.
/// @param size Number of bytes to write.
/// @return Number of bytes written, or -1 on error.
inline auto platformWrite(NativeHandle fd, void const* data, size_t size) -> intptr_t
{
    return static_cast<intptr_t>(::write(fd, data, size));
}

/// Reads bytes from a native handle.
/// @param fd The file descriptor to read from.
/// @param data Pointer to buffer to read into.
/// @param size Maximum number of bytes to read.
/// @return Bytes read, 0 on EOF / broken pipe, or -1 on error.
inline auto platformRead(NativeHandle fd, void* data, size_t size) -> intptr_t
{
    return static_cast<intptr_t>(::read(fd, data, size));
}
#endif

} // namespace net
