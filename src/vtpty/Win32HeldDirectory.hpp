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
    Unbacked = 0,       ///< The handle holds no directory (DirectoryHandle::unbacked).
    Removed = 1,        ///< The directory was removed while held.
    Replaced = 2,       ///< Its resolved path names another directory now (or nothing).
    TooLong = 3,        ///< Its resolved path is longer than MaxWindowsCurrentDirectoryUnits.
    NotUtf8 = 4,        ///< Its resolved path is not UTF-8.
    Unguardable = 5,    ///< Another handle open on it may delete or rename it, so it cannot be guarded.
    Last = Unguardable, ///< The last enumerator: what tables of it are checked against.
};

/// A held directory made ready to be passed to CreateProcessW as lpCurrentDirectory, and guarded until the
/// child it is passed to has ended.
///
/// CreateProcessW records the current directory as a path, and a child created suspended opens it only once
/// it is resumed (its loader does): a check of the path, before creation or after, cannot keep another
/// directory from being entered under the name. The guard can. It is the directory itself at that path,
/// opened (not inheritable) for listing it -- a data access, without which NTFS lets the name go -- and
/// shared for reading and writing but not for deleting, so that neither a rename nor a removal of it succeeds
/// while it is open. Its ancestors cannot be renamed either while anything beneath them is open. Windows does
/// the same for a running process's own current directory, so holding the guard for the child's whole life
/// costs nothing a user would notice; whoever passes the entry to CreateProcessW keeps it with the child and
/// releases it once the child has been reaped (PipedProcessRunner does; Process::start must too).
///
/// Residual: the path's drive letter, a DOS device name, may be redefined (DefineDosDevice, `subst`) in the
/// same logon session between the check and the child's loader; and on a remote file system the share modes
/// are the server's to enforce.
struct HeldDirectoryEntry
{
    std::wstring path;     ///< resolvedPath(), UTF-16: what CreateProcessW is given.
    FILE_ID_INFO identity; ///< The held handle's volume serial and file id.
    UniqueHandle guard;    ///< The directory at path, open without FILE_SHARE_DELETE: no one may rename it.
};

/// Prepares @p held to be a child's current directory: its path, checked to name the held directory now, and
/// guarded so that it goes on naming it while the entry lives (see HeldDirectoryEntry).
///
/// A held directory renamed since it was opened is Replaced, not entered at its new name: the path checked is
/// the one read when it was opened. So is a junction, or any reparse point, put at its name, even one that
/// leads back to it: the name itself is opened, and a junction may be retargeted once it was checked.
/// @param held The directory, held open.
/// @return What to pass to CreateProcessW, and to keep while the child runs; or why it cannot be.
///         Unguardable where another handle open on the directory may delete it (it holds DELETE access, or
///         shares no writing).
[[nodiscard]] std::expected<HeldDirectoryEntry, HeldDirectoryEntryError> prepareHeldDirectoryEntry(
    DirectoryHandle const& held);

/// A second look, cheap and redundant while the guard is held: opens @p entry's path itself (no reparse point
/// followed; FILE_READ_ATTRIBUTES, backup semantics, shared for read, write and delete, not inheritable;
/// closed before it returns) and compares the directory there with the one held.
/// @param entry What prepareHeldDirectoryEntry() made.
/// @return Nothing when entry.path still names the directory of entry.identity; Replaced otherwise.
[[nodiscard]] std::expected<void, HeldDirectoryEntryError> confirmHeldDirectoryEntry(
    HeldDirectoryEntry const& entry);

} // namespace vtpty

#endif
