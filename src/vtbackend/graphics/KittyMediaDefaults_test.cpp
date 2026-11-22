// SPDX-License-Identifier: Apache-2.0
//
// The media policy a composition root grants local sessions from the configuration's settings.

#include <vtbackend/graphics/KittyMediaDefaults.hpp>

#include <core/Utf8Path.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#ifdef _WIN32
    #include <Windows.h>
#endif

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

/// The variable a platform names its temp directory by first: `%TMP%` on Windows, `$TMPDIR` elsewhere.
#ifdef _WIN32
constexpr auto TempVariable = std::string_view { "TMP" };
#else
constexpr auto TempVariable = std::string_view { "TMPDIR" };
#endif

/// @return An environment whose temp directory is @p directory.
[[nodiscard]] core::testing::FakeEnvironment tempAt(std::filesystem::path const& directory)
{
    return core::testing::FakeEnvironment { { { std::string { TempVariable },
                                                core::pathToUtf8(directory) } } };
}
} // namespace

TEST_CASE("KittyMediaDefaults.platformTemporaryRoots", "[kitty][media]")
{
    auto const env = core::testing::FakeEnvironment { { { "TMPDIR", "/x" } } };
    auto const roots = platformTemporaryRoots(env);
#ifdef _WIN32
    // `$TMPDIR` is an MSYS or WSL spelling, a POSIX path that is not absolute here.
    CHECK_FALSE(holds(roots, "/x"));
    auto const windows = core::testing::FakeEnvironment {
        { { "TMP", "C:\\t1" }, { "TEMP", "D:/t2/" }, { "LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local" } }
    };
    auto const spelled = platformTemporaryRoots(windows);
    CHECK(holds(spelled, "C:\\t1"));
    CHECK(holds(spelled, "D:\\t2"));
    CHECK(holds(spelled, "C:\\Users\\u\\AppData\\Local\\Temp"));
    CHECK(platformTemporaryFileOnlyRoots().empty());
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
#ifdef _WIN32
    // `~/.claude/plugins/data` under `%USERPROFILE%`; a POSIX spelling is no absolute path here.
    CHECK(policy.allowedRoots.empty());
    auto const home = std::filesystem::path { "C:\\Users\\nobody-here" };
    auto const windows = mediaPolicy(env, claudeDefaults(home), home);
    CHECK(holds(windows.allowedRoots, home / ".claude" / "plugins" / "data"));
    CHECK(windows.pathCase == PathCase::Insensitive);
#else
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
    CHECK(holds(platformTemporaryRoots(core::testing::FakeEnvironment { { { "TEMP", "C:\\x\\y\\" } } }),
                "C:\\x\\y"));
#else
    CHECK(holds(roots, "/x/y"));
#endif
}

TEST_CASE("KittyMediaDefaults.mediaPolicy_drops_roots_that_open_the_home_directory", "[kitty][media]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-media-test" };
    auto const home = std::filesystem::canonical(temp.path()) / "home";
    std::filesystem::create_directories(home);
    auto const holdsUnder = [](std::vector<std::filesystem::path> const& roots,
                               std::filesystem::path const& dir) {
        return std::ranges::any_of(roots, [&](auto const& root) { return root == dir; });
    };

    SECTION("the temp directory at the root")
    {
        auto const env = tempAt(home.root_path());
        CHECK_FALSE(
            holdsUnder(mediaPolicy(env, claudeDefaults(home), home).temporaryRoots, home.root_path()));
    }

    SECTION("the temp directory at home")
    {
        CHECK_FALSE(holdsUnder(mediaPolicy(tempAt(home), claudeDefaults(home), home).temporaryRoots, home));
    }

    SECTION("the temp directory above home")
    {
        CHECK_FALSE(
            holdsUnder(mediaPolicy(tempAt(home.parent_path()), claudeDefaults(home), home).temporaryRoots,
                       home.parent_path()));
    }

#ifndef _WIN32
    SECTION("plugins/data linked to $HOME")
    {
        std::filesystem::create_directories(home / ".claude/plugins");
        std::filesystem::create_directory_symlink(home, home / ".claude/plugins/data");
        CHECK(
            mediaPolicy(core::testing::FakeEnvironment {}, claudeDefaults(home), home).allowedRoots.empty());
    }
#endif

    SECTION("a real plugins/data stays")
    {
        std::filesystem::create_directories(home / ".claude/plugins/data");
        CHECK(mediaPolicy(core::testing::FakeEnvironment {}, claudeDefaults(home), home).allowedRoots
              == std::vector<std::filesystem::path> { (home / ".claude/plugins/data").lexically_normal() });
    }
}

#ifdef _WIN32
TEST_CASE("KittyMediaDefaults.platformTemporaryRoots_adds_the_long_form_of_a_short_spelling",
          "[kitty][media][win32-shortnames]")
{
    // `%TEMP%` is often spelled in 8.3 names, and programs name their files by it; the reader judges the
    // final path, in long names. Both are roots.
    auto const temp = core::testing::ScopedTempDir { "contour-media-test-long-directory-name" };
    auto const canonical = std::filesystem::canonical(temp.path());
    auto spelling = std::wstring(MAX_PATH, L'\0');
    auto const length =
        GetShortPathNameW(canonical.c_str(), spelling.data(), static_cast<DWORD>(spelling.size()));
    REQUIRE(length > 0);
    spelling.resize(length);
    auto const shortSpelling = std::filesystem::path { spelling };
    if (shortSpelling == canonical)
        SKIP("the volume of the temp directory makes no short (8.3) names");

    auto const roots = platformTemporaryRoots(tempAt(shortSpelling));
    CHECK(holds(roots, shortSpelling));
    CHECK(holds(roots, canonical));
}
#endif
