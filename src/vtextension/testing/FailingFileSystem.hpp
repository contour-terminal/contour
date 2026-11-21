// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A test file system over an InMemoryFileSystem that fails one operation, named at construction, every time
/// it is called: a disk that is full, read-only or gone, for one step of a durable write.

#include <core/platform/FileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vtextension::testing
{

/// The operation a FailingFileSystem fails.
enum class FileOperation : uint8_t
{
    None,              ///< Nothing fails.
    CreateDirectories, ///< createDirectories().
    SetPermissions,    ///< setPermissions().
    CreateNewFile,     ///< createNewFile().
    WriteFile,         ///< writeFile().
    SyncFile,          ///< syncFile().
    ReplaceFile,       ///< replaceFile().
    SyncDirectory,     ///< syncDirectory().
    Remove,            ///< remove().
};

/// A FileSystem that passes every call through to an InMemoryFileSystem, but fails one operation.
///
/// A decorator rather than a subclass, because InMemoryFileSystem is final. Each operation forwards to the
/// same operation of the files it wraps, never through another of its own, so that only the named one fails.
class FailingFileSystem final: public core::platform::FileSystem
{
  public:
    /// @param files The files every call reaches; must outlive this.
    /// @param failing The operation that fails.
    FailingFileSystem(core::platform::testing::InMemoryFileSystem& files, FileOperation failing):
        _files { files }, _failing { failing }
    {
    }

    [[nodiscard]] bool exists(std::filesystem::path const& path) const override
    {
        return _files.exists(path);
    }
    [[nodiscard]] bool isDirectory(std::filesystem::path const& path) const override
    {
        return _files.isDirectory(path);
    }
    [[nodiscard]] bool isRegularFile(std::filesystem::path const& path) const override
    {
        return _files.isRegularFile(path);
    }
    [[nodiscard]] bool isSymlink(std::filesystem::path const& path) const override
    {
        return _files.isSymlink(path);
    }
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path) const override
    {
        return _files.isExecutableFile(path);
    }
    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override
    {
        return _files.weaklyCanonical(path);
    }
    [[nodiscard]] std::filesystem::path currentPath() const override { return _files.currentPath(); }
    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override
    {
        return _files.readFile(path);
    }
    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override
    {
        return failsWith(FileOperation::WriteFile).and_then([&] { return _files.writeFile(path, content); });
    }
    [[nodiscard]] std::expected<void, std::string> appendFile(std::filesystem::path const& path,
                                                              std::string_view content) const override
    {
        return _files.appendFile(path, content);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::istream>, std::string> openRead(
        std::filesystem::path const& path) const override
    {
        return _files.openRead(path);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        std::filesystem::path const& path, core::platform::WriteMode mode) const override
    {
        return _files.openWrite(path, mode);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::iostream>, std::string> openReadWrite(
        std::filesystem::path const& path) const override
    {
        return _files.openReadWrite(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectory(
        std::filesystem::path const& path) const override
    {
        return _files.createDirectory(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectories(
        std::filesystem::path const& path) const override
    {
        return failsWith(FileOperation::CreateDirectories).and_then([&] {
            return _files.createDirectories(path);
        });
    }
    [[nodiscard]] std::expected<bool, std::string> remove(std::filesystem::path const& path) const override
    {
        return failsWith(FileOperation::Remove).and_then([&] { return _files.remove(path); });
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const override
    {
        return _files.removeAll(path);
    }
    [[nodiscard]] std::expected<void, std::string> copyFile(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        core::platform::OverwritePolicy policy) const override
    {
        return _files.copyFile(from, to, policy);
    }
    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override
    {
        return _files.rename(from, to);
    }
    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const override
    {
        return _files.listDirectory(path);
    }
    [[nodiscard]] core::Generator<DirectoryEntry> walkDirectoryRecursive(
        std::filesystem::path path, std::error_code* outError) const override
    {
        return _files.walkDirectoryRecursive(std::move(path), outError);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const override
    {
        return _files.fileSize(path);
    }
    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override
    {
        return _files.lastWriteTime(path);
    }
    [[nodiscard]] std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const override
    {
        return _files.permissions(path);
    }
    [[nodiscard]] std::expected<void, std::string> setPermissions(std::filesystem::path const& path,
                                                                  std::filesystem::perms perms) const override
    {
        return failsWith(FileOperation::SetPermissions).and_then([&] {
            return _files.setPermissions(path, perms);
        });
    }
    [[nodiscard]] std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const override
    {
        return _files.createTempFile(prefix);
    }
    [[nodiscard]] std::expected<void, std::string> createNewFile(
        std::filesystem::path const& path) const override
    {
        return failsWith(FileOperation::CreateNewFile).and_then([&] { return _files.createNewFile(path); });
    }
    [[nodiscard]] std::expected<void, std::string> syncFile(std::filesystem::path const& path) const override
    {
        return failsWith(FileOperation::SyncFile).and_then([&] { return _files.syncFile(path); });
    }
    [[nodiscard]] std::expected<void, std::string> replaceFile(std::filesystem::path const& from,
                                                               std::filesystem::path const& to) const override
    {
        return failsWith(FileOperation::ReplaceFile).and_then([&] { return _files.replaceFile(from, to); });
    }
    [[nodiscard]] std::expected<void, std::string> syncDirectory(
        std::filesystem::path const& directory) const override
    {
        return failsWith(FileOperation::SyncDirectory).and_then([&] {
            return _files.syncDirectory(directory);
        });
    }

  private:
    /// @return An error if @p operation is the one that fails; nothing otherwise.
    [[nodiscard]] std::expected<void, std::string> failsWith(FileOperation operation) const
    {
        if (operation == _failing)
            return std::unexpected(std::make_error_code(std::errc::no_space_on_device).message());
        return {};
    }

    core::platform::testing::InMemoryFileSystem& _files;
    FileOperation _failing;
};

} // namespace vtextension::testing
