// SPDX-License-Identifier: Apache-2.0
//
// The descriptor calls of Apple's systems: the `st_atimespec` family of times; `F_RDADVISE` for
// read-ahead, the one advice macOS can act on (it has no `posix_fadvise`); and `F_FULLFSYNC`, without
// which macOS's `fsync` leaves the data in the drive's cache. macOS has no `O_RSYNC`.
#include <core/fs/posix/PosixSystem.hpp>

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/NativeSupport.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
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

    /// What an @c Advice becomes here.
    enum class AdviceAction : std::uint8_t
    {
        Ignore,    ///< Nothing to tell the system; the advice succeeds.
        ReadAhead, ///< `F_RDADVISE` over the range.
    };

    struct AdviceRow
    {
        Advice advice;
        AdviceAction action;
    };

    constexpr auto AdviceRows = std::to_array<AdviceRow>({
        { .advice = Advice::Normal, .action = AdviceAction::Ignore },
        { .advice = Advice::Sequential, .action = AdviceAction::Ignore },
        { .advice = Advice::Random, .action = AdviceAction::Ignore },
        { .advice = Advice::WillNeed, .action = AdviceAction::ReadAhead },
        { .advice = Advice::DontNeed, .action = AdviceAction::Ignore },
        { .advice = Advice::NoReuse, .action = AdviceAction::Ignore },
    });

    struct SyncFlagRow
    {
        DescriptorFlags flag;
        int openFlag; ///< 0 where this system has no such mode.
    };

    constexpr auto SyncFlagRows = std::to_array<SyncFlagRow>({
        { .flag = DescriptorFlags::FileIntegritySync, .openFlag = O_SYNC },
        { .flag = DescriptorFlags::DataIntegritySync, .openFlag = O_DSYNC },
        { .flag = DescriptorFlags::RequestedWriteSync, .openFlag = 0 },
    });

    FsResult<void> readAhead(int handle, std::uint64_t offset, std::uint64_t length)
    {
        if (length == 0)
        {
            struct ::stat status {};
            if (::fstat(handle, &status) != 0)
                return errnoError("fstat");
            auto const size = static_cast<std::uint64_t>(status.st_size);
            length = offset < size ? size - offset : 0;
        }
        auto advisory =
            ::radvisory { .ra_offset = static_cast<off_t>(offset),
                          .ra_count = static_cast<int>(std::min<std::uint64_t>(length, INT_MAX)) };
        if (advisory.ra_count == 0)
            return {};
        if (::fcntl(handle, F_RDADVISE, &advisory) == -1)
            return errnoError("fcntl(F_RDADVISE)");
        return {};
    }

    /// `F_FULLFSYNC`, or `fsync` where it fails, as SQLite and libuv do: network and FUSE file
    /// systems answer ENOTSUP, ENOTTY or EINVAL for it, and `fsync` is the best they offer.
    FsResult<void> fullSync(int handle)
    {
        if (::fcntl(handle, F_FULLFSYNC) != -1)
            return {};
        if (retryOnInterrupt([handle] { return ::fsync(handle); }) != 0)
            return errnoError("fsync");
        return {};
    }

} // namespace

StatTimes timesOf(struct ::stat const& status) noexcept
{
    return StatTimes { .accessed = fileTimeOf(status.st_atimespec),
                       .modified = fileTimeOf(status.st_mtimespec),
                       .statusChanged = fileTimeOf(status.st_ctimespec) };
}

FsResult<void> adviseRange(int handle, std::uint64_t offset, std::uint64_t length, Advice advice)
{
    constexpr auto OffsetMax = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > OffsetMax || length > OffsetMax)
        return refuse(FsErrorCode::Invalid);
    auto const row = std::ranges::find(AdviceRows, advice, &AdviceRow::advice);
    if (row == AdviceRows.end())
        return refuse(FsErrorCode::Invalid);
    switch (row->action)
    {
        case AdviceAction::Ignore: return {};
        case AdviceAction::ReadAhead: return readAhead(handle, offset, length);
    }
    return refuse(FsErrorCode::Invalid);
}

FsResult<void> syncData(int handle)
{
    return fullSync(handle);
}

FsResult<void> syncAll(int handle)
{
    return fullSync(handle);
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

FsError openFailure(int directory, std::string const& name, int flags, int error)
{
    // macOS refuses O_CREAT on an existing directory with EACCES (EPERM on some file systems),
    // where Linux answers EISDIR: the name, unfollowed, tells which it was.
    struct ::stat status {};
    if ((error == EACCES || error == EPERM) && (flags & O_CREAT) != 0
        && ::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(status.st_mode))
        return FsError { .code = FsErrorCode::IsDirectory, .systemCode = error, .context = "openat" };
    if ((flags & O_NOFOLLOW) != 0
        && std::ranges::find(noFollowSymlinkErrnos(), error) != noFollowSymlinkErrnos().end())
        return FsError { .code = FsErrorCode::Loop, .systemCode = error, .context = "openat" };
    return fsErrorFromSystemCode(error, "openat");
}

} // namespace core::fs::detail
