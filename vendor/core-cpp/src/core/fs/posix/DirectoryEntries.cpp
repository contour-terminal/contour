// SPDX-License-Identifier: Apache-2.0
//
// What a POSIX directory entry is: its d_type, or a stat of its name without following where the
// file system does not say; the same on every POSIX system.
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/posix/DirectoryRead.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>

namespace core::fs::detail
{

namespace
{

    struct ModeRow
    {
        ::mode_t format;
        DescriptorType type;
    };

    constexpr auto ModeRows = std::to_array<ModeRow>({
        { .format = S_IFREG, .type = DescriptorType::RegularFile },
        { .format = S_IFDIR, .type = DescriptorType::Directory },
        { .format = S_IFLNK, .type = DescriptorType::SymbolicLink },
        { .format = S_IFCHR, .type = DescriptorType::CharacterDevice },
        { .format = S_IFBLK, .type = DescriptorType::BlockDevice },
        { .format = S_IFIFO, .type = DescriptorType::Fifo },
        { .format = S_IFSOCK, .type = DescriptorType::Socket },
    });

    struct EntryTypeRow
    {
        unsigned char entryType; ///< `d_type`.
        DescriptorType type;
    };

    constexpr auto EntryTypeRows = std::to_array<EntryTypeRow>({
        { .entryType = DT_REG, .type = DescriptorType::RegularFile },
        { .entryType = DT_DIR, .type = DescriptorType::Directory },
        { .entryType = DT_LNK, .type = DescriptorType::SymbolicLink },
        { .entryType = DT_CHR, .type = DescriptorType::CharacterDevice },
        { .entryType = DT_BLK, .type = DescriptorType::BlockDevice },
        { .entryType = DT_FIFO, .type = DescriptorType::Fifo },
        { .entryType = DT_SOCK, .type = DescriptorType::Socket },
    });

    /// @return What the entry is; `Unknown` where its stat fails too (it went meanwhile).
    DescriptorType typeOfEntry(int directory, char const* name, unsigned char entryType)
    {
        auto const row = std::ranges::find(EntryTypeRows, entryType, &EntryTypeRow::entryType);
        if (row != EntryTypeRows.end())
            return row->type;
        struct ::stat status {};
        if (directory < 0 || ::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0)
            return DescriptorType::Unknown;
        return typeOfMode(status.st_mode);
    }

} // namespace

DescriptorType typeOfMode(::mode_t mode) noexcept
{
    auto const row = std::ranges::find(ModeRows, mode & S_IFMT, &ModeRow::format);
    return row == ModeRows.end() ? DescriptorType::Unknown : row->type;
}

std::optional<FsResult<DirectoryEntry>> entryOf(int directory, char const* name, unsigned char entryType)
{
    auto const text = std::string_view { name };
    if (text == "." || text == "..")
        return std::nullopt;
    // No file system hands out such a name; one that did would name another entry, or a path.
    if (text.empty() || text.contains('/'))
        return FsResult<DirectoryEntry> { std::unexpected {
            FsError { .code = FsErrorCode::Io, .systemCode = 0, .context = "directory entry name" } } };
    if (!isUtf8(text))
        return FsResult<DirectoryEntry> { std::unexpected { FsError {
            .code = FsErrorCode::IllegalByteSequence, .systemCode = 0, .context = "directory entry" } } };
    return FsResult<DirectoryEntry> { DirectoryEntry { .type = typeOfEntry(directory, name, entryType),
                                                       .name = std::string { text } } };
}

} // namespace core::fs::detail
