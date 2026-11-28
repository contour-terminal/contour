// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a POSIX directory stream reads: from the directory opened again for the stream alone, so its
/// position is no one else's, one batch per call. Linux reads with `getdents64` into a fixed buffer
/// (`linux/DirectoryRead.cpp`); every other POSIX system through `fdopendir` and `readdir`
/// (`posix/DirectoryRead.cpp`), whose entry buffer is the stream's own and read under its lock.
/// Private to the fs module.

#include <core/async/IExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/detail/DirectoryStream.hpp>

#include <sys/stat.h>

#include <memory>
#include <optional>

namespace core::fs::detail
{

/// A directory opened for one stream, and what it reads into; defined by the system's source.
struct DirectoryReader;

/// Opens @p directory again for a stream of its own.
/// @param directory An open directory.
/// @param closer Where the reader's handle is closed once its last owner lets go: the blocking
///        executor. Must outlive the reader.
/// @return The reader; what refused it.
[[nodiscard]] FsResult<std::shared_ptr<DirectoryReader>> openDirectoryReader(int directory,
                                                                             async::IExecutor& closer);

/// Reads the next batch of @p reader's entries, where its last batch ended. Blocks.
/// @param reader The reader.
/// @return The batch; what refused it.
[[nodiscard]] FsResult<DirectoryBatch> readDirectoryBatch(DirectoryReader& reader);

/// @param mode A `st_mode`.
/// @return What it says the object is.
[[nodiscard]] DescriptorType typeOfMode(::mode_t mode) noexcept;

/// @param directory The directory the entry is in.
/// @param name The entry's name, NUL-terminated, as the system spells it.
/// @param entryType Its `d_type`; where that is `DT_UNKNOWN`, the name is stat'ed without following.
/// @return The entry; nullopt for `.` and `..`; `IllegalByteSequence` for a name that is not UTF-8;
///         `Io` for an empty name or one holding a `/`, which no file system should report.
[[nodiscard]] CORE_FS_API std::optional<FsResult<DirectoryEntry>> entryOf(int directory,
                                                                          char const* name,
                                                                          unsigned char entryType);

} // namespace core::fs::detail
