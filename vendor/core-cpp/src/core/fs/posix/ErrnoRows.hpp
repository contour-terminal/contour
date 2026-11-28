// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The errno classification every POSIX system shares, spelled by macro so each system's own values
/// fill it. Private: included by `posix/ErrnoTable.cpp` and `bsd/ErrnoTable.cpp`, which differ only
/// in what an `O_NOFOLLOW` open answers for a symbolic link.

#include <core/fs/FsError.hpp>

#include <array>
#include <cerrno>

namespace core::fs::detail
{

/// Every errno the WASI `error-code` has a case for. `EWOULDBLOCK` and `EOPNOTSUPP` have rows of
/// their own because a system may give them values of their own; where they equal `EAGAIN` and
/// `ENOTSUP` the first row answers.
inline constexpr auto ErrnoRows = std::to_array<SystemErrorRow>({
    { .systemCode = EACCES, .code = FsErrorCode::Access },
    { .systemCode = EAGAIN, .code = FsErrorCode::WouldBlock },
    { .systemCode = EWOULDBLOCK, .code = FsErrorCode::WouldBlock },
    { .systemCode = EALREADY, .code = FsErrorCode::Already },
    { .systemCode = EBADF, .code = FsErrorCode::BadDescriptor },
    { .systemCode = EBUSY, .code = FsErrorCode::Busy },
    { .systemCode = EDEADLK, .code = FsErrorCode::Deadlock },
    { .systemCode = EDQUOT, .code = FsErrorCode::Quota },
    { .systemCode = EEXIST, .code = FsErrorCode::Exist },
    { .systemCode = EFBIG, .code = FsErrorCode::FileTooLarge },
    { .systemCode = EILSEQ, .code = FsErrorCode::IllegalByteSequence },
    { .systemCode = EINPROGRESS, .code = FsErrorCode::InProgress },
    { .systemCode = EINTR, .code = FsErrorCode::Interrupted },
    { .systemCode = EINVAL, .code = FsErrorCode::Invalid },
    { .systemCode = EIO, .code = FsErrorCode::Io },
    { .systemCode = EISDIR, .code = FsErrorCode::IsDirectory },
    { .systemCode = ELOOP, .code = FsErrorCode::Loop },
    { .systemCode = EMLINK, .code = FsErrorCode::TooManyLinks },
    { .systemCode = EMSGSIZE, .code = FsErrorCode::MessageSize },
    { .systemCode = ENAMETOOLONG, .code = FsErrorCode::NameTooLong },
    { .systemCode = ENODEV, .code = FsErrorCode::NoDevice },
    { .systemCode = ENOENT, .code = FsErrorCode::NoEntry },
    { .systemCode = ENOLCK, .code = FsErrorCode::NoLock },
    { .systemCode = ENOMEM, .code = FsErrorCode::InsufficientMemory },
    { .systemCode = ENOSPC, .code = FsErrorCode::InsufficientSpace },
    { .systemCode = ENOTDIR, .code = FsErrorCode::NotDirectory },
    { .systemCode = ENOTEMPTY, .code = FsErrorCode::NotEmpty },
    { .systemCode = ENOTRECOVERABLE, .code = FsErrorCode::NotRecoverable },
    { .systemCode = ENOTSUP, .code = FsErrorCode::Unsupported },
    { .systemCode = EOPNOTSUPP, .code = FsErrorCode::Unsupported },
    { .systemCode = ENOTTY, .code = FsErrorCode::NoTty },
    { .systemCode = ENXIO, .code = FsErrorCode::NoSuchDevice },
    { .systemCode = EOVERFLOW, .code = FsErrorCode::Overflow },
    { .systemCode = EPERM, .code = FsErrorCode::NotPermitted },
    { .systemCode = EPIPE, .code = FsErrorCode::Pipe },
    { .systemCode = EROFS, .code = FsErrorCode::ReadOnly },
    { .systemCode = ESPIPE, .code = FsErrorCode::InvalidSeek },
    { .systemCode = ETXTBSY, .code = FsErrorCode::TextFileBusy },
    { .systemCode = EXDEV, .code = FsErrorCode::CrossDevice },
});

} // namespace core::fs::detail
