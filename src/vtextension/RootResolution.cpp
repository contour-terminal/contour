// SPDX-License-Identifier: Apache-2.0
#include <vtextension/RootResolution.hpp>

#include <vtpty/PathContainment.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ArgumentKind.hpp>
#include <vtextension/RootPathSyntax.hpp>

namespace vtextension
{

namespace
{
    /// Whether @p path is @p directory or lies beneath it, by vtpty::isAtOrBeneath()'s rule: whole
    /// components, the platform's case rule. A spelling that names no path (not UTF-8, on Windows) lies
    /// inside nothing.
    [[nodiscard]] bool isInside(std::string_view directory, std::string_view path)
    {
        auto const directoryPath = core::pathFromUtf8(directory);
        auto const judgedPath = core::pathFromUtf8(path);
        return directoryPath && judgedPath
               && vtpty::isAtOrBeneath(*judgedPath, *directoryPath, vtpty::NativePathCase);
    }

    [[nodiscard]] ResolvedRoot const* rootNamed(std::span<ResolvedRoot const> roots,
                                                std::string_view label) noexcept
    {
        auto const found = std::ranges::find(roots, label, &ResolvedRoot::label);
        return found != roots.end() ? &*found : nullptr;
    }

    [[nodiscard]] RootError rootErrorOf(vtpty::DirectoryError error) noexcept
    {
        switch (error)
        {
            case vtpty::DirectoryError::Missing: return RootError::Missing;
            case vtpty::DirectoryError::NotADirectory:
            case vtpty::DirectoryError::AccessDenied: return RootError::Unopenable;
            case vtpty::DirectoryError::Unsupported: return RootError::Unsupported;
        }
        return RootError::Unopenable;
    }

    /// Refuses a path that is not absolute and normalised, by the grammar of a Path argument in @p style.
    [[nodiscard]] std::expected<void, RootError> checkSyntax(std::string_view path,
                                                             vtpty::PathStyle style) noexcept
    {
        if (!hasAbsoluteRoot(path, style))
            return std::unexpected(RootError::NotAbsolute);
        if (!argumentAccepts(ArgumentKind::Path, path, style))
            return std::unexpected(RootError::Unnormalized);
        return {};
    }

    /// The longest existing ancestor of a path, opened, and the components beneath it that do not exist yet.
    struct ExistingAncestor
    {
        vtpty::DirectoryHandle handle;
        std::vector<std::string> missing; ///< Top-down.

        /// @return The path the whole would have: the handle's resolved path, the missing components
        /// appended.
        [[nodiscard]] std::string resolvedPath() const
        {
            auto path = handle.resolvedPath();
            for (auto const& component: missing)
                path += (path == "/" ? "" : "/") + component;
            return path;
        }
    };

    /// Opens the longest existing ancestor of a normalised absolute path (which may be the path itself).
    [[nodiscard]] std::expected<ExistingAncestor, RootError> openExistingAncestor(
        vtpty::IDirectoryOpener& opener, std::string_view path)
    {
        auto existing = std::string(path);
        auto missing = std::vector<std::string> {};
        while (true)
        {
            auto opened = opener.open(existing);
            if (opened)
            {
                std::ranges::reverse(missing);
                return ExistingAncestor { .handle = std::move(*opened), .missing = std::move(missing) };
            }
            if (opened.error() != vtpty::DirectoryError::Missing)
                return std::unexpected(rootErrorOf(opened.error()));
            if (existing == "/")
                return std::unexpected(RootError::Missing);
            auto const slash = existing.rfind('/');
            missing.emplace_back(existing.substr(slash + 1));
            existing.resize(slash == 0 ? 1 : slash);
        }
    }

    /// Creates @p components beneath @p directory, one at a time, following no link.
    [[nodiscard]] std::expected<vtpty::DirectoryHandle, RootError> createBeneath(
        vtpty::IDirectoryOpener& opener,
        vtpty::DirectoryHandle directory,
        std::span<std::string const> components)
    {
        for (auto const& component: components)
        {
            auto created = opener.createBeneath(directory, component);
            if (!created)
                return std::unexpected(rootErrorOf(created.error()));
            directory = std::move(*created);
        }
        return directory;
    }

    /// Whether @p path lies inside a root other than @p root.
    [[nodiscard]] bool insideAnotherRoot(std::span<ResolvedRoot const> roots,
                                         std::string_view root,
                                         std::string_view path)
    {
        return std::ranges::any_of(roots, [&](ResolvedRoot const& other) {
            return other.label != root && std::ranges::any_of(other.paths, [&](std::string const& otherPath) {
                       return isInside(otherPath, path);
                   });
        });
    }

