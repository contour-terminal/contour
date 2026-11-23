// SPDX-License-Identifier: Apache-2.0
#include <vtextension/RootResolution.hpp>

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
        vtpty::PathStyle style;           ///< The grammar of the opener it was opened with.

        /// @return The path the whole would have: the handle's resolved path, the missing components
        /// appended.
        [[nodiscard]] std::string resolvedPath() const
        {
            auto path = handle.resolvedPath();
            for (auto const& component: missing)
                path = childPath(path, component, style);
            return path;
        }
    };

    /// Opens the longest existing ancestor of a normalised absolute path (which may be the path itself).
    [[nodiscard]] std::expected<ExistingAncestor, RootError> openExistingAncestor(
        vtpty::IDirectoryOpener& opener, std::string_view path)
    {
        auto const style = opener.pathStyle();
        auto existing = std::string(path);
        auto missing = std::vector<std::string> {};
        while (true)
        {
            auto opened = opener.open(existing);
            if (opened)
            {
                std::ranges::reverse(missing);
                return ExistingAncestor { .handle = std::move(*opened),
                                          .missing = std::move(missing),
                                          .style = style };
            }
            if (opened.error() != vtpty::DirectoryError::Missing)
                return std::unexpected(rootErrorOf(opened.error()));
            // A volume root has no parent to look in.
            auto const split = parentAndLeaf(existing, style);
            if (!split)
                return std::unexpected(RootError::Missing);
            missing.emplace_back(split->second);
            existing.resize(split->first.size());
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

    /// Whether @p path lies inside a root other than @p root, by isAtOrBeneath() in @p style.
    [[nodiscard]] bool insideAnotherRoot(std::span<ResolvedRoot const> roots,
                                         std::string_view root,
                                         std::string_view path,
                                         vtpty::PathStyle style)
    {
        return std::ranges::any_of(roots, [&](ResolvedRoot const& other) {
            return other.label != root && std::ranges::any_of(other.paths, [&](std::string const& otherPath) {
                       return isAtOrBeneath(path, otherPath, style);
                   });
        });
    }

    /// Judges a path as written, before anything is opened: at or beneath one of @p root's own directories by
    /// isAtOrBeneath() in @p style. Opening is not harmless (a UNC path connects to its server and hands it
    /// the user's credentials; an automounter or a FUSE mount answers a lookup), so a path that could not be
    /// inside @p root is refused without one.
    /// @return Nothing; otherwise the error the path would be refused with had it resolved to itself:
    ///         `NotItsRoot` when another root holds it, `OutsideRoots` when none does.
    [[nodiscard]] std::expected<void, RootError> judgeAsWritten(std::span<ResolvedRoot const> roots,
                                                                std::string_view root,
                                                                std::string_view path,
                                                                vtpty::PathStyle style)
    {
        auto const* const declared = rootNamed(roots, root);
        if (declared && std::ranges::any_of(declared->paths, [&](std::string const& directory) {
                return isAtOrBeneath(path, directory, style);
            }))
            return {};
        return std::unexpected(mostSpecificRoot(roots, path, style) ? RootError::NotItsRoot
                                                                    : RootError::OutsideRoots);
    }

    /// Judges a resolved path: inside @p root, and @p root its most specific root.
    [[nodiscard]] std::expected<void, RootError> judge(std::span<ResolvedRoot const> roots,
                                                       std::string_view root,
                                                       std::string_view resolvedPath,
                                                       vtpty::PathStyle style)
    {
        auto const owner = mostSpecificRoot(roots, resolvedPath, style);
        if (!owner)
            return std::unexpected(RootError::OutsideRoots);
        if (*owner != root)
            return std::unexpected(RootError::NotItsRoot);
        return {};
    }
} // namespace

std::optional<std::string_view> mostSpecificRoot(std::span<ResolvedRoot const> roots,
                                                 std::string_view resolvedPath,
                                                 vtpty::PathStyle style)
{
    auto best = std::optional<std::string_view> {};
    auto bestLength = std::size_t { 0 };
    for (auto const& root: roots)
        for (auto const& path: root.paths)
            if (isAtOrBeneath(resolvedPath, path, style) && (!best || path.size() > bestLength))
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
        if (insideAnotherRoot(roots, root, ancestor->resolvedPath(), opener.pathStyle()))
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
    auto const style = opener.pathStyle();
    if (auto const syntax =
            checkSyntax(path, style).and_then([&] { return judgeAsWritten(roots, root, path, style); });
        !syntax)
        return std::unexpected(syntax.error());
    auto directory = opener.open(path);
    if (!directory)
        return std::unexpected(rootErrorOf(directory.error()));
    if (auto const judged = judge(roots, root, directory->resolvedPath(), style); !judged)
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
        auto const style = opener.pathStyle();
        if (auto const syntax =
                checkSyntax(value, style).and_then([&] { return judgeAsWritten(roots, root, value, style); });
            !syntax)
            return std::unexpected(syntax.error());
        auto ancestor = openExistingAncestor(opener, value);
        if (!ancestor)
            return std::unexpected(ancestor.error());
        auto const resolved = ancestor->resolvedPath();
        if (auto const judged = judge(roots, root, resolved, style); !judged)
            return std::unexpected(judged.error());
        // The root itself, in whatever case it is spelled, is not below it.
        if (std::ranges::any_of(rootNamed(roots, root)->paths,
                                [&](std::string const& path) { return samePath(path, resolved, style); }))
            return std::unexpected(RootError::OutsideRoots);
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
    return childPath(parent->resolvedPath(), ancestor->missing.back(), ancestor->style);
}

std::expected<void, RootError> checkRootsDistinct(std::span<ResolvedRoot const> roots, vtpty::PathStyle style)
{
    auto keys = std::vector<std::string> {};
    for (auto const& root: roots)
        for (auto const& path: root.paths)
            keys.push_back(comparisonKeyOf(path, style));
    std::ranges::sort(keys);
    if (std::ranges::adjacent_find(keys) != keys.end())
        return std::unexpected(RootError::EqualRoots);
    return {};
}

} // namespace vtextension
