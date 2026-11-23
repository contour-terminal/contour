// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one check every Windows spawn into a held directory makes: CreateProcessW takes a current directory by
/// path, never by handle, so the held directory's resolved path is checked to name it -- before the child is
/// created, and again while it is still suspended.

#ifdef _WIN32

    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/UniqueHandle.hpp>

    #include <cstdint>
    #include <expected>
    #include <string>

    #include <Windows.h>

namespace vtpty
{

/// Why a held directory cannot be a child's current directory.
enum class HeldDirectoryEntryError : uint8_t
{
    Unbacked, ///< The handle holds no directory (DirectoryHandle::unbacked).
    Removed,  ///< The directory was removed while held.
    Replaced, ///< Its resolved path names another directory now (or nothing).
    TooLong,  ///< Its resolved path is longer than MaxWindowsCurrentDirectoryUnits.
    NotUtf8,  ///< Its resolved path is not UTF-8.
};

/// A held directory made ready to be passed to CreateProcessW as lpCurrentDirectory.
struct HeldDirectoryEntry
{
    std::wstring path;     ///< resolvedPath(), UTF-16: what CreateProcessW is given.
    FILE_ID_INFO identity; ///< The held handle's volume serial and file id.
};

/// Prepares @p held to be a child's current directory: its path, checked to name the held directory now.
/// CreateProcessW takes a path, not a handle: call confirmHeldDirectoryEntry() again once the child is
/// created suspended. Residual: the name swapped away and back within the CreateProcessW call is not seen.
///
/// A held directory renamed since it was opened is Replaced, not entered at its new name: the path checked is
/// the one read when it was opened. A junction put at its name that leads back to it is the same file id, so
/// it is entered -- the child's current directory is still the held directory.
/// @param held The directory, held open.
/// @return What to pass to CreateProcessW; or why it cannot be.
[[nodiscard]] std::expected<HeldDirectoryEntry, HeldDirectoryEntryError> prepareHeldDirectoryEntry(
    DirectoryHandle const& held);

/// Opens @p entry's path (FILE_READ_ATTRIBUTES, backup semantics, shared for read, write and delete, not
/// inheritable; closed before it returns) and compares the directory there with the one held.
/// @param entry What prepareHeldDirectoryEntry() made.
/// @return Nothing when entry.path still names the directory of entry.identity; Replaced otherwise.
[[nodiscard]] std::expected<void, HeldDirectoryEntryError> confirmHeldDirectoryEntry(
    HeldDirectoryEntry const& entry);

} // namespace vtpty

#endif
