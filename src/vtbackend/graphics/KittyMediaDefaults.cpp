// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyMediaDefaults.hpp>

#include <algorithm>
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
} // namespace

std::vector<std::filesystem::path> platformTemporaryRoots([[maybe_unused]] core::Environment const& env)
{
    auto roots = std::vector<std::filesystem::path> {};
#ifndef _WIN32
    auto const add = [&](std::filesystem::path const& root) {
        if (!root.is_absolute())
            return;
        auto canonical = canonicalRoot(root);
        if (std::ranges::find(roots, canonical) == roots.end())
            roots.push_back(std::move(canonical));
    };
    if (auto const tmpdir = env.get("TMPDIR"))
        add(*tmpdir);
    #ifdef __APPLE__
    add("/private/tmp");
    auto userTemp = std::array<char, PATH_MAX> {};
    if (auto const length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, userTemp.data(), userTemp.size());
        length > 0 && length <= userTemp.size())
        add(userTemp.data());
    #else
    add("/tmp");
    // kitty reads `t=t` from POSIX shared memory's directory too.
    add("/dev/shm");
    #endif
#endif
    return roots;
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
    auto allowedRoots = std::vector<std::filesystem::path> {};
    if (homeDirectory.is_absolute())
        allowedRoots.push_back(canonicalRoot(homeDirectory / DefaultAllowedRoot));
    return MediaPolicy { .transmission = Transmission,
                         .temporaryRoots = platformTemporaryRoots(env),
                         .allowedRoots = std::move(allowedRoots),
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
