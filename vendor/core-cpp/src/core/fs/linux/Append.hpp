// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Linux's append where `RWF_APPEND` is refused. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>

#include <cstddef>
#include <mutex>
#include <span>

namespace core::fs::detail
{

/// Writes @p bytes at the end of the file, found with `fstat`, through the descriptor's own
/// handle: no new open, so the handle's write integrity (`O_SYNC`, `O_DSYNC`) and the access it
/// was opened with hold, whatever the file's mode now says. @p appendLock serialises this
/// descriptor's appends, so two of them never find the same end. Any other writer can still land
/// between the `fstat` and the write: another process, and another descriptor on the same file in
/// this one, which holds a lock of its own. An `O_APPEND` handle would rule that out.
/// @param handle The descriptor's handle, open for writing.
/// @param appendLock The descriptor's append lock.
/// @param bytes What to write.
/// @return How many bytes were written.
[[nodiscard]] CORE_FS_API FsResult<std::size_t> appendAtEnd(int handle,
                                                            std::mutex& appendLock,
                                                            std::span<std::byte const> bytes);

} // namespace core::fs::detail
