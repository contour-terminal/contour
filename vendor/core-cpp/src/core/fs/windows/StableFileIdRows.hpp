// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which Windows file systems give an object a file id that names it, and only it, for its life:
/// the table @c IDescriptorFileSystem::relation trusts identities from. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/detail/Ancestry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace core::fs::detail
{

/// Whether a file system's file ids can decide a relation. The refusing case is zero.
enum class FileIdStability : std::uint8_t
{
    Unstable, ///< Not to be trusted: ids that move with a directory entry (FAT, exFAT), are made up
              ///< per session (SMB without persistent ids), or a file system nobody has vouched for.
    Stable,   ///< An id names one object for its life (NTFS, ReFS).
};

/// One file system, by the name `GetVolumeInformationByHandleW` reports.
struct StableFileIdRow
{
    std::wstring_view fileSystemName; ///< As reported, compared exactly.
    FileIdStability stability = FileIdStability::Unstable;
};

/// @return The file systems whose ids are trusted; every name not here is @c FileIdStability::Unstable.
[[nodiscard]] CORE_FS_API std::span<StableFileIdRow const> stableFileIdRows() noexcept;

/// @param fileSystemName What `GetVolumeInformationByHandleW` reports.
/// @return Its row's stability; `Unstable` for a name no row has.
[[nodiscard]] CORE_FS_API FileIdStability fileIdStabilityOf(std::wstring_view fileSystemName) noexcept;

/// An object's identity, where it can decide a relation.
/// @param volumeSerial `FILE_ID_INFO::VolumeSerialNumber`.
/// @param fileId `FILE_ID_INFO::FileId`, 16 bytes, least significant first.
/// @param fileSystemName What the volume's file system is called.
/// @return The identity; `Unsupported` on a file system whose ids are not trusted, or for a zero
///         id, which names no object.
[[nodiscard]] CORE_FS_API FsResult<ObjectIdentity> stableIdentityOf(std::uint64_t volumeSerial,
                                                                    std::span<std::byte const, 16> fileId,
                                                                    std::wstring_view fileSystemName);

} // namespace core::fs::detail
