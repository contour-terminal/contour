// SPDX-License-Identifier: Apache-2.0
//
// The media policy a composition root grants local sessions from the configuration's settings.

#include <vtbackend/graphics/KittyMediaDefaults.hpp>

#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace vtbackend::kitty_graphics;

namespace
{
[[nodiscard]] bool holds(std::vector<std::filesystem::path> const& roots, std::filesystem::path const& root)
{
    return std::ranges::find(roots, root) != roots.end();
}

/// The settings the configuration makes by default, `~` resolved against @p home.
[[nodiscard]] MediaSettings claudeDefaults(std::filesystem::path const& home)
{
    return MediaSettings { .transmission = FileTransmission::Enabled,
                           .allowList = { home / ".claude/plugins/data" } };
}
} // namespace

TEST_CASE("KittyMediaDefaults.platformTemporaryRoots", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/x" } } };
    auto const roots = platformTemporaryRoots(env);
#ifdef _WIN32
    CHECK(roots.empty());
#else
    CHECK(holds(roots, "/x"));
    #ifdef __APPLE__
    CHECK(holds(roots, "/private/tmp"));
    // As spelled too: the requested path is judged before anything resolves it.
    CHECK(holds(roots, "/tmp"));
    #else
    CHECK(holds(roots, "/tmp"));
    CHECK_FALSE(holds(roots, "/dev/shm"));
    CHECK(holds(platformTemporaryFileOnlyRoots(), "/dev/shm"));
    #endif
#endif
}

TEST_CASE("KittyMediaDefaults.platformTemporaryRoots_ignores_a_relative_TMPDIR", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "relative" } } };
    CHECK_FALSE(holds(platformTemporaryRoots(env), "relative"));
}

TEST_CASE("KittyMediaDefaults.mediaPolicy", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/x" } } };
    auto const policy = mediaPolicy(env, claudeDefaults("/home/nobody-here"), "/home/nobody-here");
#ifdef VTBACKEND_KITTY_FILE_MEDIA
    CHECK(policy.transmission == FileTransmission::Enabled);
#else
    CHECK(policy.transmission == FileTransmission::Disabled);
#endif
#ifndef _WIN32
    // A root is kept as spelled and with its links resolved, which differ where a firmlink lies on the way
    // (macOS: /home is /System/Volumes/Data/home); either spelling is the same allow-list entry.
    auto const spelled = std::filesystem::path { "/home/nobody-here/.claude/plugins/data" };
    CHECK(std::ranges::find(policy.allowedRoots, spelled) != policy.allowedRoots.end());
    CHECK(policy.allowedRoots.size() <= 2);
    for (auto const& root: policy.allowedRoots)
        CHECK((root == spelled || root == std::filesystem::weakly_canonical(spelled)));
    CHECK(policy.temporaryRoots == platformTemporaryRoots(env));
    CHECK(policy.temporaryFileOnlyRoots == platformTemporaryFileOnlyRoots());
    CHECK(policy.pathCase == platformPathCase());
    CHECK(policy.sizeLimit == MaxChunkedPayloadSize);
#endif
}

TEST_CASE("KittyMediaDefaults.mediaPolicy_without_a_home", "[kitty][media]")
{
    // The configuration resolves `~/.claude/plugins/data` against no home to a relative path: no root.
    auto const env = core::testing::FakeEnvironment {};
    CHECK(mediaPolicy(env, claudeDefaults(""), "").allowedRoots.empty());
}

TEST_CASE("KittyMediaDefaults.mediaPolicy_follows_the_settings", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment {};
    SECTION("disabled")
    {
        auto const settings =
            MediaSettings { .transmission = FileTransmission::Disabled, .allowList = { "/srv/pictures" } };
        CHECK(mediaPolicy(env, settings, "/home/nobody-here").transmission == FileTransmission::Disabled);
    }
#ifndef _WIN32
    SECTION("an allow-list of its own, a relative entry dropped")
    {
        auto const settings = MediaSettings { .transmission = FileTransmission::Enabled,
                                              .allowList = { "/srv/pictures", "relative/dir" } };
        auto const policy = mediaPolicy(env, settings, "/home/nobody-here");
        CHECK(policy.transmission == FileTransmission::Enabled);
        CHECK(policy.allowedRoots == std::vector<std::filesystem::path> { "/srv/pictures" });
    }
    SECTION("an entry that opens the home directory is dropped")
    {
        auto const settings = MediaSettings { .transmission = FileTransmission::Enabled,
                                              .allowList = { "/home/nobody-here", "/home" } };
        CHECK(mediaPolicy(env, settings, "/home/nobody-here").allowedRoots.empty());
    }
#endif
}

TEST_CASE("KittyMediaDefaults.platformTemporaryRoots_keeps_the_spelling_and_drops_a_trailing_separator",
          "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/x/y/" } } };
    auto const roots = platformTemporaryRoots(env);
#ifdef _WIN32
    CHECK(roots.empty());
#else
    CHECK(holds(roots, "/x/y"));
#endif
}

#ifndef _WIN32
TEST_CASE("KittyMediaDefaults.mediaPolicy_drops_roots_that_open_the_home_directory", "[kitty][media]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-media-test" };
    auto const home = std::filesystem::canonical(temp.path()) / "home";
    std::filesystem::create_directories(home);
    auto const holdsUnder = [](std::vector<std::filesystem::path> const& roots,
                               std::filesystem::path const& dir) {
        return std::ranges::any_of(roots, [&](auto const& root) { return root == dir; });
    };

    SECTION("TMPDIR=/")
    {
        auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/" } } };
        CHECK_FALSE(holdsUnder(mediaPolicy(env, claudeDefaults(home), home).temporaryRoots, "/"));
    }

    SECTION("TMPDIR=$HOME")
    {
        auto const env = core::testing::FakeEnvironment { { { "TMPDIR", home.string() } } };
        CHECK_FALSE(holdsUnder(mediaPolicy(env, claudeDefaults(home), home).temporaryRoots, home));
    }

    SECTION("TMPDIR above $HOME")
    {
        auto const env = core::testing::FakeEnvironment { { { "TMPDIR", home.parent_path().string() } } };
        CHECK_FALSE(
            holdsUnder(mediaPolicy(env, claudeDefaults(home), home).temporaryRoots, home.parent_path()));
    }

    SECTION("plugins/data linked to $HOME")
    {
        std::filesystem::create_directories(home / ".claude/plugins");
        std::filesystem::create_directory_symlink(home, home / ".claude/plugins/data");
        CHECK(
            mediaPolicy(core::testing::FakeEnvironment {}, claudeDefaults(home), home).allowedRoots.empty());
    }

    SECTION("a real plugins/data stays")
    {
        std::filesystem::create_directories(home / ".claude/plugins/data");
        CHECK(mediaPolicy(core::testing::FakeEnvironment {}, claudeDefaults(home), home).allowedRoots
              == std::vector<std::filesystem::path> { home / ".claude/plugins/data" });
    }
}
#endif
