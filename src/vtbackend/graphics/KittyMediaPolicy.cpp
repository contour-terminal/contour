// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <core/Utf8Path.hpp>

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

    /// The programs that, in the foreground of a PTY, relay output written somewhere other than this
    /// machine's own filesystem: another host's (ssh and its kin, PuTTY's `plink`), any pane's of a
    /// multiplexer (whose panes may run ssh), or a container's or a VM's (`wsl` and the `wslhost` it runs
    /// through, Hyper-V's `hvc`). Listed by the name a PTY reports (Linux `comm`, at most 15 characters),
    /// last path component only, in lower case and without `.exe`: isRemoteShellClient() compares a Windows
    /// image name so. A denylist, so a relay not named here passes as local; the kitty graphics page says so.
    constexpr auto RemoteShellClients = std::array<std::string_view, 28> {
        "autossh",   "dbclient", "distrobox-enter", "docker", "et",   "hvc",
        "incus",     "kitten",   "kubectl",         "lxc",    "mosh", "mosh-client",
        "multipass", "plink",    "podman",          "rlogin", "rsh",  "screen",
        "slogin",    "ssh",      "sshpass",         "telnet", "tmux", "toolbox",
        "vagrant",   "wsl",      "wslhost",         "zellij",
    };

    /// The suffix a Windows image name carries that the POSIX name of the same program does not.
    constexpr auto ExecutableSuffix = std::string_view { ".exe" };

    /// @return @p c with an ASCII upper-case letter folded to lower case; any other code unit as it is.
    template <typename Char>
    [[nodiscard]] constexpr Char asciiFolded(Char c) noexcept
    {
        return c >= Char { 'A' } && c <= Char { 'Z' } ? static_cast<Char>(c - Char { 'A' } + Char { 'a' })
                                                      : c;
    }

    /// @return Whether @p lhs and @p rhs are equal, ASCII case ignored.
    [[nodiscard]] constexpr bool equalFolded(std::string_view lhs, std::string_view rhs) noexcept
    {
        return std::ranges::equal(lhs, rhs, [](char a, char b) { return asciiFolded(a) == asciiFolded(b); });
    }

    /// @return Whether the path components @p lhs and @p rhs name the same entry under @p pathCase. Case
    ///         insensitive, the two separators are alike too, as they are on Windows.
    [[nodiscard]] bool sameComponent(std::filesystem::path const& lhs,
                                     std::filesystem::path const& rhs,
                                     PathCase pathCase)
    {
        switch (pathCase)
        {
            case PathCase::Sensitive: return lhs == rhs;
            case PathCase::Insensitive: break;
        }
        using NativeChar = std::filesystem::path::value_type;
        auto const folded = [](NativeChar c) {
            return c == NativeChar { '/' } ? NativeChar { '\\' } : asciiFolded(c);
        };
        return std::ranges::equal(
            lhs.native(), rhs.native(), [&](NativeChar a, NativeChar b) { return folded(a) == folded(b); });
    }

    /// The marker a `t=t` file's path must carry to be deleted, kitty's rule against deleting files not meant
    /// for it: looked for anywhere in the full path, as the protocol has it.
    constexpr auto TemporaryNameMarker = std::string_view { "tty-graphics-protocol" };

    /// @return Whether @p path carries TemporaryNameMarker, compared in the platform's native character
    ///         type (wide on Windows) without converting, so it neither allocates nor throws.
    [[nodiscard]] bool carriesTemporaryNameMarker(std::filesystem::path const& path) noexcept
    {
        using NativeChar = std::filesystem::path::value_type;
        return !std::ranges::search(path.native(), TemporaryNameMarker, [](NativeChar lhs, char rhs) {
                    return lhs == static_cast<NativeChar>(rhs);
                }).empty();
    }

    /// @return @p path's components, without the empty one a trailing separator leaves.
    [[nodiscard]] std::vector<std::filesystem::path> componentsOf(std::filesystem::path const& path)
    {
        auto components = std::vector<std::filesystem::path> {};
        for (auto const& component: path)
            if (!component.empty())
                components.push_back(component);
        return components;
    }

    /// @return The components of @p path below @p root, compared component by component under @p pathCase
    ///         (so `/tmpx` is not below `/tmp`), or nullopt when @p path is not strictly below @p root.
    [[nodiscard]] std::optional<std::vector<std::filesystem::path>> componentsBelow(
        std::filesystem::path const& root, std::filesystem::path const& path, PathCase pathCase)
    {
        if (!root.is_absolute())
            return std::nullopt;
        auto const rootComponents = componentsOf(root);
        auto pathComponents = componentsOf(path);
        auto const same = [pathCase](std::filesystem::path const& lhs, std::filesystem::path const& rhs) {
            return sameComponent(lhs, rhs, pathCase);
        };
        if (pathComponents.size() <= rootComponents.size()
            || !std::ranges::equal(
                rootComponents, pathComponents | std::views::take(rootComponents.size()), same))
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

    /// @return Whether the path component @p component is hidden: its name starts with a dot.
    [[nodiscard]] bool isHidden(std::filesystem::path const& component)
    {
        return component.native().starts_with('.');
    }

    /// @return The roots @p medium may be read below besides the temp roots. `t=t` deletes what it reads, so
    ///         it reads only where deleting is the norm: the temp roots and those for temporary files only.
    [[nodiscard]] std::vector<std::filesystem::path> const& furtherRootsFor(MediaPolicy const& policy,
                                                                            Medium medium) noexcept
    {
        return medium == Medium::File ? policy.allowedRoots : policy.temporaryFileOnlyRoots;
    }

    [[nodiscard]] Placement placementIn(std::span<std::filesystem::path const> roots,
                                        std::filesystem::path const& path,
                                        PathCase pathCase)
    {
        auto placement = Placement::Outside;
        for (auto const& root: roots)
        {
            auto const below = componentsBelow(root, path, pathCase);
            if (!below)
                continue;
            if (std::ranges::none_of(*below, isHidden))
                return Placement::Inside;
            placement = Placement::Hidden;
        }
        return placement;
    }

    /// The root a reader walks from, and what lies below it.
    struct Enclosing
    {
        std::filesystem::path root;
        std::vector<std::filesystem::path> belowRoot;
    };

    /// @return The deepest root eligible for @p medium that @p path lies below with no hidden component in
    ///         between, and the components below it; nullopt when there is none.
    [[nodiscard]] std::optional<Enclosing> enclosingOf(MediaPolicy const& policy,
                                                       std::filesystem::path const& path,
                                                       Medium medium)
    {
        auto const& furtherRoots = furtherRootsFor(policy, medium);
        auto best = std::optional<Enclosing> {};
        auto bestDepth = size_t { 0 };
        for (auto const& roots: { std::span { policy.temporaryRoots }, std::span { furtherRoots } })
            for (auto const& root: roots)
            {
                auto below = componentsBelow(root, path, policy.pathCase);
                if (!below || std::ranges::any_of(*below, isHidden))
                    continue;
                if (auto const depth = componentsOf(root).size(); !best || depth > bestDepth)
                {
                    best = Enclosing { .root = root, .belowRoot = std::move(*below) };
                    bestDepth = depth;
                }
            }
        return best;
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

std::expected<void, MediaRefusal> checkRequest(MediaPolicy const& policy,
                                               Medium medium,
                                               std::string_view path) noexcept
{
    return checkMedium(policy, medium).and_then([&]() -> std::expected<void, MediaRefusal> {
        if (path.size() > MaxFilePathLength)
            return std::unexpected(MediaRefusal::PathTooLong);
        return {};
    });
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

        auto placement = placementIn(policy.temporaryRoots, realPath, policy.pathCase);
        if (placement != Placement::Inside)
            placement =
                std::max(placement, placementIn(furtherRootsFor(policy, medium), realPath, policy.pathCase));
        switch (placement)
        {
            case Placement::Outside: return std::unexpected(MediaRefusal::OutsideRoots);
            case Placement::Hidden: return std::unexpected(MediaRefusal::HiddenComponent);
            case Placement::Inside: break;
        }
        return {};
    });
}