    /// Judges a resolved path: inside @p root, and @p root its most specific root.
    [[nodiscard]] std::expected<void, RootError> judge(std::span<ResolvedRoot const> roots,
                                                       std::string_view root,
                                                       std::string_view resolvedPath)
    {
        auto const owner = mostSpecificRoot(roots, resolvedPath);
        if (!owner)
            return std::unexpected(RootError::OutsideRoots);
        if (*owner != root)
            return std::unexpected(RootError::NotItsRoot);
        return {};
    }
} // namespace

std::optional<std::string_view> mostSpecificRoot(std::span<ResolvedRoot const> roots,
                                                 std::string_view resolvedPath)
{
    auto best = std::optional<std::string_view> {};
    auto bestLength = std::size_t { 0 };
    for (auto const& root: roots)
        for (auto const& path: root.paths)
            if (isInside(path, resolvedPath) && (!best || path.size() > bestLength))
            {
                best = root.label;
                bestLength = path.size();
            }
    return best;
}

std::expected<void, RootError> ensureRoot(vtpty::IDirectoryOpener& opener,
                                          std::span<ResolvedRoot const> roots,
                                          std::string_view root)
{
    auto const* const declared = rootNamed(roots, root);
    if (!declared)
        return std::unexpected(RootError::Unresolved);
    for (auto const& path: declared->paths)
    {
        auto ancestor = openExistingAncestor(opener, path);
        if (!ancestor)
            return std::unexpected(ancestor.error());
        if (ancestor->missing.empty())
            continue;
        if (insideAnotherRoot(roots, root, ancestor->resolvedPath()))
            return std::unexpected(RootError::CreateInsideRoot);
        if (auto const created = createBeneath(opener, std::move(ancestor->handle), ancestor->missing);
            !created)
            return std::unexpected(created.error());
    }
    return {};
}

std::expected<vtpty::DirectoryHandle, RootError> openInRoot(vtpty::IDirectoryOpener& opener,
                                                            std::span<ResolvedRoot const> roots,
                                                            std::string_view root,
                                                            std::string_view path)
{
    if (auto const syntax = checkSyntax(path, opener.pathStyle()); !syntax)
        return std::unexpected(syntax.error());
    auto directory = opener.open(path);
    if (!directory)
        return std::unexpected(rootErrorOf(directory.error()));
    if (auto const judged = judge(roots, root, directory->resolvedPath()); !judged)
        return std::unexpected(judged.error());
    return std::move(*directory);
}

namespace
{
    /// Resolves and judges a `path:<root>` value, creating nothing.
    [[nodiscard]] std::expected<ExistingAncestor, RootError> resolveInRoot(
        vtpty::IDirectoryOpener& opener,
        std::span<ResolvedRoot const> roots,
        std::string_view root,
        std::string_view value)
    {
        if (auto const syntax = checkSyntax(value, opener.pathStyle()); !syntax)
            return std::unexpected(syntax.error());
        auto ancestor = openExistingAncestor(opener, value);
        if (!ancestor)
            return std::unexpected(ancestor.error());
        auto const resolved = ancestor->resolvedPath();
        if (auto const judged = judge(roots, root, resolved); !judged)
            return std::unexpected(judged.error());
        if (std::ranges::contains(rootNamed(roots, root)->paths, resolved))
            return std::unexpected(RootError::OutsideRoots); // The root itself is not below it.
        return ancestor;
    }
} // namespace

std::expected<std::string, RootError> resolvePathArgument(vtpty::IDirectoryOpener& opener,
                                                          std::span<ResolvedRoot const> roots,
                                                          std::string_view root,
                                                          std::string_view value)
{
    return resolveInRoot(opener, roots, root, value).transform(&ExistingAncestor::resolvedPath);
}

std::expected<std::string, RootError> createPathParents(vtpty::IDirectoryOpener& opener,
                                                        std::span<ResolvedRoot const> roots,
                                                        std::string_view root,
                                                        std::string_view value)
{
    auto ancestor = resolveInRoot(opener, roots, root, value);
    if (!ancestor)
        return std::unexpected(ancestor.error());
    if (ancestor->missing.size() <= 1)
        return ancestor->resolvedPath();

    // Every missing component but the last is created by handle; the last stays a name.
    auto const above = std::span<std::string const>(ancestor->missing).first(ancestor->missing.size() - 1);
    auto const parent = createBeneath(opener, std::move(ancestor->handle), above);
    if (!parent)
        return std::unexpected(parent.error());
    auto const& leaf = ancestor->missing.back();
    return parent->resolvedPath() == "/" ? "/" + leaf : parent->resolvedPath() + "/" + leaf;
}

std::expected<void, RootError> checkRootsDistinct(std::span<ResolvedRoot const> roots)
{
    auto paths = std::vector<std::string_view> {};
    for (auto const& root: roots)
        paths.insert(paths.end(), root.paths.begin(), root.paths.end());
    std::ranges::sort(paths);
    if (std::ranges::adjacent_find(paths) != paths.end())
        return std::unexpected(RootError::EqualRoots);
    return {};
}

} // namespace vtextension
