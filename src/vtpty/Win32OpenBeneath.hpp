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

/// What openBeneath() opens. Phase C adds the entries a read needs.
enum class BeneathEntry : uint8_t
{
    DirectoryCreatedIfMissing = 0,    ///< A directory, created when nothing has the name.
    Last = DirectoryCreatedIfMissing, ///< The last enumerator: what the table of entries is checked against.
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

/// @return Whether @p handle is a name-surrogate reparse point (junction, symlink, mount point):
///         GetFileInformationByHandleEx(FileAttributeTagInfo); an unanswerable query counts as one.
[[nodiscard]] bool isNameSurrogate(HANDLE handle) noexcept;

} // namespace vtpty

#endif
