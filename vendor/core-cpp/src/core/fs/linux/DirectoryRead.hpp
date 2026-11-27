// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The records Linux's `getdents64` writes, read field by field within the bytes it returned, so a
/// record that claims more than is there is refused rather than read past. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/detail/DirectoryStream.hpp>

#include <cstddef>
#include <span>

namespace core::fs::detail
{

/// Reads the `dirent64` records of @p bytes onto @p batch, through @c entryOf.
/// @param directory The directory they were read from, which an entry of unknown type is stat'ed in.
/// @param bytes What one `getdents64` call returned.
/// @param batch Where the entries go.
/// @return Nothing; `Invalid` where a record is shorter than its header, its length (`d_reclen`) is
///         no longer than the header or runs past @p bytes, or its name has no NUL within it.
[[nodiscard]] CORE_FS_API FsResult<void> parseDirentRecords(int directory,
                                                            std::span<std::byte const> bytes,
                                                            DirectoryBatch& batch);

} // namespace core::fs::detail
