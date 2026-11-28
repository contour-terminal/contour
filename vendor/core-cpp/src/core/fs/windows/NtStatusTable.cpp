// SPDX-License-Identifier: Apache-2.0
//
// The NTSTATUS table: what NtCreateFile and NtSetInformationFile answer, mapped directly rather than
// through RtlNtStatusToDosError, whose Win32 codes lose distinctions the WASI codes keep.
#include <core/fs/FsError.hpp>

#include <array>
#include <cstdint>
#include <span>

// <ntstatus.h> defines every STATUS_ code, and <windows.h> defines some of them again unless it is
// told not to: the documented order is this one. The codes are spelled `((NTSTATUS) ...)`, and every
// core-cpp target builds with WIN32_LEAN_AND_MEAN, under which <windows.h> no longer declares
// NTSTATUS; <winternl.h> does.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

namespace core::fs
{

namespace
{

    /// @return @p status as the signed 32-bit value the API returns, widened.
    constexpr std::int64_t statusCode(NTSTATUS status) noexcept
    {
        return std::int64_t { status };
    }

    constexpr auto NtStatusRows = std::to_array<SystemErrorRow>({
        { .systemCode = statusCode(STATUS_ACCESS_DENIED), .code = FsErrorCode::Access },
        { .systemCode = statusCode(STATUS_DELETE_PENDING), .code = FsErrorCode::Access },
        { .systemCode = statusCode(STATUS_CANNOT_DELETE), .code = FsErrorCode::Access },
        { .systemCode = statusCode(STATUS_OBJECT_NAME_NOT_FOUND), .code = FsErrorCode::NoEntry },
        { .systemCode = statusCode(STATUS_OBJECT_PATH_NOT_FOUND), .code = FsErrorCode::NoEntry },
        { .systemCode = statusCode(STATUS_NO_SUCH_FILE), .code = FsErrorCode::NoEntry },
        { .systemCode = statusCode(STATUS_OBJECT_NAME_COLLISION), .code = FsErrorCode::Exist },
        { .systemCode = statusCode(STATUS_FILE_IS_A_DIRECTORY), .code = FsErrorCode::IsDirectory },
        { .systemCode = statusCode(STATUS_NOT_A_DIRECTORY), .code = FsErrorCode::NotDirectory },
        { .systemCode = statusCode(STATUS_DIRECTORY_NOT_EMPTY), .code = FsErrorCode::NotEmpty },
        { .systemCode = statusCode(STATUS_NOT_SAME_DEVICE), .code = FsErrorCode::CrossDevice },
        { .systemCode = statusCode(STATUS_DISK_FULL), .code = FsErrorCode::InsufficientSpace },
        { .systemCode = statusCode(STATUS_DISK_QUOTA_EXCEEDED), .code = FsErrorCode::Quota },
        { .systemCode = statusCode(STATUS_SHARING_VIOLATION), .code = FsErrorCode::Busy },
        { .systemCode = statusCode(STATUS_FILE_LOCK_CONFLICT), .code = FsErrorCode::Busy },
        { .systemCode = statusCode(STATUS_LOCK_NOT_GRANTED), .code = FsErrorCode::Busy },
        { .systemCode = statusCode(STATUS_OBJECT_NAME_INVALID), .code = FsErrorCode::Invalid },
        { .systemCode = statusCode(STATUS_OBJECT_PATH_SYNTAX_BAD), .code = FsErrorCode::Invalid },
        { .systemCode = statusCode(STATUS_INVALID_PARAMETER), .code = FsErrorCode::Invalid },
        { .systemCode = statusCode(STATUS_NOT_A_REPARSE_POINT), .code = FsErrorCode::Invalid },
        { .systemCode = statusCode(STATUS_STOPPED_ON_SYMLINK), .code = FsErrorCode::Loop },
        { .systemCode = statusCode(STATUS_REPARSE_POINT_NOT_RESOLVED), .code = FsErrorCode::Loop },
        // OBJ_DONT_REPARSE met a reparse point. Every open here names one component beneath a held
        // handle, so the point is that component itself: a link that is not followed, which is
        // Loop, as POSIX answers an O_NOFOLLOW open of one with ELOOP.
        { .systemCode = statusCode(STATUS_REPARSE_POINT_ENCOUNTERED), .code = FsErrorCode::Loop },
        { .systemCode = statusCode(STATUS_IO_REPARSE_TAG_NOT_HANDLED), .code = FsErrorCode::Unsupported },
        { .systemCode = statusCode(STATUS_NOT_SUPPORTED), .code = FsErrorCode::Unsupported },
        { .systemCode = statusCode(STATUS_INVALID_DEVICE_REQUEST), .code = FsErrorCode::Unsupported },
        { .systemCode = statusCode(STATUS_INVALID_HANDLE), .code = FsErrorCode::BadDescriptor },
        { .systemCode = statusCode(STATUS_NAME_TOO_LONG), .code = FsErrorCode::NameTooLong },
        { .systemCode = statusCode(STATUS_NO_MEMORY), .code = FsErrorCode::InsufficientMemory },
        { .systemCode = statusCode(STATUS_INSUFFICIENT_RESOURCES), .code = FsErrorCode::InsufficientMemory },
        { .systemCode = statusCode(STATUS_MEDIA_WRITE_PROTECTED), .code = FsErrorCode::ReadOnly },
        { .systemCode = statusCode(STATUS_PRIVILEGE_NOT_HELD), .code = FsErrorCode::NotPermitted },
        { .systemCode = statusCode(STATUS_TOO_MANY_LINKS), .code = FsErrorCode::TooManyLinks },
        { .systemCode = statusCode(STATUS_FILE_TOO_LARGE), .code = FsErrorCode::FileTooLarge },
        { .systemCode = statusCode(STATUS_NO_SUCH_DEVICE), .code = FsErrorCode::NoSuchDevice },
    });

} // namespace

std::span<SystemErrorRow const> ntStatusTable() noexcept
{
    return NtStatusRows;
}

} // namespace core::fs
