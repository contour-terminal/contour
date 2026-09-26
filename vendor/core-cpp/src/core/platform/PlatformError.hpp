// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace core::platform
{

/// Error codes for platform-level operations (process, pipe, file, signal).
enum class PlatformError : std::uint8_t
{
    // Process errors
    ForkFailed,
    ExecFailed,
    WaitFailed,
    ProgramNotFound,

    // Pipe/handle errors
    PipeCreationFailed,
    HandleDuplicationFailed,

    // File errors
    FileNotFound,
    PermissionDenied,
    IoError,

    // Signal/session errors
    SignalFailed,
    SessionCreationFailed,
    ProcessGroupFailed,
    TerminalControlFailed,

    // General
    NotImplemented,
    /// An argument no implementation could act on: an environment variable name that is empty or
    /// holds '=' or NUL, for instance. Last, so the values before it keep their numbers.
    InvalidArgument,
};

/// Converts a PlatformError to a human-readable string.
///
/// @param error The error code to convert
/// @return A string view describing the error
[[nodiscard]] constexpr std::string_view toString(PlatformError error) noexcept
{
    switch (error)
    {
        case PlatformError::ForkFailed: return "fork failed";
        case PlatformError::ExecFailed: return "exec failed";
        case PlatformError::WaitFailed: return "wait failed";
        case PlatformError::ProgramNotFound: return "program not found";
        case PlatformError::PipeCreationFailed: return "pipe creation failed";
        case PlatformError::HandleDuplicationFailed: return "handle duplication failed";
        case PlatformError::FileNotFound: return "file not found";
        case PlatformError::PermissionDenied: return "permission denied";
        case PlatformError::IoError: return "I/O error";
        case PlatformError::SignalFailed: return "signal failed";
        case PlatformError::SessionCreationFailed: return "session creation failed";
        case PlatformError::ProcessGroupFailed: return "process group failed";
        case PlatformError::TerminalControlFailed: return "terminal control failed";
        case PlatformError::NotImplemented: return "not implemented";
        case PlatformError::InvalidArgument: return "invalid argument";
    }
    return "unknown error";
}

} // namespace core::platform
