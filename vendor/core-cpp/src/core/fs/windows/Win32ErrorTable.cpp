// SPDX-License-Identifier: Apache-2.0
//
// The Win32 error table, which the Win32 calls of the native descriptors answer in, and the empty
// O_NOFOLLOW list: Windows has no errno for an open to report.
#include <core/fs/FsError.hpp>

#include <array>
#include <span>

#include <windows.h>

namespace core::fs
{

namespace
{

    constexpr auto Win32ErrorRows = std::to_array<SystemErrorRow>({
        { .systemCode = ERROR_ACCESS_DENIED, .code = FsErrorCode::Access },
        { .systemCode = ERROR_DELETE_PENDING, .code = FsErrorCode::Access },
        { .systemCode = ERROR_FILE_NOT_FOUND, .code = FsErrorCode::NoEntry },
        { .systemCode = ERROR_PATH_NOT_FOUND, .code = FsErrorCode::NoEntry },
        { .systemCode = ERROR_ALREADY_EXISTS, .code = FsErrorCode::Exist },
        { .systemCode = ERROR_FILE_EXISTS, .code = FsErrorCode::Exist },
        { .systemCode = ERROR_DIR_NOT_EMPTY, .code = FsErrorCode::NotEmpty },
        { .systemCode = ERROR_DIRECTORY, .code = FsErrorCode::NotDirectory },
        { .systemCode = ERROR_NOT_SAME_DEVICE, .code = FsErrorCode::CrossDevice },
        { .systemCode = ERROR_DISK_FULL, .code = FsErrorCode::InsufficientSpace },
        { .systemCode = ERROR_HANDLE_DISK_FULL, .code = FsErrorCode::InsufficientSpace },
        { .systemCode = ERROR_DISK_QUOTA_EXCEEDED, .code = FsErrorCode::Quota },
        { .systemCode = ERROR_SHARING_VIOLATION, .code = FsErrorCode::Busy },
        { .systemCode = ERROR_LOCK_VIOLATION, .code = FsErrorCode::Busy },
        { .systemCode = ERROR_BUSY, .code = FsErrorCode::Busy },
        { .systemCode = ERROR_INVALID_NAME, .code = FsErrorCode::Invalid },
        { .systemCode = ERROR_INVALID_PARAMETER, .code = FsErrorCode::Invalid },
        { .systemCode = ERROR_NOT_A_REPARSE_POINT, .code = FsErrorCode::Invalid },
        { .systemCode = ERROR_CANT_RESOLVE_FILENAME, .code = FsErrorCode::Loop },
        { .systemCode = ERROR_STOPPED_ON_SYMLINK, .code = FsErrorCode::Loop },
        { .systemCode = ERROR_NO_UNICODE_TRANSLATION, .code = FsErrorCode::IllegalByteSequence },
        { .systemCode = ERROR_INVALID_HANDLE, .code = FsErrorCode::BadDescriptor },
        { .systemCode = ERROR_NOT_ENOUGH_MEMORY, .code = FsErrorCode::InsufficientMemory },
        { .systemCode = ERROR_OUTOFMEMORY, .code = FsErrorCode::InsufficientMemory },
        { .systemCode = ERROR_FILENAME_EXCED_RANGE, .code = FsErrorCode::NameTooLong },
        { .systemCode = ERROR_NOT_SUPPORTED, .code = FsErrorCode::Unsupported },
        { .systemCode = ERROR_WRITE_PROTECT, .code = FsErrorCode::ReadOnly },
        { .systemCode = ERROR_PRIVILEGE_NOT_HELD, .code = FsErrorCode::NotPermitted },
        { .systemCode = ERROR_TOO_MANY_LINKS, .code = FsErrorCode::TooManyLinks },
        { .systemCode = ERROR_FILE_TOO_LARGE, .code = FsErrorCode::FileTooLarge },
        { .systemCode = ERROR_BROKEN_PIPE, .code = FsErrorCode::Pipe },
        { .systemCode = ERROR_NEGATIVE_SEEK, .code = FsErrorCode::InvalidSeek },
    });

} // namespace

std::span<SystemErrorRow const> systemErrorTable() noexcept
{
    return Win32ErrorRows;
}

std::span<int const> noFollowSymlinkErrnos() noexcept
{
    return {};
}

} // namespace core::fs
