// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The three columns of Linux's `/proc/<pid>/mountinfo` the relation walk reads for a mount: the
/// device of its file system (the superblock's, one for every btrfs subvolume of it), and its root,
/// the directory of that file system it shows -- `/` for a mount of a whole file system, the
/// subdirectory's path for a bind mount of one or a subvolume's -- and its file system's type.
/// Private to the fs module.

#include <core/fs/FsExport.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::fs::detail
{

/// One mount's line of mountinfo, the columns the relation walk reads.
struct MountEntry
{
    std::uint64_t device = 0; ///< Its file system's device, `major << 32 | minor`.
    std::string root;         ///< Its root, octal escapes (`\040`) decoded.
    std::string type;         ///< Its file system's type (`ext4`, `overlay`, `fuse.sshfs`).

    [[nodiscard]] bool operator==(MountEntry const&) const = default;
};

/// One mount's place, as the scan for mounts beneath a directory reads it.
struct MountPlace
{
    std::uint64_t id = 0;   ///< Its mount id.
    std::string mountPoint; ///< Where it is mounted, octal escapes decoded.

    [[nodiscard]] bool operator==(MountPlace const&) const = default;
};

/// @param mountInfo The text of a `mountinfo` file.
/// @return Every mount's id and mount point, in the file's order; nullopt where any line but the
///         empty one after the last newline is malformed: an id that is no number, a missing mount
///         point, or one with a bad escape or an escaped NUL.
[[nodiscard]] CORE_FS_API std::optional<std::vector<MountPlace>> mountPlacesOf(std::string_view mountInfo);

/// @param mountInfo The text of a `mountinfo` file.
/// @param mountId A mount's id (`statx`'s `stx_mnt_id`).
/// @return That mount's entry; nullopt where no well-formed line has the id. A line is malformed
///         where a column is missing (the `-` separator and the type after it included), the id or
///         the device is no number, or the root or the type holds a bad escape or an escaped NUL.
[[nodiscard]] CORE_FS_API std::optional<MountEntry> mountEntryOf(std::string_view mountInfo,
                                                                 std::uint64_t mountId);

} // namespace core::fs::detail
