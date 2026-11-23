// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Win32 calls a read beneath a held directory makes on Windows, behind a seam a test injects through.

#ifdef _WIN32

    #include <vtpty/BeneathRead.hpp>
    #include <vtpty/UniqueHandle.hpp>
    #include <vtpty/Win32OpenBeneath.hpp>

    #include <cstddef>
    #include <expected>
    #include <memory>
    #include <span>
    #include <string_view>

    #include <Windows.h>

namespace vtpty
{

/// The Win32 calls a read beneath a directory makes on Windows: the seam a test injects an error, an
/// identity, a size or attributes through. The Windows counterpart of IBeneathSystem, which speaks POSIX
/// descriptors.
class IWin32BeneathSystem
{
  public:
    IWin32BeneathSystem() = default;
    IWin32BeneathSystem(IWin32BeneathSystem const&) = delete;
    IWin32BeneathSystem(IWin32BeneathSystem&&) = delete;
    IWin32BeneathSystem& operator=(IWin32BeneathSystem const&) = delete;
    IWin32BeneathSystem& operator=(IWin32BeneathSystem&&) = delete;
    virtual ~IWin32BeneathSystem() = default;

    /// Opens one entry beneath @p parent through openBeneath(), following no reparse point.
    /// @param parent An open directory.
    /// @param name One component, UTF-16, already checked by isPlainWindowsName().
    /// @param entry BeneathEntry::Directory (FILE_TRAVERSE | FILE_READ_ATTRIBUTES) or BeneathEntry::Entry
    ///              (FILE_READ_ATTRIBUTES): the access is the entry's, from one table.
    /// @return The handle, not inheritable; or the Win32 error.
    [[nodiscard]] virtual std::expected<UniqueHandle, DWORD> open(HANDLE parent,
                                                                  std::wstring_view name,
                                                                  BeneathEntry entry) = 0;

    /// Re-opens the object @p looked holds for reading its data (reopenHeld(), as ReOpenFile() does): no name
    /// is resolved, so what is read is what was looked at. Opened shared for read, write and delete,
    /// following no reparse point and recalling no data from remote storage. A directory is never re-opened.
    /// @param looked A handle open() returned.
    /// @return A synchronous handle with FILE_READ_DATA and FILE_READ_ATTRIBUTES (so that status() can
    ///         describe it again), not inheritable; or the Win32 error (ERROR_ACCESS_DENIED for a directory).
    [[nodiscard]] virtual std::expected<UniqueHandle, DWORD> reopenForRead(HANDLE looked) = 0;

    /// @param handle An open handle with FILE_READ_ATTRIBUTES.
    /// @return What @p handle is: kind (windowsFileKindOf()), identity (FILE_ID_INFO, else
    ///         GetFileInformationByHandle where the volume has no 128-bit ids), links (0 when
    ///         delete-pending), size (EndOfFile), modification time; or the Win32 error of the first query
    ///         that failed.
    [[nodiscard]] virtual std::expected<FileStatus, DWORD> status(HANDLE handle) = 0;

    /// Reads up to @p into.size() bytes at the handle's position.
    /// @param handle A handle reopenForRead() returned.
    /// @param into Where the bytes go.
    /// @return How many were read, 0 at the end; or the Win32 error.
    [[nodiscard]] virtual std::expected<std::size_t, DWORD> read(HANDLE handle, std::span<char> into) = 0;
};

/// @return This machine's calls.
[[nodiscard]] IWin32BeneathSystem& nativeWin32BeneathSystem() noexcept;

/// Each component is opened beneath the handle above it through @p system, as itself: a junction, symlink or
/// mount point at any step is `Escapes`, as is a directory on another volume than the root's. A directory
/// one of the fenced identities is `Fenced`, however its name is spelled (another case, an 8.3 short name).
/// The last component is looked at (BeneathEntry::Entry) and, only when it is a regular file whose data is
/// on disk, re-opened by its handle for reading; the re-opened handle's description is the authority: a
/// regular file with one name on the root's volume. A zero file id is `Unreadable` at every step (some SMB
/// servers report one for every file, and it would match every fence or none); so is an unbacked root.
/// @param system The Win32 calls the reader makes; outlives the reader.
/// @return A reader over this machine's filesystem through @p system, with IBeneathReader's contract on
///         Windows.
[[nodiscard]] std::unique_ptr<IBeneathReader> makeWin32BeneathReader(IWin32BeneathSystem& system);

} // namespace vtpty

#endif
