// SPDX-License-Identifier: Apache-2.0
#include <vtpty/WindowsBeneathRules.hpp>

#include <algorithm>

namespace vtpty
{

namespace
{
    /// The FILETIME of 1970-01-01 00:00:00 UTC: 100 ns ticks since 1601-01-01.
    constexpr auto UnixEpochFileTime = uint64_t { 116'444'736'000'000'000 };

    /// The FILETIMEs in one millisecond: 100 ns ticks.
    constexpr auto FileTimeTicksPerMs = uint64_t { 10'000 };

    /// The attributes of an entry whose data is not on disk: opening or reading it would fetch it.
    constexpr auto DataOffDiskAttributes =
        WindowsAttribute::Offline | WindowsAttribute::RecallOnOpen | WindowsAttribute::RecallOnDataAccess;

    /// @param tag A reparse tag of an entry with FILE_ATTRIBUTE_REPARSE_POINT.
    /// @return Whether @p tag is one of AcceptedStorageReparseTags, its variable bits ignored by each row's
    /// mask.
    [[nodiscard]] bool isAcceptedStorageTag(uint32_t tag) noexcept
    {
        return std::ranges::any_of(AcceptedStorageReparseTags,
                                   [tag](StorageReparseTag row) { return (tag & ~row.mask) == row.tag; });
    }
} // namespace

FileKind windowsFileKindOf(WindowsEntryFacts const facts) noexcept
{
    auto const isDirectory = (facts.attributes & WindowsAttribute::Directory) != 0;
    auto const isReparsePoint = (facts.attributes & WindowsAttribute::ReparsePoint) != 0;

    if (isReparsePoint)
    {
        if ((facts.reparseTag & WindowsNameSurrogateBit) != 0)
            return FileKind::Symlink;
        if (!isAcceptedStorageTag(facts.reparseTag))
            return FileKind::Other;
    }

    auto const dataOffDisk = (facts.attributes & DataOffDiskAttributes) != 0;
    if (dataOffDisk && !isDirectory)
        return FileKind::Other;

    if (isDirectory)
        return FileKind::Directory;

    if (facts.handleType != WindowsHandleType::Disk)
        return FileKind::Other;

    return FileKind::Regular;
}

uint64_t unixMsOfFileTime(uint64_t const fileTime) noexcept
{
    if (fileTime < UnixEpochFileTime)
        return 0;
    return (fileTime - UnixEpochFileTime) / FileTimeTicksPerMs;
}

} // namespace vtpty
