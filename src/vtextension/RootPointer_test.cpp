// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/RootPathSyntax.hpp>
#include <vtextension/RootPointer.hpp>
#include <vtextension/UserSettableEnvironment.hpp>

#ifdef _WIN32
    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/testing/Junction.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/platform/NativeFileSystem.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <algorithm>
    #include <filesystem>

    #include <Windows.h>
#endif

using namespace std::string_view_literals;
using core::platform::testing::InMemoryFileSystem;
using vtextension::EnvironmentSet;
using vtextension::grantDigestOf;
using vtextension::Manifest;
using vtextension::ResolvedRoot;
using vtextension::resolveRootPath;
using vtextension::resolveRoots;
using vtextension::RootPointerError;
using vtextension::rootSpellingOf;
using vtextension::rootValuesOf;

namespace
{

constexpr auto Posix = vtpty::PathStyle::Posix;
constexpr auto Windows = vtpty::PathStyle::Windows;

/// One configured directory, and how rootSpellingOf() spells it.
struct SpellingRow
{
    std::string_view raw;                                 ///< The directory as configured.
    std::expected<std::string, RootPointerError> spelled; ///< Its spelling, or why it has none.
};

/// The orchestrator's configuration, as the host passes it: a worktree root and two repositories.
constexpr auto TwoRepos = R"({"worktree_root":"~/worktrees","max_agents":4,
    "repos":[{"path":"~/repo/contour","forge":"github"},{"path":"/srv/other"}]})"sv;

/// @return A manifest whose roots are the orchestrator's.
[[nodiscard]] Manifest orchestratorRoots()
{
    auto manifest = vtextension::parseManifestText("name = orchestrator\nversion = 2\nplacement daemon\n"
                                                   "root worktrees create = config:worktree_root\n"
                                                   "root repos = config:repos/*/path\n");
    REQUIRE(manifest.has_value());
    return *manifest;
}

} // namespace

TEST_CASE("RootPointer: the values a pointer names", "[daemon][roots]")
{
    struct Row
    {
        std::string_view json;
        std::string_view pointer;
        std::expected<std::vector<std::string>, RootPointerError> values;
    };
    for (auto const& row: std::array {
             Row { .json = TwoRepos,
                   .pointer = "worktree_root",
                   .values = std::vector<std::string> { "~/worktrees" } },
             Row { .json = TwoRepos,
                   .pointer = "repos/*/path",
                   .values = std::vector<std::string> { "~/repo/contour", "/srv/other" } },
             Row { .json = TwoRepos,
                   .pointer = "nowhere",
                   .values = std::unexpected(RootPointerError::MissingKey) },
             Row { .json = TwoRepos,
                   .pointer = "max_agents",
                   .values = std::unexpected(RootPointerError::NotAString) },
             Row { .json = TwoRepos,
                   .pointer = "worktree_root/*",
                   .values = std::unexpected(RootPointerError::StarOverNonList) },
             Row { .json = R"({"repos":{}})",
                   .pointer = "repos/*/path",
                   .values = std::unexpected(RootPointerError::StarOverNonList) },
             Row { .json = TwoRepos, .pointer = "", .values = std::unexpected(RootPointerError::BadPointer) },
             Row { .json = TwoRepos,
                   .pointer = "repos//path",
                   .values = std::unexpected(RootPointerError::BadPointer) },
             Row { .json = "{",
                   .pointer = "worktree_root",
                   .values = std::unexpected(RootPointerError::NotJson) },
         })
    {
        INFO(row.pointer);
        CHECK(rootValuesOf(row.json, row.pointer) == row.values);
    }
}

TEST_CASE("RootPointer: a configured directory is expanded and made absolute", "[daemon][roots]")
{
    auto const fileSystem = InMemoryFileSystem {};
    CHECK(resolveRootPath("~/w", "/home/u", fileSystem, Posix) == "/home/u/w");
    CHECK(resolveRootPath("~", "/home/u", fileSystem, Posix) == "/home/u");
    CHECK(resolveRootPath("/srv/x/../y", "/home/u", fileSystem, Posix) == "/srv/y");
    CHECK(resolveRootPath("~bob/w", "/home/u", fileSystem, Posix)
          == std::unexpected(RootPointerError::NotAbsolute));
    CHECK(resolveRootPath("rel", "/home/u", fileSystem, Posix)
          == std::unexpected(RootPointerError::NotAbsolute));
    CHECK(resolveRootPath("~/w", "", fileSystem, Posix) == std::unexpected(RootPointerError::NoHome));
    CHECK(resolveRootPath("~", "", fileSystem, Posix) == std::unexpected(RootPointerError::NoHome));
    CHECK(resolveRootPath("/srv/w", "", fileSystem, Posix) == "/srv/w");
}

