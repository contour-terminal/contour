// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyMediaDefaults.hpp>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <system_error>

#ifdef __APPLE__
    #include <array>
    #include <climits>
#endif
#ifndef _WIN32
    #include <unistd.h>
#endif

namespace vtbackend::kitty_graphics
{

namespace
{
    /// The allow-list root below the home directory: where Claude Code's plugins cache what they render.
    constexpr auto DefaultAllowedRoot = std::string_view { ".claude/plugins/data" };

    /// @return @p path with every link resolved as far as it exists: the policy compares canonical paths.
    [[nodiscard]] std::filesystem::path canonicalRoot(std::filesystem::path const& path)
    {
        auto error = std::error_code {};
        auto canonical = std::filesystem::weakly_canonical(path, error);
        return error ? path.lexically_normal() : canonical;
    }

    /// Adds @p root to @p roots as spelled and canonical, unless it is relative or already there.
    void addRoot(std::vector<std::filesystem::path>& roots, std::filesystem::path const& root)
    {
        if (!root.is_absolute())
            return;
        for (auto const& spelling: { root.lexically_normal(), canonicalRoot(root) })
        {
            // Without the trailing separator `$TMPDIR` often carries: the policy compares components.
            auto trimmed = spelling.has_filename() ? spelling : spelling.parent_path();
            if (std::ranges::find(roots, trimmed) == roots.end())
                roots.push_back(std::move(trimmed));
        }
    }

    /// @return Whether @p root, made canonical, would open the whole home directory @p canonicalHome: it is
    ///         `/`, the home directory, or a directory above it.
    [[nodiscard]] bool isOverBroad(std::filesystem::path const& root,
                                   std::filesystem::path const& canonicalHome)
    {
        auto const canonical = canonicalRoot(root);
        if (canonical == canonical.root_path())
            return true;
        if (canonicalHome.empty())
            return false;
        auto const [rootEnd, homeEnd] = std::ranges::mismatch(canonical, canonicalHome);
        return rootEnd == canonical.end();
    }

    /// @return @p roots without those isOverBroad() refuses.
    [[nodiscard]] std::vector<std::filesystem::path> withoutOverBroad(
        std::vector<std::filesystem::path> roots, std::filesystem::path const& canonicalHome)
    {
        std::erase_if(roots,
                      [&](std::filesystem::path const& root) { return isOverBroad(root, canonicalHome); });
        return roots;
    }
} // namespace

std::vector<std::filesystem::path> platformTemporaryRoots([[maybe_unused]] core::Environment const& env)
{
    auto roots = std::vector<std::filesystem::path> {};
#ifndef _WIN32
    if (auto const tmpdir = env.get("TMPDIR"))
        addRoot(roots, *tmpdir);
    #ifdef __APPLE__
    addRoot(roots, "/private/tmp");
    auto userTemp = std::array<char, PATH_MAX> {};
    if (auto const length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, userTemp.data(), userTemp.size());
        length > 0 && length <= userTemp.size())
        addRoot(roots, userTemp.data());
    #else
    addRoot(roots, "/tmp");
    #endif
#endif
    return roots;
}

std::vector<std::filesystem::path> platformTemporaryFileOnlyRoots()
{
#ifdef __linux__
    return { "/dev/shm" };
#else
    return {};
#endif
}

MediaPolicy defaultMediaPolicy(core::Environment const& env,
                               std::filesystem::path const& homeDirectory,
                               uint32_t userId)
{
#ifdef _WIN32
    // The owner, hidden-file and reparse-point rules are not defined there yet.
    constexpr auto Transmission = FileTransmission::Disabled;
#else
    constexpr auto Transmission = FileTransmission::Enabled;
#endif
    // A home directory that is not absolute (unset) names no allow-list root: one relative to the working
    // directory would be wherever the process happened to start.
    auto const canonicalHome =
        homeDirectory.is_absolute() ? canonicalRoot(homeDirectory) : std::filesystem::path {};
    auto allowedRoots = std::vector<std::filesystem::path> {};
    if (homeDirectory.is_absolute())
        addRoot(allowedRoots, homeDirectory / DefaultAllowedRoot);
    return MediaPolicy { .transmission = Transmission,
                         .temporaryRoots = withoutOverBroad(platformTemporaryRoots(env), canonicalHome),
                         .temporaryFileOnlyRoots =
                             withoutOverBroad(platformTemporaryFileOnlyRoots(), canonicalHome),
                         .allowedRoots = withoutOverBroad(std::move(allowedRoots), canonicalHome),
                         .userId = userId,
                         .sizeLimit = MaxChunkedPayloadSize };
}

uint32_t currentUserId() noexcept
{
#ifdef _WIN32
    return 0;
#else
    return static_cast<uint32_t>(::getuid());
#endif
}

std::unique_ptr<KittyMediumSource const> makePlatformMediumSource()
{
#ifdef _WIN32
    return std::make_unique<RefusingMediumSource>();
#else
    return std::make_unique<PosixMediumSource>();
#endif
}

} // namespace vtbackend::kitty_graphics
