// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The file operations a replace needs to survive a power loss, which core::platform::FileSystem does not
/// offer: a rename alone can reach the disk before the data it names does, and then the next start finds an
/// empty or partial file where the last good snapshot was. Also the exclusive create it starts with, which
/// does not follow a link planted where the temporary file goes.

#include <expected>
#include <filesystem>
#include <string>

namespace vthost
{

/// The seam to the OS's durability calls, so the store's call order is testable without a disk.
class IDurableFiles
{
  public:
    IDurableFiles() = default;
    IDurableFiles(IDurableFiles const&) = delete;
    IDurableFiles(IDurableFiles&&) = delete;
    IDurableFiles& operator=(IDurableFiles const&) = delete;
    IDurableFiles& operator=(IDurableFiles&&) = delete;
    virtual ~IDurableFiles() = default;

    /// Creates @p path empty, failing when anything exists there, a symbolic link included.
    /// @param path The file to create.
    /// @return Nothing, or why the OS refused.
    [[nodiscard]] virtual std::expected<void, std::string> createExclusive(
        std::filesystem::path const& path) const = 0;

    /// Flushes what was written to @p path to stable storage.
    /// @param path The existing file.
    /// @return Nothing, or why the OS refused.
    [[nodiscard]] virtual std::expected<void, std::string> syncFile(
        std::filesystem::path const& path) const = 0;

    /// Renames @p from over @p to atomically, and on Windows durably too.
    /// @param from The synced file.
    /// @param to The file it replaces, if any.
    /// @return Nothing, or why the OS refused.
    [[nodiscard]] virtual std::expected<void, std::string> replace(std::filesystem::path const& from,
                                                                   std::filesystem::path const& to) const = 0;

    /// Flushes the entries of @p directory, so a rename within it survives a power loss; a no-op on Windows,
    /// whose replace() wrote through already.
    /// @param directory The existing directory.
    /// @return Nothing, or why the OS refused.
    [[nodiscard]] virtual std::expected<void, std::string> syncDirectory(
        std::filesystem::path const& directory) const = 0;
};

/// The OS's own: fsync (F_FULLFSYNC on macOS) and rename on POSIX; FlushFileBuffers and
/// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) on Windows.
class NativeDurableFiles final: public IDurableFiles
{
  public:
    /// @return The one stateless instance.
    [[nodiscard]] static NativeDurableFiles const& instance()
    {
        static auto const value = NativeDurableFiles {};
        return value;
    }

    [[nodiscard]] std::expected<void, std::string> createExclusive(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> syncFile(std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> replace(std::filesystem::path const& from,
                                                           std::filesystem::path const& to) const override;
    [[nodiscard]] std::expected<void, std::string> syncDirectory(
        std::filesystem::path const& directory) const override;
};

} // namespace vthost
