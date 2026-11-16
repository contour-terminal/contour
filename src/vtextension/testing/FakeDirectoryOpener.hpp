// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/DirectoryHandle.hpp>

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vtextension::testing
{

/// A filesystem of named directories: each path maps to the path it resolves to (itself, or a symlink's
/// target), and opening one hands out an unbacked handle carrying the resolved path. `/` always exists.
class FakeDirectoryOpener final: public vtpty::IDirectoryOpener
{
  public:
    /// The directories that exist: path to resolved path.
    std::map<std::string, std::string, std::less<>> directories;

    /// Paths open() and canonical() refuse with an error other than Missing.
    std::map<std::string, vtpty::DirectoryError, std::less<>> failures;

    /// Paths createBeneath() refuses to create, by their whole path.
    std::map<std::string, vtpty::DirectoryError, std::less<>> createFailures;

    /// Adds a directory that resolves to itself.
    void add(std::string const& path) { directories[path] = path; }

    /// Adds a symlink at @p path to the directory @p target.
    void link(std::string const& path, std::string const& target) { directories[path] = target; }

    [[nodiscard]] std::expected<vtpty::DirectoryHandle, vtpty::DirectoryError> open(
        std::string_view path) override
    {
        return canonical(path).transform(
            [](std::string resolved) { return vtpty::DirectoryHandle::unbacked(std::move(resolved)); });
    }

    [[nodiscard]] std::expected<std::string, vtpty::DirectoryError> canonical(
        std::string_view existingPath) override
    {
        if (auto const failure = failures.find(existingPath); failure != failures.end())
            return std::unexpected(failure->second);
        if (existingPath == "/")
            return std::string { "/" };
        auto const directory = directories.find(existingPath);
        if (directory == directories.end())
            return std::unexpected(vtpty::DirectoryError::Missing);
        return directory->second;
    }

    [[nodiscard]] std::expected<vtpty::DirectoryHandle, vtpty::DirectoryError> createBeneath(
        vtpty::DirectoryHandle const& parent, std::string_view name) override
    {
        auto const path =
            (parent.resolvedPath() == "/" ? "/" : parent.resolvedPath() + "/") + std::string(name);
        if (auto const failure = createFailures.find(path); failure != createFailures.end())
            return std::unexpected(failure->second);
        if (auto const existing = directories.find(path); existing != directories.end())
        {
            if (existing->second != path)
                return std::unexpected(vtpty::DirectoryError::NotADirectory); // A symlink is never followed.
            return vtpty::DirectoryHandle::unbacked(path);
        }
        created.push_back(path);
        directories.emplace(path, path);
        return vtpty::DirectoryHandle::unbacked(path);
    }

    /// Every directory createBeneath() created, by its whole path, in order.
    std::vector<std::string> created;
};

} // namespace vtextension::testing
