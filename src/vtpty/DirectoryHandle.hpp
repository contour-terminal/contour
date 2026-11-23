// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32
    #include <vtpty/UniqueHandle.hpp>

    #include <core/platform/Types.hpp>
#else
    #include <vtpty/OwnedFd.hpp>
#endif

#include <vtpty/PathStyle.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// A directory held open, so that a child entered by it runs there even if its name is replaced in the
// meantime -- the guard against a working directory swapped for a symlink between the check and the spawn.
namespace vtpty
{

/// Why a directory could not be opened, resolved or created.
enum class DirectoryError : uint8_t
{
    Missing,       ///< Nothing exists at the path (or a component of it is a dangling or looping link).
    NotADirectory, ///< Something exists there, but it is not a directory.
    AccessDenied,  ///< The directory exists but may not be opened, or the system refused for another reason.
    Unsupported,   ///< This platform has no directory handles.
};

#ifdef _WIN32
using OwnedDirectory = UniqueHandle; ///< What a DirectoryHandle owns here: a directory HANDLE.
#else
using OwnedDirectory = OwnedFd; ///< What a DirectoryHandle owns here: a directory descriptor.
#endif

/// An open directory: a child entered by it runs there even if the path is replaced afterwards.
class DirectoryHandle
{
  public:
    /// @param directory The open directory; a default-constructed one for none (see unbacked()).
    /// @param resolvedPath The directory's canonical path, as read from @p directory when it was opened.
    DirectoryHandle(OwnedDirectory directory, std::string resolvedPath) noexcept:
        _directory { std::move(directory) }, _resolvedPath { std::move(resolvedPath) }
    {
    }

    /// A handle that holds no open directory, for test doubles that never enter it.
    /// @param resolvedPath The path it stands for.
    /// @return The handle; its descriptor() is -1 (POSIX), its nativeHandle() null (Windows).
    [[nodiscard]] static DirectoryHandle unbacked(std::string resolvedPath) noexcept
    {
        return DirectoryHandle { OwnedDirectory {}, std::move(resolvedPath) };
    }

#ifndef _WIN32
    /// @return The open directory's descriptor; -1 when unbacked.
    [[nodiscard]] int descriptor() const noexcept { return _directory.get(); }
#else
    /// @return The open directory's HANDLE, still owned; null when unbacked. Opened with
    ///         HeldDirectoryAccess, shared read/write/delete, not inheritable.
    [[nodiscard]] core::platform::NativeHandle nativeHandle() const noexcept { return _directory.get(); }
#endif

    /// @return The directory's canonical path, read from the open directory when it was opened.
    [[nodiscard]] std::string const& resolvedPath() const noexcept { return _resolvedPath; }

    /// @return Whether the open directory has been removed since -- no name links to it any more, whatever
    ///         stands at its path now (Windows: its deletion is pending, or no link is left); false for an
    ///         unbacked handle.
    [[nodiscard]] bool isRemoved() const noexcept;

  private:
    OwnedDirectory _directory;
    std::string _resolvedPath;
};

/// Opens directories by handle, resolves paths, and creates directories one component at a time beneath a
/// handle: the seam the filesystem is reached through where a working directory is chosen.
class IDirectoryOpener
{
  public:
    IDirectoryOpener() = default;
    IDirectoryOpener(IDirectoryOpener const&) = delete;
    IDirectoryOpener(IDirectoryOpener&&) = delete;
    IDirectoryOpener& operator=(IDirectoryOpener const&) = delete;
    IDirectoryOpener& operator=(IDirectoryOpener&&) = delete;
    virtual ~IDirectoryOpener() = default;

    /// Opens the directory at @p path, following symlinks.
    /// @return The open directory, its resolved path read from the open directory; or why it could not be
    ///         opened.
    [[nodiscard]] virtual std::expected<DirectoryHandle, DirectoryError> open(std::string_view path) = 0;

    /// @param existingPath A path that exists.
    /// @return Its canonical form, every symlink resolved; or why it could not be resolved.
    [[nodiscard]] virtual std::expected<std::string, DirectoryError> canonical(
        std::string_view existingPath) = 0;

    /// Creates one directory beneath an open one, following no link: a name that is already a directory is
    /// opened, a symlink or anything else in its place is refused.
    /// @param parent The open directory to create in.
    /// @param name One path component: not empty, not `.` or `..`, without `/`.
    /// @return The new (or existing) directory, opened; `NotADirectory` if a symlink or a non-directory is in
    ///         the way.
    [[nodiscard]] virtual std::expected<DirectoryHandle, DirectoryError> createBeneath(
        DirectoryHandle const& parent, std::string_view name) = 0;

    /// @return The grammar of the paths this opener takes and resolves.
    [[nodiscard]] virtual PathStyle pathStyle() const noexcept = 0;
};

/// Where an opened directory's path is read from.
enum class DescriptorPaths : uint8_t
{
    /// Nowhere: the name's canonical form is opened again one component at a time from `/`, following no
    /// link, so the path given is the descriptor's however the name changes meanwhile.
    Unavailable = 0,
    /// The kernel, from the descriptor (`/proc/self/fd/N` on Linux, `fcntl(F_GETPATH)` on macOS), where it
    /// can tell; Unavailable's walk where it cannot.
    FromKernel,
};

/// @param paths Where an opened directory's path is read from; ignored on Windows, where it is always read
///              from the handle.
/// @return An opener over this machine's filesystem.
[[nodiscard]] std::unique_ptr<IDirectoryOpener> makeDirectoryOpener(DescriptorPaths paths);

/// @return The opener over this machine's filesystem. POSIX: a directory is opened
///         `O_RDONLY | O_DIRECTORY | O_CLOEXEC`, and its resolved path read from the descriptor
///         (DescriptorPaths::FromKernel). Windows: a directory is opened with backup semantics, shared for
///         read, write and delete so that holding it stops no rename or removal, not inheritable; its
///         resolved path is read from the handle (GetFinalPathNameByHandleW), in DOS form without the `\\?\`
///         prefix; createBeneath() opens the child relative to the parent's handle, refusing a junction,
///         symlink or mount point in its place and any name Win32 spells differently (isPlainWindowsName()).
///         A directory deleted by the time it is opened is `Missing`.
[[nodiscard]] IDirectoryOpener& nativeDirectoryOpener() noexcept;

} // namespace vtpty
