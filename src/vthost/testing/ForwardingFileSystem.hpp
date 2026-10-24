// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The base of the test file systems that change one behaviour of InMemoryFileSystem: it forwards
/// every call, so a decorator overrides only the call it is about.

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

namespace vthost::testing
{

/// A FileSystem that passes every call through to the InMemoryFileSystem it holds.
///
/// A decorator rather than a subclass, because InMemoryFileSystem is final.
class ForwardingFileSystem: public core::platform::FileSystem
{
  public:
    /// The files, as an InMemoryFileSystem holds them.
    core::platform::testing::InMemoryFileSystem files;

    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override
    {
        return files.readFile(path);
    }
    [[nodiscard]] bool exists(std::filesystem::path const& path) const override { return files.exists(path); }
    [[nodiscard]] bool isDirectory(std::filesystem::path const& path) const override
    {
        return files.isDirectory(path);
    }
    [[nodiscard]] bool isRegularFile(std::filesystem::path const& path) const override
    {
        return files.isRegularFile(path);
    }
    [[nodiscard]] bool isSymlink(std::filesystem::path const& path) const override
    {
        return files.isSymlink(path);
    }
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path) const override
    {
        return files.isExecutableFile(path);
    }
    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override
    {
        return files.weaklyCanonical(path);
    }
    [[nodiscard]] std::filesystem::path currentPath() const override { return files.currentPath(); }
    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override
    {
        return files.writeFile(path, content);
    }
    [[nodiscard]] std::expected<void, std::string> appendFile(std::filesystem::path const& path,
                                                              std::string_view content) const override
    {
        return files.appendFile(path, content);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::istream>, std::string> openRead(
        std::filesystem::path const& path) const override
    {
        return files.openRead(path);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        std::filesystem::path const& path, core::platform::WriteMode mode) const override
    {
        return files.openWrite(path, mode);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::iostream>, std::string> openReadWrite(
        std::filesystem::path const& path) const override
    {
        return files.openReadWrite(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectory(
        std::filesystem::path const& path) const override
    {
        return files.createDirectory(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectories(
        std::filesystem::path const& path) const override
    {
        return files.createDirectories(path);
    }
    [[nodiscard]] std::expected<bool, std::string> remove(std::filesystem::path const& path) const override
    {
        return files.remove(path);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const override
    {
        return files.removeAll(path);
    }
    [[nodiscard]] std::expected<void, std::string> copyFile(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        core::platform::OverwritePolicy policy) const override
    {
        return files.copyFile(from, to, policy);
    }
    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override
    {
        return files.rename(from, to);
    }
    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const override
    {
        return files.listDirectory(path);
    }
    [[nodiscard]] core::Generator<DirectoryEntry> walkDirectoryRecursive(
        std::filesystem::path path, std::error_code* outError) const override
    {
        return files.walkDirectoryRecursive(std::move(path), outError);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const override
    {
        return files.fileSize(path);
    }
    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override
    {
        return files.lastWriteTime(path);
    }
    [[nodiscard]] std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const override
    {
        return files.permissions(path);
    }
    [[nodiscard]] std::expected<void, std::string> setPermissions(std::filesystem::path const& path,
                                                                  std::filesystem::perms perms) const override
    {
        return files.setPermissions(path, perms);
    }
    [[nodiscard]] std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const override
    {
        return files.createTempFile(prefix);
    }
};

} // namespace vthost::testing
