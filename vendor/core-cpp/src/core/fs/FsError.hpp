// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The error vocabulary of the file system: an @c FsErrorCode category, a structured @c FsError that
/// adds the raw system code and a context string, and the per-OS tables that classify a system code.

#include <core/fs/FsExport.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace core::fs
{

/// Classifies a file-system failure.
///
/// One code for each case of the WASI 0.3 `wasi:filesystem` `error-code`, in its order, plus
/// @c WouldBlock, which that interface has no case of its own for. The order is the interface's,
/// so the zero enumerator is @c Access rather than an absent case: an @c FsError always carries a
/// failure, and its member defaults to @c Io.
enum class FsErrorCode : std::uint8_t
{
    Access,              ///< Permission denied by the system (`EACCES`).
    WouldBlock,          ///< The operation would block (`EAGAIN`).
    Already,             ///< The connection or operation is already in progress (`EALREADY`).
    BadDescriptor,       ///< The descriptor is closed or not one of this file system's (`EBADF`).
    Busy,                ///< The resource is in use (`EBUSY`, a sharing violation).
    Deadlock,            ///< The operation would deadlock (`EDEADLK`).
    Quota,               ///< A storage quota is exceeded (`EDQUOT`).
    Exist,               ///< The file exists (`EEXIST`).
    FileTooLarge,        ///< The file is too large (`EFBIG`).
    IllegalByteSequence, ///< A name is not valid UTF-8, or cannot be represented (`EILSEQ`).
    InProgress,          ///< The operation is in progress (`EINPROGRESS`).
    Interrupted,         ///< A signal interrupted the call (`EINTR`).
    Invalid,             ///< An argument is invalid (`EINVAL`), a name included.
    Io,                  ///< An I/O error, and every system code nothing classifies further (`EIO`).
    IsDirectory,         ///< The object is a directory (`EISDIR`).
    Loop,                ///< Too many symbolic links, or a link where none may be followed (`ELOOP`).
    TooManyLinks,        ///< Too many hard links (`EMLINK`).
    MessageSize,         ///< A message is too large (`EMSGSIZE`).
    NameTooLong,         ///< A name is too long (`ENAMETOOLONG`).
    NoDevice,            ///< No such device (`ENODEV`).
    NoEntry,             ///< No such file or directory (`ENOENT`).
    NoLock,              ///< No locks are available (`ENOLCK`).
    InsufficientMemory,  ///< Not enough memory (`ENOMEM`).
    InsufficientSpace,   ///< No space left on the device (`ENOSPC`).
    NotDirectory,        ///< A component that must be a directory is not one (`ENOTDIR`).
    NotEmpty,            ///< The directory is not empty (`ENOTEMPTY`).
    NotRecoverable,      ///< The state is not recoverable (`ENOTRECOVERABLE`).
    Unsupported,         ///< The operation is not supported (`ENOTSUP`).
    NoTty,               ///< Not a terminal (`ENOTTY`).
    NoSuchDevice,        ///< No such device or address (`ENXIO`).
    Overflow,            ///< A value does not fit its type (`EOVERFLOW`).
    NotPermitted,        ///< Refused by policy: a right the descriptor lacks, a path that leaves it,
                         ///< a reserved name (`EPERM`).
    Pipe,                ///< The other end of a pipe is gone (`EPIPE`).
    ReadOnly,            ///< The file system is read-only (`EROFS`).
    InvalidSeek,         ///< The object cannot seek (`ESPIPE`).
    TextFileBusy,        ///< The file is an executable in use (`ETXTBSY`).
    CrossDevice,         ///< The operation crosses file systems, or names a descriptor of another
                         ///< file system instance (`EXDEV`).
    Last, ///< Not a code: the number of codes above it, so a table or a test can walk every one of
          ///< them. **A new code goes above it, never below.**
};

/// @param code The error code to describe.
/// @return Lower-case words naming @p code, or `"unknown error"` for a value that is not one of the
///         codes (including @c FsErrorCode::Last). The switch has no `default`, so a new code makes
///         every compiler name this function.
[[nodiscard]] constexpr std::string_view toString(FsErrorCode code) noexcept
{
    switch (code)
    {
        case FsErrorCode::Access: return "access";
        case FsErrorCode::WouldBlock: return "would block";
        case FsErrorCode::Already: return "already";
        case FsErrorCode::BadDescriptor: return "bad descriptor";
        case FsErrorCode::Busy: return "busy";
        case FsErrorCode::Deadlock: return "deadlock";
        case FsErrorCode::Quota: return "quota";
        case FsErrorCode::Exist: return "exist";
        case FsErrorCode::FileTooLarge: return "file too large";
        case FsErrorCode::IllegalByteSequence: return "illegal byte sequence";
        case FsErrorCode::InProgress: return "in progress";
        case FsErrorCode::Interrupted: return "interrupted";
        case FsErrorCode::Invalid: return "invalid";
        case FsErrorCode::Io: return "io";
        case FsErrorCode::IsDirectory: return "is directory";
        case FsErrorCode::Loop: return "loop";
        case FsErrorCode::TooManyLinks: return "too many links";
        case FsErrorCode::MessageSize: return "message size";
        case FsErrorCode::NameTooLong: return "name too long";
        case FsErrorCode::NoDevice: return "no device";
        case FsErrorCode::NoEntry: return "no entry";
        case FsErrorCode::NoLock: return "no lock";
        case FsErrorCode::InsufficientMemory: return "insufficient memory";
        case FsErrorCode::InsufficientSpace: return "insufficient space";
        case FsErrorCode::NotDirectory: return "not directory";
        case FsErrorCode::NotEmpty: return "not empty";
        case FsErrorCode::NotRecoverable: return "not recoverable";
        case FsErrorCode::Unsupported: return "unsupported";
        case FsErrorCode::NoTty: return "no tty";
        case FsErrorCode::NoSuchDevice: return "no such device";
        case FsErrorCode::Overflow: return "overflow";
        case FsErrorCode::NotPermitted: return "not permitted";
        case FsErrorCode::Pipe: return "pipe";
        case FsErrorCode::ReadOnly: return "read only";
        case FsErrorCode::InvalidSeek: return "invalid seek";
        case FsErrorCode::TextFileBusy: return "text file busy";
        case FsErrorCode::CrossDevice: return "cross device";
        case FsErrorCode::Last: break;
    }
    return "unknown error";
}

/// A structured file-system error: a category, the raw system code it was classified from, and an
/// optional context string for diagnostics.
struct FsError
{
    FsErrorCode code = FsErrorCode::Io; ///< The error category.
    std::int64_t systemCode = 0;        ///< The raw errno, Win32 error or NTSTATUS, or 0 if none.
    std::string context;                ///< Optional human context (e.g. the failing call).

    /// @return The category in words, then the context and the system code where there are any,
    ///         for example `no entry (openat) [system code 2]`.
    [[nodiscard]] CORE_FS_API std::string toString() const;
};

/// The result of a file-system operation.
template <typename T>
using FsResult = std::expected<T, FsError>;

/// One row of a table that classifies a system code.
struct SystemErrorRow
{
    std::int64_t systemCode; ///< The errno, Win32 error or NTSTATUS.
    FsErrorCode code;        ///< What it means.
};

/// @return This platform's errno table, or its Win32 error table on Windows.
[[nodiscard]] CORE_FS_API std::span<SystemErrorRow const> systemErrorTable() noexcept;

/// @return The NTSTATUS table on Windows, which the native API (`NtCreateFile`) answers in; empty
///         elsewhere.
[[nodiscard]] CORE_FS_API std::span<SystemErrorRow const> ntStatusTable() noexcept;

/// @return The errno values an `O_NOFOLLOW` open answers when its final component is a symbolic
///         link: `ELOOP`, and `EMLINK` too where FreeBSD answers that; empty on Windows.
[[nodiscard]] CORE_FS_API std::span<int const> noFollowSymlinkErrnos() noexcept;

/// Classifies an errno, or a Win32 error on Windows, through @c systemErrorTable.
/// @param systemCode The raw code, kept in the result.
/// @param context What failed, for diagnostics.
/// @return The error; @c FsErrorCode::Io for a code the table does not hold.
[[nodiscard]] CORE_FS_API FsError fsErrorFromSystemCode(std::int64_t systemCode, std::string context = {});

/// Classifies an NTSTATUS through @c ntStatusTable.
/// @param status The raw status, sign-extended from its 32 bits, and kept in the result.
/// @param context What failed, for diagnostics.
/// @return The error; @c FsErrorCode::Io for a status the table does not hold, which off Windows is
///         every status.
[[nodiscard]] CORE_FS_API FsError fsErrorFromNtStatus(std::int64_t status, std::string context = {});

} // namespace core::fs