TEST_CASE("RootPointer: a configured Windows directory is spelled once", "[daemon][roots]")
{
    using enum RootPointerError;
    auto const rows = std::to_array<SpellingRow>({
        { .raw = R"(C:\Users\me\repos)", .spelled = R"(C:\Users\me\repos)" },
        { .raw = R"(c:/Users/me/repos)", .spelled = R"(C:\Users\me\repos)" },
        { .raw = R"(C:\repos\)", .spelled = R"(C:\repos)" },
        { .raw = R"(C:\)", .spelled = R"(C:\)" },
        { .raw = R"(C:/)", .spelled = R"(C:\)" },
        { .raw = R"(c:\)", .spelled = R"(C:\)" },
        { .raw = R"(\\srv\share\r)", .spelled = R"(\\srv\share\r)" },
        { .raw = R"(//srv/share/r)", .spelled = R"(\\srv\share\r)" },
        { .raw = R"(\\srv\share)", .spelled = R"(\\srv\share)" },
        { .raw = R"(~)", .spelled = R"(C:\Users\me)" },
        { .raw = R"(~\w)", .spelled = R"(C:\Users\me\w)" },
        { .raw = R"(~/w)", .spelled = R"(C:\Users\me\w)" },
        { .raw = R"(C:x)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(\x)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(x)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(\\srv)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(\\srv\)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(~bob\w)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(\\?\C:\x)", .spelled = std::unexpected(DevicePath) },
        { .raw = R"(//?/C:/x)", .spelled = std::unexpected(DevicePath) },
        { .raw = R"(\\.\pipe\x)", .spelled = std::unexpected(DevicePath) },
        { .raw = R"(\??\C:\x)", .spelled = std::unexpected(DevicePath) },
        { .raw = R"(C:\a\..\b)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\a\.\b)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\a\\b)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\a:b)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\repos.)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\repos )", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\con)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(C:\a|b)", .spelled = std::unexpected(NotPlainName) },
        { .raw = R"(\\\x)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(\\srv\\x)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(\\srv\share\)", .spelled = R"(\\srv\share)" },
        { .raw = R"(C:\\)", .spelled = R"(C:\)" },
        { .raw = R"(C:\a\\)", .spelled = R"(C:\a)" },
        { .raw = R"(//./pipe/x)", .spelled = std::unexpected(DevicePath) },
        { .raw = R"(/??/C:/x)", .spelled = std::unexpected(DevicePath) },
    });
    for (auto const& row: rows)
    {
        INFO(row.raw);
        CHECK(rootSpellingOf(row.raw, R"(C:\Users\me)", Windows) == row.spelled);
    }
    CHECK(rootSpellingOf(R"(~\w)", "", Windows) == std::unexpected(NoHome));

    // No longer than a root's paths are compared at: a spelling the comparison rule refuses is no root.
    auto const longest = std::string(R"(C:\)") + std::string(vtextension::MaxPathArgumentBytes - 3, 'a');
    CHECK(rootSpellingOf(longest, "", Windows) == longest);
    CHECK(rootSpellingOf(longest + "a", "", Windows) == std::unexpected(NotAbsolute));
}

TEST_CASE("RootPointer: a configured POSIX directory is spelled as before", "[daemon][roots]")
{
    using enum RootPointerError;
    auto const rows = std::to_array<SpellingRow>({
        { .raw = "/srv/x/../y", .spelled = "/srv/x/../y" },
        { .raw = "~/w", .spelled = "/home/u/w" },
        { .raw = R"(C:\x)", .spelled = std::unexpected(NotAbsolute) },
        { .raw = R"(~\w)", .spelled = std::unexpected(NotAbsolute) },
    });
    for (auto const& row: rows)
    {
        INFO(row.raw);
        CHECK(rootSpellingOf(row.raw, "/home/u", Posix) == row.spelled);
    }
}

