// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A pipe whose end of this process is opened for overlapped I/O, which alone can be bounded by a timeout and
/// ended early: what a pseudoconsole and the process runner are both connected by.

#ifdef _WIN32

    #include <vtpty/Win32Handle.hpp>

    #include <cstdint>
    #include <expected>
    #include <string_view>
    #include <system_error>

namespace vtpty
{

/// Which way a pipe carries bytes, seen from this process.
enum class PipeDirection : uint8_t
{
    Inbound = 0, ///< This process reads; the other end writes.
    Outbound,    ///< This process writes; the other end reads (duplex, so that it may ask for room).
};

/// Both ends of a pipe made by createOverlappedPipe().
struct OverlappedPipe
{
    UniqueHandle ours;   ///< Opened for overlapped I/O; not inheritable.
    UniqueHandle theirs; ///< Synchronous, as a child or a pseudoconsole needs it; not inheritable.
};

/// Creates a one-instance, local-only named pipe whose end of this process is overlapped (CreatePipe() gives
/// no such end). Its name is unique across processes and time: purpose, pid, process start time, a counter.
/// @param direction Which way it carries bytes.
/// @param purpose Part of its name (`conpty-out`, `runner-stdout`, …).
/// @param bufferSize The pipe's buffer, each way.
/// @return The two ends; or why the pipe could not be made.
[[nodiscard]] std::expected<OverlappedPipe, std::error_code> createOverlappedPipe(PipeDirection direction,
                                                                                  std::wstring_view purpose,
                                                                                  std::uint32_t bufferSize);

} // namespace vtpty

#endif
