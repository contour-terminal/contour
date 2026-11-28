// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How @c IDescriptorFileSystem::relation decides: the identities of a directory and of its real
/// parents, compared. The walk that collects them is the operating system's
/// (`posix/Ancestry.cpp`, `windows/Ancestry.cpp`, one of them chosen by CMake), the decision is
/// shared with the in-memory model. Private to the fs module.

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/platform/Types.hpp>

#include <cstdint>
#include <span>

namespace core::fs::detail
{

struct NativeContext; // detail/NativeSupport.hpp

/// What tells one directory from every other for as long as it exists: its device and inode on
/// POSIX, its volume serial number and 128-bit file id on Windows.
struct ObjectIdentity
{
    std::uint64_t device = 0; ///< The device or volume.
    std::uint64_t idLow = 0;  ///< The inode, or the file id's low 64 bits.
    std::uint64_t idHigh = 0; ///< The file id's high 64 bits (Windows).

    [[nodiscard]] bool operator==(ObjectIdentity const&) const = default;
};

/// Decides how two directories relate from their ancestries.
/// @param first The first directory's identity, then each of its real parents' up to the root.
/// @param second The same for the second directory.
/// @return `Same` where their own identities match; `FirstContainsSecond` where the first's is
///         among the second's ancestors; the mirror case; otherwise `Unrelated`. Neither may be
///         empty.
[[nodiscard]] CORE_FS_API Relation decideRelation(std::span<ObjectIdentity const> first,
                                                  std::span<ObjectIdentity const> second) noexcept;

/// Walks the real parents of two directories of a native file system and decides how they relate;
/// defined by the walker CMake chose. Runs on the blocking executor.
/// @param context The file system's: its `maxAncestorDepth` bounds each walk.
/// @param first A directory handle of it.
/// @param second Another, or the same.
/// @return How they relate, or what @c IDescriptorFileSystem::relation names.
[[nodiscard]] CORE_FS_API FsResult<Relation> nativeRelation(NativeContext const& context,
                                                            platform::NativeHandle first,
                                                            platform::NativeHandle second);

} // namespace core::fs::detail
