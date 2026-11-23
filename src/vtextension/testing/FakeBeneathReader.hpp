// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IBeneathReader over files named by their whole path, for handles of any kind -- the unbacked
// ones FakeDirectoryOpener hands out included.
#pragma once

#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/PathStyle.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/RootPathSyntax.hpp>

namespace vtextension::testing
{

/// Answers each read from @ref files by the path it names: the root's resolved path, then each component,
/// joined by childPath() in the fake's path grammar. A path it holds no answer for is answered @ref
/// otherwise.
class FakeBeneathReader final: public vtpty::IBeneathReader
{
  public:
    /// A fake that joins paths in the POSIX grammar.
    FakeBeneathReader() = default;

    /// @param style The path grammar the fake joins a root's resolved path and its components in.
    explicit FakeBeneathReader(vtpty::PathStyle style) noexcept: _style { style } {}

    /// What a read of each path answers: its file, or why it is not read.
    std::map<std::string, std::expected<vtpty::BeneathFile, vtpty::BeneathReadError>, std::less<>> files;

    /// What a read of any other path answers; `Unsupported`, as a host that reads no file, unless set.
    vtpty::BeneathReadError otherwise = vtpty::BeneathReadError::Unsupported;

    /// Every path read, in order, and the limit it was read with.
    std::vector<std::pair<std::string, std::size_t>> reads;

    /// Adds a file at @p path holding @p bytes.
    void add(std::string const& path, std::string bytes, uint64_t modifiedUnixMs = 0)
    {
        files.insert_or_assign(
            path, vtpty::BeneathFile { .bytes = std::move(bytes), .modifiedUnixMs = modifiedUnixMs });
    }

    [[nodiscard]] std::expected<vtpty::BeneathFile, vtpty::BeneathReadError> read(
        vtpty::DirectoryHandle const& root,
        std::span<std::string_view const> components,
        std::size_t maxBytes,
        std::span<vtpty::FileIdentity const> fenced) override
    {
        if (removedRoots.contains(root.resolvedPath()))
            return std::unexpected(vtpty::BeneathReadError::DirectoryMissing);
        auto path = root.resolvedPath();
        for (auto const component: components.first(components.empty() ? 0 : components.size() - 1))
        {
            path = childPath(path, component, _style);
            // any_of, not contains: MSVC's STL takes find's vectorized path for a trivially comparable
            // 16-byte element under clang-cl, and fails its static_assert on the element's size.
            if (auto const identity = identities.find(path);
                identity != identities.end()
                && std::ranges::any_of(fenced, [&](auto const& fence) { return fence == identity->second; }))
                return std::unexpected(vtpty::BeneathReadError::Fenced);
        }
        if (!components.empty())
            path = childPath(path, components.back(), _style);
        reads.emplace_back(path, maxBytes);
        if (auto const file = files.find(path); file != files.end())
            return file->second;
        return std::unexpected(otherwise);
    }

    [[nodiscard]] std::expected<vtpty::FileIdentity, vtpty::BeneathReadError> identityOf(
        vtpty::DirectoryHandle const& root, std::span<std::string_view const> components) override
    {
        if (removedRoots.contains(root.resolvedPath()))
            return std::unexpected(vtpty::BeneathReadError::DirectoryMissing);
        auto path = root.resolvedPath();
        for (auto const component: components)
            path = childPath(path, component, _style);
        identified.push_back(path);
        if (auto const identity = identities.find(path); identity != identities.end())
            return identity->second;
        return std::unexpected(vtpty::BeneathReadError::Escapes);
    }

    /// Roots, by their resolved path, removed while held: every read or identityOf() beneath a handle on one
    /// answers `DirectoryMissing`.
    std::set<std::string, std::less<>> removedRoots;

    /// Every directory identityOf() was asked for, by its whole path, in order.
    std::vector<std::string> identified;

    /// The identity of each directory, by its whole path: one without is unreachable, and never fenced.
    std::map<std::string, vtpty::FileIdentity, std::less<>> identities;

  private:
    vtpty::PathStyle _style = vtpty::PathStyle::Posix;
};

} // namespace vtextension::testing
