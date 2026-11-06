// SPDX-License-Identifier: Apache-2.0
//
// The decision behind tryAttachConsole() in main.cpp: which standard handles a GUI-subsystem process keeps
// after attaching to its parent's console. Pure, so it is testable without a console.
#pragma once

#include <cstdint>

namespace contour::platform
{

/// What a standard handle was before the process attached to its parent's console.
enum class StdHandleKind : uint8_t
{
    /// Null, invalid, or a handle of no known file type.
    Absent = 0,
    /// A console buffer.
    Console,
    /// A pipe, a file, or a character device that is no console (`NUL`, a serial port) the parent redirected
    /// the stream to.
    Redirected,
};

/// Whether a standard handle is one at all: GetStdHandle() returns null for none and INVALID_HANDLE_VALUE
/// on failure.
enum class HandleValidity : uint8_t
{
    Invalid = 0, ///< Null or INVALID_HANDLE_VALUE.
    Valid,       ///< Anything else.
};

/// Whether GetConsoleMode() succeeds on a handle, which it does for a console buffer alone.
enum class ConsoleMode : uint8_t
{
    Unavailable = 0, ///< It fails: no console buffer.
    Available,       ///< It succeeds: a console buffer.
};

/// What GetFileType() reports for a handle.
enum class StdFileType : uint8_t
{
    Unknown = 0, ///< FILE_TYPE_UNKNOWN, or anything else not below (FILE_TYPE_REMOTE).
    Disk,        ///< FILE_TYPE_DISK: a file.
    Char,        ///< FILE_TYPE_CHAR: a console, `NUL`, a serial port.
    Pipe,        ///< FILE_TYPE_PIPE: an anonymous or a named pipe.
};

/// Classifies a standard handle as the process was started with it.
/// @param validity Whether the handle is one at all.
/// @param consoleMode Whether GetConsoleMode() succeeds on it.
/// @param fileType What GetFileType() reports for it.
/// @return Console where the console mode is available, Redirected for a pipe, a file or another character
///         device, and Absent for an invalid handle or an unknown file type.
[[nodiscard]] constexpr StdHandleKind classifyStdHandle(HandleValidity validity,
                                                        ConsoleMode consoleMode,
                                                        StdFileType fileType) noexcept
{
    if (validity == HandleValidity::Invalid)
        return StdHandleKind::Absent;
    if (consoleMode == ConsoleMode::Available)
        return StdHandleKind::Console;
    switch (fileType)
    {
        case StdFileType::Disk:
        case StdFileType::Char:
        case StdFileType::Pipe: return StdHandleKind::Redirected;
        case StdFileType::Unknown: return StdHandleKind::Absent;
    }
    return StdHandleKind::Absent;
}

/// What attaching to the parent's console does with a standard handle.
enum class AfterAttach : uint8_t
{
    TakeConsole = 0, ///< Let the attach replace the handle with the console's.
    Restore,         ///< Put the handle the process was started with back after the attach.
};

/// AttachConsole(ATTACH_PARENT_PROCESS) replaces every standard handle the process was not started with
/// through STARTF_USESTDHANDLES, which is not how cmd.exe hands them over (it inherits them), so a
/// redirection would be lost to the console unless it is put back.
/// @param before The kind of the handle before attaching.
/// @return Whether the console's handle replaces it, or the saved one is restored.
[[nodiscard]] constexpr AfterAttach afterAttach(StdHandleKind before) noexcept
{
    return before == StdHandleKind::Redirected ? AfterAttach::Restore : AfterAttach::TakeConsole;
}

} // namespace contour::platform
