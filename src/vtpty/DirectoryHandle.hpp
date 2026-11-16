// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/OwnedFd.hpp>

#include <cstdint>
#include <expected>
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

/// An open directory: a child entered by it runs there even if the path is replaced afterwards.
class DirectoryHandle
{
  public:
    /// @param descriptor The open directory; -1 for none (see unbacked()).
    /// @param resolvedPath The directory's canonical path, as read from @p descriptor when it was opened.
    DirectoryHandle(OwnedFd descriptor, std::string resolvedPath) noexcept:
        _descriptor { std::move(descriptor) }, _resolvedPath { std::move(resolvedPath) }
    {
    }

    /// A handle that holds no descriptor, for test doubles that never enter it.
    /// @param resolvedPath The path it stands for.
    /// @return The handle; its descriptor() is -1.
    [[nodiscard]] static DirectoryHandle unbacked(std::string resolvedPath) noexcept
    {
        return DirectoryHandle { OwnedFd {}, std::move(resolvedPath) };
    }

    /// @return The open directory's descriptor; -1 when unbacked.
    [[nodiscard]] int descriptor() const noexcept { return _descriptor.get(); }

    /// @return The directory's canonical path, read from the descriptor when it was opened.
    [[nodiscard]] std::string const& resolvedPath() const noexcept { return _resolvedPath; }

  private:
    OwnedFd _descriptor;
    std::string _resolvedPath;
};

/// Opens directories by handle, resolves paths, and creates directories: the seam the filesystem is
/// reached through where a working directory is chosen.
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
    /// @return The open directory, its resolved path read from the descriptor; or why it could not be opened.
    [[nodiscard]] virtual std::expected<DirectoryHandle, DirectoryError> open(std::string_view path) = 0;

    /// @param existingPath A path that exists.
    /// @return Its canonical form, every symlink resolved; or why it could not be resolved.
    [[nodiscard]] virtual std::expected<std::string, DirectoryError> canonical(
        std::string_view existingPath) = 0;

    /// Creates the directory at @p path and every missing parent, as `mkdir -p` does.
    /// @return Nothing once the directory exists; `NotADirectory` if something else is in the way.
    [[nodiscard]] virtual std::expected<void, DirectoryError> ensureExists(std::string_view path) = 0;
};

/// @return The opener over this machine's filesystem. POSIX: a directory is opened
///         `O_RDONLY | O_DIRECTORY | O_CLOEXEC`, and its resolved path read from the descriptor
///         (`/proc/self/fd/N` on Linux, `fcntl(F_GETPATH)` on macOS, `realpath` of the opened name
///         elsewhere). Windows: every call answers `Unsupported`.
[[nodiscard]] IDirectoryOpener& nativeDirectoryOpener() noexcept;

} // namespace vtpty
