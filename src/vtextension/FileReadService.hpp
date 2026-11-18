// SPDX-License-Identifier: Apache-2.0
//
// The policy between a daemon-world extension and the files it may read: only beneath a root its manifest
// declares `read`, granted by the user, by a relative path that names one entry per component, read beneath
// the root's directory held open, following no link, and never blocking.
#pragma once

#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// The most one `file-read` returns: a larger file is `too-large`.
inline constexpr std::size_t MaxFileReadBytes = std::size_t { 64 } * 1024;

/// What `file-read` is served by.
class IFileReadService
{
  public:
    IFileReadService() = default;
    IFileReadService(IFileReadService const&) = delete;
    IFileReadService(IFileReadService&&) = delete;
    IFileReadService& operator=(IFileReadService const&) = delete;
    IFileReadService& operator=(IFileReadService&&) = delete;
    virtual ~IFileReadService() = default;

    /// `file-read`: reads a regular file inside a root declared readable.
    /// @param root The label of the root.
    /// @param path The file, relative to the root's directory.
    /// @return Its bytes and modification time; or why it was not read.
    [[nodiscard]] virtual std::expected<FileContent, FileError> read(std::string_view root,
                                                                     std::string_view path) = 0;
};

/// One extension's readable roots.
///
/// read() refuses, in this order: a root not declared `read` (`not-granted`); a
/// path that is empty, absolute, or holds an empty, `.` or `..` component or a NUL (`bad-argument`); a path
/// whose most specific root is another one (`not-granted`). It then reads beneath the root's directory,
/// opened on its first read and held from then on (a mount placed over the path later does not reach the
/// handle): one that does not exist is `directory-gone`, and opened again on a later read, as is one removed
/// while held; one that resolves elsewhere is `escapes`. The file is read beneath that handle
/// (vtpty::IBeneathReader): a directory below the root that does not exist is `directory-gone`, a missing
/// file `not-regular`, a link, a second name or another device `escapes`, and a directory that is another
/// root's -- by identity, however the path spells it -- `not-granted`. A root of several directories is read
/// in declaration order: the first in which the path's directories exist answers. The user's grant is not
/// asked here: the service is made only for an extension the user granted (DaemonExtensionRuntime), and goes
/// with it when that grant does.
class FileReadService final: public IFileReadService
{
  public:
    /// @param manifest The extension's manifest: which roots it declares `read`.
    /// @param roots Its roots, resolved.
    /// @param directories Opens each root's directory; outlives the service.
    /// @param reader Reads beneath it; outlives the service.
    FileReadService(Manifest const& manifest,
                    std::vector<ResolvedRoot> roots,
                    vtpty::IDirectoryOpener& directories,
                    vtpty::IBeneathReader& reader);

    [[nodiscard]] std::expected<FileContent, FileError> read(std::string_view root,
                                                             std::string_view path) override;

  private:
    /// Another root's directory beneath a root's directory.
    struct NestedRoot
    {
        std::vector<std::string> components;         ///< Its path below the root's directory.
        std::optional<vtpty::FileIdentity> identity; ///< Which directory it is, once found from the handle.
    };

    /// A root's directory, opened once and held: a mount placed over its path later does not reach it.
    struct OpenedDirectory
    {
        std::expected<vtpty::DirectoryHandle, FileError> handle { std::unexpect, FileError::DirectoryGone };
        std::vector<NestedRoot> nested; ///< The other roots' directories beneath it, fenced off.
    };

    /// Opens the directory @p directory of the root @p root, judged by the path read back from it, and names
    /// the other roots' directories beneath it.
    [[nodiscard]] OpenedDirectory openDirectory(std::string_view root, std::string_view directory) const;

    /// @return The identities of @p opened's nested roots, each found from its handle once and then kept.
    [[nodiscard]] std::vector<vtpty::FileIdentity> fencesOf(OpenedDirectory& opened) const;

    /// Reads @p components beneath the directory @p directory of the root @p root, held open from the first
    /// read on; opened anew only when it did not exist, or was removed since.
    [[nodiscard]] std::expected<FileContent, FileError> readIn(std::string_view root,
                                                               std::string_view directory,
                                                               std::span<std::string_view const> components);

    /// Reads @p components beneath @p opened, entering none of its nested roots.
    [[nodiscard]] std::expected<FileContent, FileError> readBeneath(
        OpenedDirectory& opened, std::span<std::string_view const> components) const;

    std::vector<std::string> _readableRoots;
    std::vector<ResolvedRoot> _roots;
    vtpty::IDirectoryOpener& _directories;
    vtpty::IBeneathReader& _reader;
    std::mutex _mutex;                                           ///< Guards @ref _opened.
    std::map<std::string, OpenedDirectory, std::less<>> _opened; ///< Each root directory read so far.
};

} // namespace vtextension