std::expected<void, MediaRefusal> checkOpenedFile(MediaPolicy const& policy, OpenedFile const& file) noexcept
{
    if (file.kind != FileKind::Regular)
        return std::unexpected(MediaRefusal::NotRegularFile);
    if (file.owner != FileOwner::Reader)
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
    return enclosingOf(policy, path, medium).transform([](Enclosing enclosing) {
        return std::move(enclosing.root);
    });
}

std::expected<RequestedPath, MediaRefusal> requestedPath(MediaPolicy const& policy,
                                                         Medium medium,
                                                         std::string_view path)
{
    // Refused before the filesystem is touched at all.
    if (auto const accepted = checkRequest(policy, medium, path); !accepted)
        return std::unexpected(accepted.error());
    if (path.empty() || path.contains('\0'))
        return std::unexpected(MediaRefusal::Unreadable);
    // As UTF-8 on every platform: a narrow path constructor reads Windows' ANSI code page, which names
    // another file than the application did.
    auto const decoded = core::pathFromUtf8(path);
    if (!decoded)
        return std::unexpected(MediaRefusal::Unreadable);

    // Judged as requested before anything is looked up: a lookup outside the roots could reach an automount
    // or a network filesystem -- a stall under the terminal lock, and a beacon to whoever named it. A
    // relative path is refused too, as it would resolve against the terminal's own working directory.
    auto requested = decoded->lexically_normal();
    if (auto const accepted = checkPath(policy, requested, medium); !accepted)
        return std::unexpected(accepted.error());
    auto enclosing = enclosingOf(policy, requested, medium);
    if (!enclosing)
        return std::unexpected(MediaRefusal::OutsideRoots);
    return RequestedPath { .path = std::move(requested),
                           .root = std::move(enclosing->root),
                           .belowRoot = std::move(enclosing->belowRoot) };
}

bool isRemoteShellClient(std::string_view processName) noexcept
{
    auto const separator = processName.find_last_of("/\\");
    auto name = separator == std::string_view::npos ? processName : processName.substr(separator + 1);
    if (name.size() > ExecutableSuffix.size()
        && equalFolded(name.substr(name.size() - ExecutableSuffix.size()), ExecutableSuffix))
        name.remove_suffix(ExecutableSuffix.size());
    return std::ranges::any_of(RemoteShellClients,
                               [name](std::string_view client) { return equalFolded(client, name); });
}

DeletionAfterRead deletionAfterRead(MediaPolicy const& policy,
                                    std::filesystem::path const& realPath,
                                    Medium medium)
{
    // checkPath() reads a `t=t` file only inside a temp root, never an allow-list root.
    return medium == Medium::TemporaryFile && carriesTemporaryNameMarker(realPath)
                   && checkPath(policy, realPath, medium)
               ? DeletionAfterRead::Delete
               : DeletionAfterRead::Keep;
}

} // namespace vtbackend::kitty_graphics
