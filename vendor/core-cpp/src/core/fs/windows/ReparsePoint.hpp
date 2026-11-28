// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a reparse point holds, read and written through `FSCTL_GET_REPARSE_POINT` and
/// `FSCTL_SET_REPARSE_POINT`, and what a walk may do with it. The data's layout is the driver kit's
/// `REPARSE_DATA_BUFFER`, which `<winnt.h>` does not declare: it is read and written here field by
/// field at its documented offsets, so no structure with a trailing array is cast over bytes.
///
/// A symbolic link is followed only where it is relative; a junction, a mount point and every other
/// name surrogate name a place by an absolute path, and are never followed. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/windows/NtFile.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::fs::detail
{

/// `SYMLINK_FLAG_RELATIVE` of the driver kit: the link's target is relative to the directory that
/// holds the link.
inline constexpr std::uint32_t SymlinkFlagRelative = 0x0000'0001U;

/// Whether a link's target is resolved from the directory holding the link.
enum class LinkTarget : std::uint8_t
{
    Absolute, ///< From a volume or the object namespace (`\??\C:\...`): never followed.
    Relative, ///< From the directory holding the link.
};

/// What a reparse point holds, as far as a walk needs it.
struct ReparseData
{
    std::uint32_t tag = 0;                    ///< The reparse tag.
    std::wstring substituteName;              ///< What the system follows (symbolic links and mount points).
    std::wstring printName;                   ///< What a user is shown; may be empty.
    LinkTarget target = LinkTarget::Absolute; ///< How @c substituteName is resolved.
};

/// @param tag A reparse tag.
/// @return Whether a point with @p tag names another object (a symbolic link, a junction, a mount
///         point), which is a link to a walk; any other point is a file of its own whose filter
///         supplies its data (a cloud placeholder, a deduplicated file).
[[nodiscard]] constexpr bool isNameSurrogate(std::uint32_t tag) noexcept
{
    return (tag & 0x2000'0000U) != 0;
}

/// Encodes a relative symbolic link's reparse data.
/// @param substituteName The target as the system follows it, separated by `\`.
/// @param printName The target as it was given, read back by `readlinkAt`.
/// @return The data, for `FSCTL_SET_REPARSE_POINT`.
[[nodiscard]] CORE_FS_API std::vector<std::byte> relativeSymlinkReparseData(std::wstring_view substituteName,
                                                                            std::wstring_view printName);

/// Encodes a mount point's reparse data, as a junction is made: no flags, and the names after the
/// path offsets.
/// @param substituteName What the system follows (`\??\C:\dir\` or `\??\Volume{...}\`).
/// @param printName What a user is shown.
/// @return The data, for `FSCTL_SET_REPARSE_POINT`.
[[nodiscard]] CORE_FS_API std::vector<std::byte> mountPointReparseData(std::wstring_view substituteName,
                                                                       std::wstring_view printName);

/// Decodes reparse data.
/// @param bytes What `FSCTL_GET_REPARSE_POINT` returned.
/// @return The tag, with the names and the relative flag of a symbolic link or a mount point;
///         `Invalid` where the data is shorter than it says or its names lie outside it.
[[nodiscard]] CORE_FS_API FsResult<ReparseData> parseReparseData(std::span<std::byte const> bytes);

/// What a walk follows: the target of a relative symbolic link.
/// @param data A decoded point.
/// @return The target in UTF-8; `NotPermitted` for an absolute link, a junction, a mount point or
///         any other name surrogate, whose target lies outside every directory a walk holds;
///         `Invalid` for a point that is no link at all.
[[nodiscard]] CORE_FS_API FsResult<std::string> followedTarget(ReparseData const& data);

/// What `readlinkAt` answers: the target of a relative symbolic link as it was given. That is the
/// print name, where it spells the substitute name with `/` for some of its `\`, and the substitute
/// name otherwise, so that what is read back is always what is followed.
/// @param data A decoded point.
/// @return The target in UTF-8; what @c followedTarget refuses, refused alike.
[[nodiscard]] CORE_FS_API FsResult<std::string> reportedTarget(ReparseData const& data);

/// Reads @p handle's reparse data.
/// @param handle A file opened as the point itself.
/// @return The data; `Invalid` where it is no reparse point.
[[nodiscard]] CORE_FS_API FsResult<ReparseData> readReparseData(HANDLE handle);

/// Writes @p data as @p handle's reparse point.
/// @param handle A file or directory opened with write access.
/// @param data Encoded reparse data.
/// @return Nothing; `NotPermitted` where a symbolic link needs a privilege the process lacks.
[[nodiscard]] CORE_FS_API FsResult<void> writeReparseData(HANDLE handle, std::span<std::byte const> data);

} // namespace core::fs::detail
