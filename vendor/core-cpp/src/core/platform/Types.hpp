// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Types.hpp
/// @brief Platform-agnostic type definitions for cross-platform compatibility.
///
/// This header does not include `<Windows.h>`: a consumer's translation unit that includes it
/// would get `min`/`max` macros and the rest of the Windows API with it. On Windows the handle
/// types are spelled as the types `HANDLE` and `DWORD` are (`void*`, `unsigned long`), and the
/// functions that call into the Windows API are defined out of line.

#include <cstddef>
#include <cstdint>

#ifndef _WIN32
    #include <sys/types.h>

    #include <unistd.h>
#endif

namespace core::platform
{

#ifdef _WIN32
/// Native file/pipe handle type for the current platform: a Windows `HANDLE`.
using NativeHandle = void*;

/// Process identifier type for the current platform: a Windows `DWORD`.
using ProcessId = unsigned long;

/// Invalid handle sentinel value, Windows' `INVALID_HANDLE_VALUE`.
/// Cannot be constexpr on Windows because the value is a pointer made from an integer. Spelled
/// `void* const` rather than `NativeHandle const`, which reads as a pointer to const and is not one.
inline void* const InvalidHandle = reinterpret_cast<NativeHandle>(static_cast<std::intptr_t>(-1));

/// Invalid process ID sentinel value.
constexpr ProcessId InvalidProcessId = 0;

/// @brief Returns the standard input handle for the current platform.
[[nodiscard]] NativeHandle standardInput() noexcept;

/// @brief Returns the standard output handle for the current platform.
[[nodiscard]] NativeHandle standardOutput() noexcept;

/// @brief Returns the standard error handle for the current platform.
[[nodiscard]] NativeHandle standardError() noexcept;
#else
/// Native file/pipe handle type for the current platform.
using NativeHandle = int;

/// Process identifier type for the current platform.
using ProcessId = pid_t;

/// Invalid handle sentinel value.
constexpr NativeHandle InvalidHandle = -1;

/// Invalid process ID sentinel value.
constexpr ProcessId InvalidProcessId = -1;

/// @brief Returns the standard input handle for the current platform.
constexpr auto standardInput() -> NativeHandle
{
    return STDIN_FILENO;
}

/// @brief Returns the standard output handle for the current platform.
constexpr auto standardOutput() -> NativeHandle
{
    return STDOUT_FILENO;
}

/// @brief Returns the standard error handle for the current platform.
constexpr auto standardError() -> NativeHandle
{
    return STDERR_FILENO;
}
#endif

/// @brief Converts a native handle to an integer suitable for the VM number type.
///
/// @c NativeHandle is an @c int on POSIX but a @c void* (@c HANDLE) on Windows.
/// A single @c static_cast cannot express both conversions (it rejects the
/// pointer-to-integer case), so this helper selects the correct cast at compile
/// time, keeping call sites portable and free of C-style casts.
///
/// @param handle The native handle to convert.
/// @return The handle reinterpreted as a signed 64-bit integer.
[[nodiscard]] inline auto nativeHandleToNumber(NativeHandle handle) noexcept -> std::int64_t
{
#ifdef _WIN32
    return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(handle));
#else
    return static_cast<std::int64_t>(handle);
#endif
}

#ifdef _WIN32
/// Cross-platform close using Windows CloseHandle.
///
/// @param h The native handle to close.
void platformClose(NativeHandle h) noexcept;

/// Cross-platform write using Windows WriteFile.
///
/// @param h The native handle to write to.
/// @param data Pointer to data to write.
/// @param size Number of bytes to write.
/// @return Number of bytes written, or -1 on error.
std::intptr_t platformWrite(NativeHandle h, void const* data, std::size_t size);

/// Cross-platform read using Windows ReadFile.
///
/// Semantics match POSIX `read(2)`: returns the number of bytes read, 0 on
/// end-of-file (including the case where a pipe's write end has been closed
/// and the buffer is drained), or -1 on a genuine I/O error.
///
/// @param h The native handle to read from.
/// @param data Pointer to buffer to read into.
/// @param size Maximum number of bytes to read.
/// @return Bytes read, 0 on EOF / broken pipe, or -1 on error.
std::intptr_t platformRead(NativeHandle h, void* data, std::size_t size);

/// Checks whether a native handle refers to a terminal (console) device.
///
/// @param h The native handle to check.
/// @return true if the handle is connected to a terminal.
[[nodiscard]] bool isTerminal(NativeHandle h) noexcept;
#else
/// Cross-platform close using POSIX close.
///
/// @param fd The file descriptor to close.
inline void platformClose(NativeHandle fd) noexcept
{
    if (fd != InvalidHandle)
        ::close(fd);
}

/// Cross-platform write using POSIX write.
///
/// @param fd The file descriptor to write to.
/// @param data Pointer to data to write.
/// @param size Number of bytes to write.
/// @return Number of bytes written, or -1 on error.
inline auto platformWrite(NativeHandle fd, void const* data, std::size_t size) -> std::intptr_t
{
    return static_cast<std::intptr_t>(::write(fd, data, size));
}

/// Cross-platform read using POSIX read.
///
/// Semantics are `read(2)`'s: the number of bytes read, 0 at the end of the file (a pipe whose
/// write end has closed, once it is drained, included), or -1 on an error, with `errno` set.
///
/// Under Emscripten a pipe has no end of file. Its pipes behave as if the read end were always
/// non-blocking: a read of an empty pipe fails with EAGAIN whether or not the writer has closed.
/// A drained pipe whose writer closed therefore reads -1 with `errno` EAGAIN there, never 0, and
/// the caller cannot tell it from a pipe that is merely empty for now. Code that must run there
/// learns that the writer is done some other way: a length sent first, a terminator, or the
/// writer's own completion.
///
/// @param fd The file descriptor to read from.
/// @param data Pointer to buffer to read into.
/// @param size Maximum number of bytes to read.
/// @return Bytes read, 0 at the end of the file (never for a pipe under Emscripten), or -1 on
///         error.
inline auto platformRead(NativeHandle fd, void* data, std::size_t size) -> std::intptr_t
{
    return static_cast<std::intptr_t>(::read(fd, data, size));
}

/// Checks whether a native handle refers to a terminal device.
///
/// @param fd The file descriptor to check.
/// @return true if the descriptor is connected to a terminal.
[[nodiscard]] inline bool isTerminal(NativeHandle fd) noexcept
{
    return ::isatty(fd) != 0;
}
#endif

} // namespace core::platform
