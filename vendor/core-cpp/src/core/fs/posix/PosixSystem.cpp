// SPDX-License-Identifier: Apache-2.0
//
// The descriptor calls of Linux, the BSDs and any POSIX system but Apple's: the `st_atim` family of
// times, `posix_fadvise`, `fdatasync` and `fsync`. A write-integrity mode the system lacks (FreeBSD
// has no `O_RSYNC`) is a row whose flag is 0, chosen by the macro test inside the data.
#include <core/fs/posix/PosixSystem.hpp>

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/NativeSupport.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

namespace
{

    FileTime fileTimeOf(struct ::timespec const& time) noexcept
    {
        return FileTime { .seconds = time.tv_sec, .nanoseconds = static_cast<std::uint32_t>(time.tv_nsec) };
    }

    struct AdviceRow
    {
        Advice advice;
        int systemAdvice;
    };

    constexpr auto AdviceRows = std::to_array<AdviceRow>({
        { .advice = Advice::Normal, .systemAdvice = POSIX_FADV_NORMAL },
        { .advice = Advice::Sequential, .systemAdvice = POSIX_FADV_SEQUENTIAL },
        { .advice = Advice::Random, .systemAdvice = POSIX_FADV_RANDOM },
        { .advice = Advice::WillNeed, .systemAdvice = POSIX_FADV_WILLNEED },
        { .advice = Advice::DontNeed, .systemAdvice = POSIX_FADV_DONTNEED },
        { .advice = Advice::NoReuse, .systemAdvice = POSIX_FADV_NOREUSE },
    });

    struct SyncFlagRow
    {
        DescriptorFlags flag;
        int openFlag; ///< 0 where this system has no such mode.
    };

    constexpr auto SyncFlagRows = std::to_array<SyncFlagRow>({
        { .flag = DescriptorFlags::FileIntegritySync, .openFlag = O_SYNC },
#ifdef O_DSYNC
        { .flag = DescriptorFlags::DataIntegritySync, .openFlag = O_DSYNC },
#else
        { .flag = DescriptorFlags::DataIntegritySync, .openFlag = 0 },
#endif
#ifdef O_RSYNC
        { .flag = DescriptorFlags::RequestedWriteSync, .openFlag = O_RSYNC },
#else
        { .flag = DescriptorFlags::RequestedWriteSync, .openFlag = 0 },
#endif
    });

} // namespace

StatTimes timesOf(struct ::stat const& status) noexcept
{
    return StatTimes { .accessed = fileTimeOf(status.st_atim),
                       .modified = fileTimeOf(status.st_mtim),
                       .statusChanged = fileTimeOf(status.st_ctim) };
}

FsResult<void> adviseRange(int handle, std::uint64_t offset, std::uint64_t length, Advice advice)
{
    constexpr auto OffsetMax = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > OffsetMax || length > OffsetMax)
        return refuse(FsErrorCode::Invalid);
    auto const row = std::ranges::find(AdviceRows, advice, &AdviceRow::advice);
    if (row == AdviceRows.end())
        return refuse(FsErrorCode::Invalid);
    // posix_fadvise answers the error number rather than setting errno.
    if (auto const error = ::posix_fadvise(
            handle, static_cast<off_t>(offset), static_cast<off_t>(length), row->systemAdvice);
        error != 0)
        return std::unexpected { fsErrorFromSystemCode(error, "posix_fadvise") };
    return {};
}

FsResult<void> syncAll(int handle)
{
    if (retryOnInterrupt([handle] { return ::fsync(handle); }) != 0)
        return errnoError("fsync");
    return {};
}

FsResult<void> syncData(int handle)
{
    // Where fdatasync fails (a file system without it answers EINVAL), fsync, as SQLite and libuv do.
    if (retryOnInterrupt([handle] { return ::fdatasync(handle); }) == 0)
        return {};
    return syncAll(handle);
}

FsResult<int> syncOpenFlags(DescriptorFlags flags)
{
    auto openFlags = 0;
    for (auto const& row: SyncFlagRows)
    {
        if (!hasFlag(flags, row.flag))
            continue;
        if (row.openFlag == 0)
            return refuse(FsErrorCode::Unsupported);
        openFlags |= row.openFlag;
    }
    return openFlags;
}

FsError openFailure(int, std::string const&, int flags, int error)
{
    // A link that became the final component after the walk looked: FreeBSD's O_NOFOLLOW answers
    // EMLINK, which no other open sends, where the rest say ELOOP.
    if ((flags & O_NOFOLLOW) != 0
        && std::ranges::find(noFollowSymlinkErrnos(), error) != noFollowSymlinkErrnos().end())
        return FsError { .code = FsErrorCode::Loop, .systemCode = error, .context = "openat" };
    return fsErrorFromSystemCode(error, "openat");
}

} // namespace core::fs::detail
