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
#include <vtextension/RootPointer.hpp>

using namespace std::string_view_literals;
using core::platform::testing::InMemoryFileSystem;
using vtextension::grantDigestOf;
using vtextension::Manifest;
using vtextension::ResolvedRoot;
using vtextension::resolveRootPath;
using vtextension::resolveRoots;
using vtextension::RootPointerError;
using vtextension::rootValuesOf;

namespace
{

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
    CHECK(resolveRootPath("~/w", "/home/u", fileSystem) == "/home/u/w");
    CHECK(resolveRootPath("~", "/home/u", fileSystem) == "/home/u");
    CHECK(resolveRootPath("/srv/x/../y", "/home/u", fileSystem) == "/srv/y");
    CHECK(resolveRootPath("~bob/w", "/home/u", fileSystem) == std::unexpected(RootPointerError::NotAbsolute));
    CHECK(resolveRootPath("rel", "/home/u", fileSystem) == std::unexpected(RootPointerError::NotAbsolute));
    CHECK(resolveRootPath("~/w", "", fileSystem) == std::unexpected(RootPointerError::NoHome));
    CHECK(resolveRootPath("~", "", fileSystem) == std::unexpected(RootPointerError::NoHome));
    CHECK(resolveRootPath("/srv/w", "", fileSystem) == "/srv/w");
}

TEST_CASE("RootPointer: every root a manifest declares, resolved", "[daemon][roots]")
{
    auto const fileSystem = InMemoryFileSystem {};
    auto const manifest = orchestratorRoots();

    SECTION("in declaration order")
    {
        CHECK(resolveRoots(manifest, TwoRepos, "/home/u", fileSystem)
              == std::vector<ResolvedRoot> {
                  { .label = "worktrees", .paths = { "/home/u/worktrees" } },
                  { .label = "repos", .paths = { "/home/u/repo/contour", "/srv/other" } },
              });
    }

    SECTION("a worktree root equal to a repository is refused")
    {
        auto const equal = R"({"worktree_root":"/srv/other","repos":[{"path":"/srv/other/"}]})"sv;
        CHECK(resolveRoots(manifest, equal, "/home/u", fileSystem)
              == std::unexpected(RootPointerError::EqualRoots));
    }

    SECTION("a missing key is the root's error")
    {
        CHECK(resolveRoots(manifest, R"({"repos":[]})", "/home/u", fileSystem)
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
