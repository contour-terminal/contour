// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The records a Windows directory stream reads: `FILE_ID_BOTH_DIR_INFO`, as
/// `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` writes them into one fixed buffer per
/// batch, read field by field at their documented offsets and held to the buffer's bounds. Private
/// to the fs module.

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/windows/NtFile.hpp>

#include <span>
#include <vector>

namespace core::fs::detail
{

/// Reads the chain of records @p records holds, from its first, skipping `.` and `..`.
/// @param records What one `FileIdBothDirectoryInfo` call wrote, at the start of its buffer.
/// @return The entries, in order: a name surrogate (a symbolic link, a junction, a mount point) is
///         a link, anything else the directory or the file it is; a name UTF-8 cannot spell (an
///         unpaired surrogate) is `IllegalByteSequence` for that entry, and an empty name or one
///         holding `\` or `/` is `Io` for that entry. `Invalid` where a record
///         or its name does not lie within @p records, or the chain does not move forward.
[[nodiscard]] CORE_FS_API FsResult<std::vector<FsResult<DirectoryEntry>>> parseDirectoryRecords(
    std::span<std::byte const> records);

} // namespace core::fs::detail
