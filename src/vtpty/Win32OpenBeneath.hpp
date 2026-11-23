// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Opening one entry beneath an open directory on Windows, relative to its handle rather than by a path, so
/// that what is opened is in the held directory whatever its name is now, and no link in the entry's place
/// is followed.

#ifdef _WIN32

    #include <vtpty/UniqueHandle.hpp>
    #include <vtpty/Win32Handle.hpp>

    #include <cstdint>
    #include <expected>
    #include <string_view>

    #include <Windows.h>

namespace vtpty
{

/// What a held directory is opened for: listing and entering it, reading its attributes, and waiting on it.
inline constexpr DWORD HeldDirectoryAccess =
    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

/// What openBeneath() opens. None is opened as FILE_NON_DIRECTORY_FILE: its STATUS_FILE_IS_A_DIRECTORY maps
/// to ERROR_ACCESS_DENIED, which could not be told from a real denial.
enum class BeneathEntry : uint8_t
{
    DirectoryCreatedIfMissing = 0, ///< A directory, created when nothing has the name.
    Directory = 1, ///< An existing directory, to walk through: FILE_OPEN, FILE_DIRECTORY_FILE.
    Entry = 2,     ///< Any existing entry, looked at: FILE_OPEN, as a directory or not, whichever it is.
    Last = Entry,  ///< The last enumerator: what the table of entries is checked against.
};

/// Opens one entry beneath an open directory, relative to its handle (NtCreateFile with a RootDirectory),
/// following no reparse point: a link in the name's place is opened itself, never its target.
/// @param parent An open directory. A null one is refused, before anything is opened: with no RootDirectory,
///               NtCreateFile would read @p name as an absolute object path.
/// @param name One component, UTF-16, already checked by isPlainWindowsName(). An empty one (which would
///             reopen @p parent) and one holding `\` or `/` (which would walk beneath it) are refused, before
///             anything is opened.
/// @param entry What is opened.
/// @param access The access the handle is opened with; SYNCHRONIZE is always added, as the synchronous I/O
///               the handle is opened for requires.
/// @return The handle, not inheritable; ERROR_ACCESS_DENIED for a refused @p parent or @p name; or the
///         Win32 error the NTSTATUS maps to (RtlNtStatusToDosError).
[[nodiscard]] std::expected<UniqueHandle, DWORD> openBeneath(HANDLE parent,
                                                             std::wstring_view name,
                                                             BeneathEntry entry,
                                                             DWORD access);

/// Re-opens the object @p held refers to by its handle, resolving no name: NtCreateFile relative to it with
/// an empty name, as ReOpenFile() does, so what is opened is what @p held looked at whatever its name is now.
/// Opened itself (FILE_OPEN_REPARSE_POINT), recalling no data from remote storage (FILE_OPEN_NO_RECALL, which
/// ReOpenFile() refuses as ERROR_INVALID_PARAMETER), sequentially, synchronously, shared for read, write and
/// delete, not inheritable.
/// @param held An open file. A null one is refused, before anything is opened.
/// @param access The access the new handle is opened with; SYNCHRONIZE is always added.
/// @return The new handle; ERROR_ACCESS_DENIED for a null @p held; or the Win32 error the NTSTATUS maps to.
[[nodiscard]] std::expected<UniqueHandle, DWORD> reopenHeld(HANDLE held, DWORD access);

/// @return Whether @p handle is a name-surrogate reparse point (junction, symlink, mount point):
///         GetFileInformationByHandleEx(FileAttributeTagInfo); an unanswerable query counts as one.
[[nodiscard]] bool isNameSurrogate(HANDLE handle) noexcept;

} // namespace vtpty

#endif