#ifdef _WIN32
TEST_CASE("RootPointer: a Windows root resolves to the spelling its handle reads back", "[daemon][roots]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-a2" };
    auto const longPath = directory.path() / "Long Directory Name" / "inner";
    std::filesystem::create_directories(longPath);
    auto const& fileSystem = core::platform::NativeFileSystem::instance();
    auto const opened = vtpty::nativeDirectoryOpener().open(core::pathToUtf8(longPath));
    REQUIRE(opened.has_value());
    auto const& expected = opened->resolvedPath();
    auto const resolve = [&fileSystem](std::string const& spelling) {
        return resolveRootPath(spelling, "", fileSystem, Windows);
    };

    SECTION("upper-cased")
    {
        auto spelling = core::pathToUtf8(longPath);
        std::ranges::transform(spelling, spelling.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
        CHECK(resolve(spelling) == expected);
    }

    SECTION("separated by /")
    {
        auto spelling = core::pathToUtf8(longPath);
        std::ranges::replace(spelling, '\\', '/');
        CHECK(resolve(spelling) == expected);
    }

    SECTION("through a junction to it")
    {
        auto const junction = directory.path() / "junction";
        REQUIRE(vtpty::testing::createJunction(junction, longPath).has_value());
        CHECK(resolve(core::pathToUtf8(junction)) == expected);
    }

    SECTION("through its 8.3 name")
    {
        auto shortName = std::wstring(MAX_PATH, L'\0');
        auto const length =
            GetShortPathNameW(longPath.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
        REQUIRE(length > 0);
        REQUIRE(length < shortName.size());
        shortName.resize(length);
        if (std::filesystem::path { shortName } == longPath)
            WARN("8.3 names disabled");
        else
            CHECK(resolve(core::pathToUtf8(std::filesystem::path { shortName })) == expected);
    }

    SECTION("a tail that does not exist yet keeps its spelling")
    {
        auto const resolved = resolve(core::pathToUtf8(longPath) + R"(\New\Sub)");
        REQUIRE(resolved.has_value());
        CHECK(resolved->ends_with(R"(\New\Sub)"));
        CHECK(*resolved == expected + R"(\New\Sub)");
    }
}
#endif

TEST_CASE("RootPointer: every root a manifest declares, resolved", "[daemon][roots]")
{
    auto const fileSystem = InMemoryFileSystem {};
    auto const manifest = orchestratorRoots();

    SECTION("in declaration order")
    {
        CHECK(resolveRoots(manifest, TwoRepos, "/home/u", fileSystem, Posix)
              == std::vector<ResolvedRoot> {
                  { .label = "worktrees", .paths = { "/home/u/worktrees" } },
                  { .label = "repos", .paths = { "/home/u/repo/contour", "/srv/other" } },
              });
    }

    SECTION("a worktree root equal to a repository is refused")
    {
        auto const equal = R"({"worktree_root":"/srv/other","repos":[{"path":"/srv/other/"}]})"sv;
        CHECK(resolveRoots(manifest, equal, "/home/u", fileSystem, Posix)
              == std::unexpected(RootPointerError::EqualRoots));
    }

    SECTION("a missing key is the root's error")
    {
        CHECK(resolveRoots(manifest, R"({"repos":[]})", "/home/u", fileSystem, Posix)
              == std::unexpected(RootPointerError::MissingKey));
    }
}

TEST_CASE("RootPointer: the grant digest names the manifest and every directory", "[daemon][roots]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/w" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    auto const digest = grantDigestOf("m1", roots);
    CHECK(digest.size() == 64);
    CHECK(grantDigestOf("m1", roots) == digest);
    CHECK(grantDigestOf("m2", roots) != digest);

    auto moved = roots;
    moved[1].paths[0] = "/r/elsewhere";
    CHECK(grantDigestOf("m1", moved) != digest);

    auto added = roots;
    added[1].paths.emplace_back("/r/other");
    CHECK(grantDigestOf("m1", added) != digest);
}

TEST_CASE("RootPointer: the grant digest names every set, and only when there is one",
          "[daemon][roots][sets]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/w" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    // No set: the digest from before sets existed, so that a grant given then still holds.
    auto const withoutSets = grantDigestOf("m1", roots);
    CHECK(grantDigestOf("m1", roots, {}) == withoutSets);
    CHECK(withoutSets == "0fe97c7b408b8cae0b59b9ec7e6789deeaec6a959774a4d15fef800995e2f165");

    auto const work =
        EnvironmentSet { .name = "work", .variables = { { .name = "CLAUDE_CONFIG_DIR", .value = "/a" } } };
    auto const other =
        EnvironmentSet { .name = "other", .variables = { { .name = "NO_PROXY", .value = "localhost" } } };
    auto const oneSet = std::vector { work };
    auto const withSet = grantDigestOf("m1", roots, oneSet);
    CHECK(withSet != withoutSets);
    CHECK(grantDigestOf("m1", roots, oneSet) == withSet);

    auto changed = oneSet;
    changed[0].variables[0].value = "/b";
    CHECK(grantDigestOf("m1", roots, changed) != withSet);

    auto const workFirst = std::vector { work, other };
    auto const otherFirst = std::vector { other, work };
    CHECK(grantDigestOf("m1", roots, workFirst) != withSet);
    CHECK(grantDigestOf("m1", roots, workFirst) != grantDigestOf("m1", roots, otherFirst));
}
