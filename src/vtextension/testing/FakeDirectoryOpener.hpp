// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/DirectoryHandle.hpp>

#include <algorithm>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/RootPathSyntax.hpp>

namespace vtextension::testing
{

/// A filesystem of named directories: each path maps to the path it resolves to (itself, or a symlink's
/// target), and opening one hands out an unbacked handle carrying the resolved path. `/` always exists.
///
/// In vtpty::PathStyle::Windows, paths are looked up by comparisonKeyOf(), so `c:\WT` finds `C:\wt` and
/// answers the stored spelling, as `GetFinalPathNameByHandleW` answers the on-disk case; every volume root
/// (`X:\`, `\\srv\share`) exists; and createBeneath() refuses a name isPlainComponent() refuses with
/// AccessDenied.
class FakeDirectoryOpener final: public vtpty::IDirectoryOpener
{
  public:
    /// A fake that speaks the POSIX path grammar.
    FakeDirectoryOpener() = default;

    /// @param style The path grammar the fake reports it speaks.
    explicit FakeDirectoryOpener(vtpty::PathStyle style) noexcept: _style { style } {}

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
        opened.emplace_back(path);
        if (onOpen)
            onOpen(path);
        return canonical(path).transform(
            [](std::string resolved) { return vtpty::DirectoryHandle::unbacked(std::move(resolved)); });
    }

    [[nodiscard]] std::expected<std::string, vtpty::DirectoryError> canonical(
        std::string_view existingPath) override
    {
        if (auto const failure = lookup(failures, existingPath); failure != failures.end())
            return std::unexpected(failure->second);
        if (isVolumeRoot(existingPath, _style))
            return std::string { existingPath };
        auto const directory = lookup(directories, existingPath);
        if (directory == directories.end())
            return std::unexpected(vtpty::DirectoryError::Missing);
        return directory->second;
    }

    [[nodiscard]] std::expected<vtpty::DirectoryHandle, vtpty::DirectoryError> createBeneath(
        vtpty::DirectoryHandle const& parent, std::string_view name) override
    {
        if (_style == vtpty::PathStyle::Windows && !isPlainComponent(name, _style))
            return std::unexpected(vtpty::DirectoryError::AccessDenied);
        auto const path = childPath(parent.resolvedPath(), name, _style);
        if (auto const failure = lookup(createFailures, path); failure != createFailures.end())
            return std::unexpected(failure->second);
        if (auto const existing = lookup(directories, path); existing != directories.end())
        {
            if (existing->second != existing->first)
                return std::unexpected(vtpty::DirectoryError::NotADirectory); // A symlink is never followed.
            return vtpty::DirectoryHandle::unbacked(existing->second);
        }
        created.push_back(path);
        directories.emplace(path, path);
        return vtpty::DirectoryHandle::unbacked(path);
    }

    [[nodiscard]] vtpty::PathStyle pathStyle() const noexcept override { return _style; }

    /// Every path open() was asked for, in order.
    std::vector<std::string> opened;

    /// Runs first in every open().
    std::function<void(std::string_view path)> onOpen;

    /// Every directory createBeneath() created, by its whole path, in order.
    std::vector<std::string> created;

  private:
    /// @return The entry of @p map named @p path: by its spelling (Posix), by comparisonKeyOf() (Windows).
    template <typename Map>
    [[nodiscard]] Map::iterator lookup(Map& map, std::string_view path) const
    {
        if (_style == vtpty::PathStyle::Posix)
            return map.find(path);
        auto const key = comparisonKeyOf(path, _style);
        return std::ranges::find_if(
            map, [&](auto const& entry) { return comparisonKeyOf(entry.first, _style) == key; });
    }

    vtpty::PathStyle _style = vtpty::PathStyle::Posix;
};

} // namespace vtextension::testing
