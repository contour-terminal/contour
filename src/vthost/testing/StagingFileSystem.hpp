// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The file system the daemon image store is tested on: an InMemoryFileSystem made to behave as the
/// operating system does in the two places the store depends on, and given one seam to race a
/// concurrent launcher through.

#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/testing/ForwardingFileSystem.hpp>

namespace vthost::testing
{

/// An InMemoryFileSystem whose rename() and lastWriteTime() answer as the OS's do.
///
/// - rename() refuses a destination that already exists, unless it is an empty directory, as
///   rename(2) and MoveFileEx do. InMemoryFileSystem merges a directory onto an existing one, which
///   would hide a publish that the OS rejects. It also keeps the permissions of what it moves, which
///   InMemoryFileSystem drops.
/// - lastWriteTime() reports FixedWriteTime for every path. InMemoryFileSystem reports the clock's
///   now(), so an executable's fingerprint, and with it the image's name, would change per call.
class StagingFileSystem final: public ForwardingFileSystem
{
  public:
    /// What lastWriteTime() reports for every path that exists.
    static constexpr auto FixedWriteTime = std::filesystem::file_time_type {};

    /// Runs at the start of every rename(), with its source and destination: another launcher,
    /// acting in the window before this rename.
    mutable std::function<void(std::filesystem::path const& from, std::filesystem::path const& to)>
        beforeRename;

    /// Runs at the start of every removeAll(), with its path.
    mutable std::function<void(std::filesystem::path const& path)> beforeRemoveAll;

    [[nodiscard]] std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const override
    {
        if (beforeRemoveAll)
            beforeRemoveAll(path);
        return files.removeAll(path);
    }

    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override
    {
        if (beforeRename)
            beforeRename(from, to);
        if (files.exists(to))
        {
            auto const children = files.listDirectory(to);
            if (!children || !children->empty())
                return std::unexpected(std::format("Cannot rename onto {}: it exists", to.generic_string()));
            std::ignore = files.remove(to);
        }
        // InMemoryFileSystem drops a renamed file's permissions; the OS moves them with it.
        auto moved = std::vector<std::pair<std::filesystem::path, std::filesystem::perms>> {};
        for (auto const& entry: files.walkDirectoryRecursive(from))
            if (auto const perms = files.permissions(entry.path))
                moved.emplace_back(to / entry.path.lexically_relative(from), *perms);
        auto renamed = files.rename(from, to);
        if (renamed)
            for (auto const& [path, perms]: moved)
                std::ignore = files.setPermissions(path, perms);
        return renamed;
    }

    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override
    {
        return files.lastWriteTime(path).transform([](auto) { return FixedWriteTime; });
    }
};

} // namespace vthost::testing
