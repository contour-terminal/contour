// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>

namespace vtbackend::kitty_graphics
{

namespace
{
    /// Trees no file medium is read from, whatever root an application names: reading in them has side
    /// effects (devices) or reveals what no image needs (process environments, kernel state).
    constexpr auto SystemTrees = std::array<std::string_view, 3> { "/proc", "/sys", "/dev" };

    /// Subtrees of SystemTrees still readable by `t=t` alone, from a temp root there, as kitty allows.
    constexpr auto TemporaryFileSystemTreeExemptions = std::array<std::string_view, 1> { "/dev/shm" };

    /// The programs that, in the foreground of a PTY, relay another host's output: the names a PTY reports
    /// (Linux `comm`, at most 15 characters), last path component only.
    constexpr auto RemoteShellClients = std::array<std::string_view, 12> {
        "autossh", "dbclient", "et",     "kitten", "mosh",    "mosh-client",
        "rlogin",  "rsh",      "slogin", "ssh",    "sshpass", "telnet",
    };

    /// The marker every `t=t` file name carries, kitty's rule against deleting files not meant for it.
    constexpr auto TemporaryNameMarker = std::string_view { "tty-graphics-protocol" };

    /// @return @p path's components, without the empty one a trailing separator leaves.
    [[nodiscard]] std::vector<std::filesystem::path> componentsOf(std::filesystem::path const& path)
    {
        auto components = std::vector<std::filesystem::path> {};
        for (auto const& component: path)
            if (!component.empty())
                components.push_back(component);
        return components;
    }

    /// @return The components of @p path below @p root, compared component by component (so `/tmpx` is not
    ///         below `/tmp`), or nullopt when @p path is not strictly below @p root.
    [[nodiscard]] std::optional<std::vector<std::filesystem::path>> componentsBelow(
        std::filesystem::path const& root, std::filesystem::path const& path)
    {
        if (!root.is_absolute())
            return std::nullopt;
        auto const rootComponents = componentsOf(root);
        auto pathComponents = componentsOf(path);
        if (pathComponents.size() <= rootComponents.size()
            || !std::ranges::equal(rootComponents, pathComponents | std::views::take(rootComponents.size())))
            return std::nullopt;
        pathComponents.erase(pathComponents.begin(),
                             pathComponents.begin() + static_cast<std::ptrdiff_t>(rootComponents.size()));
        return pathComponents;
    }

    /// @return Whether @p path is @p tree or lies below it.
    [[nodiscard]] bool isWithin(std::string_view tree, std::filesystem::path const& path)
    {
        auto const treeComponents = componentsOf(std::filesystem::path { tree });
        auto const pathComponents = componentsOf(path);
        return pathComponents.size() >= treeComponents.size()
               && std::ranges::equal(treeComponents,
                                     pathComponents | std::views::take(treeComponents.size()));
    }

    [[nodiscard]] bool isSystemPath(std::filesystem::path const& path, Medium medium)
    {
        auto const within = [&](std::string_view tree) {
            return isWithin(tree, path);
        };
        if (!std::ranges::any_of(SystemTrees, within))
            return false;
        return medium != Medium::TemporaryFile
               || !std::ranges::any_of(TemporaryFileSystemTreeExemptions, within);
    }

    /// Where a path lies relative to a set of roots.
    enum class Placement : uint8_t
    {
        Outside = 0, ///< Below none of them.
        Hidden = 1,  ///< Below some, but behind a hidden component below each of those.
        Inside = 2,  ///< Below one with no hidden component in between.
    };

