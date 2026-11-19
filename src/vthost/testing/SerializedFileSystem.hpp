// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An in-memory file system that more than one thread may call: what a test reads an extension's store
/// through while the extension's lane writes it.

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <vthost/testing/ForwardingFileSystem.hpp>

namespace vthost::testing
{

/// A ForwardingFileSystem whose every call holds one lock. Recursive, because the base forwards a create or a
/// replace through this file system's own writeFile() and rename(). What it cannot cover is said where: a
/// stream it hands out, and a directory walk, are read and written outside the lock.
class SerializedFileSystem final: public ForwardingFileSystem
{
  public:
    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::readFile(path);
    }
    [[nodiscard]] bool exists(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::exists(path);
    }
    [[nodiscard]] bool isDirectory(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::isDirectory(path);
    }
    [[nodiscard]] bool isRegularFile(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::isRegularFile(path);
    }
    [[nodiscard]] bool isSymlink(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::isSymlink(path);
    }
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::isExecutableFile(path);
    }
    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::weaklyCanonical(path);
    }
    [[nodiscard]] std::filesystem::path currentPath() const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::currentPath();
    }
    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::writeFile(path, content);
    }
    [[nodiscard]] std::expected<void, std::string> appendFile(std::filesystem::path const& path,
                                                              std::string_view content) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::appendFile(path, content);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::istream>, std::string> openRead(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::openRead(path);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        std::filesystem::path const& path, core::platform::WriteMode mode) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::openWrite(path, mode);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::iostream>, std::string> openReadWrite(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::openReadWrite(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectory(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::createDirectory(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectories(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::createDirectories(path);
    }
    [[nodiscard]] std::expected<bool, std::string> remove(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::remove(path);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::removeAll(path);
    }
    [[nodiscard]] std::expected<void, std::string> copyFile(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        core::platform::OverwritePolicy policy) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::copyFile(from, to, policy);
    }
    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::rename(from, to);
    }
    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::listDirectory(path);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::fileSize(path);
    }
    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::lastWriteTime(path);
    }
    [[nodiscard]] std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::permissions(path);
    }
    [[nodiscard]] std::expected<void, std::string> setPermissions(std::filesystem::path const& path,
                                                                  std::filesystem::perms perms) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::setPermissions(path, perms);
    }
    [[nodiscard]] std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::createTempFile(prefix);
    }
    [[nodiscard]] std::expected<void, std::string> createNewFile(
        std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::createNewFile(path);
    }
    [[nodiscard]] std::expected<void, std::string> syncFile(std::filesystem::path const& path) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::syncFile(path);
    }
    [[nodiscard]] std::expected<void, std::string> replaceFile(std::filesystem::path const& from,
                                                               std::filesystem::path const& to) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::replaceFile(from, to);
    }
    [[nodiscard]] std::expected<void, std::string> syncDirectory(
        std::filesystem::path const& directory) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return ForwardingFileSystem::syncDirectory(directory);
    }

  private:
    mutable std::recursive_mutex _mutex;
};

} // namespace vthost::testing