    [[nodiscard]] Placement placementIn(std::span<std::filesystem::path const> roots,
                                        std::filesystem::path const& path)
    {
        auto placement = Placement::Outside;
        for (auto const& root: roots)
        {
            auto const below = componentsBelow(root, path);
            if (!below)
                continue;
            auto const hidden = std::ranges::any_of(*below, [](std::filesystem::path const& component) {
                return component.native().starts_with('.');
            });
            if (!hidden)
                return Placement::Inside;
            placement = Placement::Hidden;
        }
        return placement;
    }
} // namespace

std::expected<void, MediaRefusal> checkMedium(MediaPolicy const& policy, Medium medium) noexcept
{
    if (policy.transmission == FileTransmission::Disabled)
        return std::unexpected(MediaRefusal::Disabled);
    switch (medium)
    {
        case Medium::File:
        case Medium::TemporaryFile: return {};
        case Medium::Direct:
        case Medium::SharedMemory: break;
    }
    return std::unexpected(MediaRefusal::UnsupportedMedium);
}

std::expected<void, MediaRefusal> checkPath(MediaPolicy const& policy,
                                            std::filesystem::path const& realPath,
                                            Medium medium)
{
    return checkMedium(policy, medium).and_then([&]() -> std::expected<void, MediaRefusal> {
        if (!realPath.is_absolute())
            return std::unexpected(MediaRefusal::OutsideRoots);
        if (isSystemPath(realPath, medium))
            return std::unexpected(MediaRefusal::SystemPath);

        // `t=t` deletes what it reads, so it reads only where deleting is the norm: the temp roots.
        auto const& furtherRoots =
            medium == Medium::File ? policy.allowedRoots : policy.temporaryFileOnlyRoots;
        auto placement = placementIn(policy.temporaryRoots, realPath);
        if (placement != Placement::Inside)
            placement = std::max(placement, placementIn(furtherRoots, realPath));
        switch (placement)
        {
            case Placement::Outside: return std::unexpected(MediaRefusal::OutsideRoots);
            case Placement::Hidden: return std::unexpected(MediaRefusal::HiddenComponent);
            case Placement::Inside: break;
        }

        if (medium == Medium::TemporaryFile && !realPath.filename().native().contains(TemporaryNameMarker))
            return std::unexpected(MediaRefusal::NotTemporaryName);
        return {};
    });
}

std::expected<void, MediaRefusal> checkOpenedFile(MediaPolicy const& policy, OpenedFile const& file) noexcept
{
    if (file.kind != FileKind::Regular)
        return std::unexpected(MediaRefusal::NotRegularFile);
    if (file.ownerId != policy.userId)
        return std::unexpected(MediaRefusal::WrongOwner);
    // A hard link into an allowed root must not reach a file outside it, `~/.ssh/id_rsa` say.
    if (file.linkCount != 1)
        return std::unexpected(MediaRefusal::MultipleLinks);
    if (file.size > policy.sizeLimit)
        return std::unexpected(MediaRefusal::TooLarge);
    return {};
}

std::optional<std::filesystem::path> enclosingRoot(MediaPolicy const& policy,
                                                   std::filesystem::path const& path,
                                                   Medium medium)
{
    auto const& furtherRoots = medium == Medium::File ? policy.allowedRoots : policy.temporaryFileOnlyRoots;
    auto best = std::optional<std::filesystem::path> {};
    auto bestDepth = size_t { 0 };
    for (auto const& roots: { std::span { policy.temporaryRoots }, std::span { furtherRoots } })
        for (auto const& root: roots)
        {
            auto const below = componentsBelow(root, path);
            if (!below || std::ranges::any_of(*below, [](std::filesystem::path const& component) {
                    return component.native().starts_with('.');
                }))
                continue;
            if (auto const depth = componentsOf(root).size(); !best || depth > bestDepth)
            {
                best = root;
                bestDepth = depth;
            }
        }
    return best;
}

bool isRemoteShellClient(std::string_view processName) noexcept
{
    auto const separator = processName.rfind('/');
    auto const name = separator == std::string_view::npos ? processName : processName.substr(separator + 1);
    return std::ranges::contains(RemoteShellClients, name);
}

DeletionAfterRead deletionAfterRead(MediaPolicy const& policy,
                                    std::filesystem::path const& realPath,
                                    Medium medium)
{
    // checkPath() reads a `t=t` file only inside a temp root, never an allow-list root.
    return medium == Medium::TemporaryFile && checkPath(policy, realPath, medium) ? DeletionAfterRead::Delete
                                                                                  : DeletionAfterRead::Keep;
}

} // namespace vtbackend::kitty_graphics
