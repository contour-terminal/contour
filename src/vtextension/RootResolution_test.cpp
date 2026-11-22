// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/RootResolution.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>

using namespace std::string_view_literals;
using vtextension::mostSpecificRoot;
using vtextension::ResolvedRoot;
using vtextension::RootError;
using vtextension::testing::FakeDirectoryOpener;

TEST_CASE("RootResolution: most specific root", "[daemon][processes]")
{
    auto const flat = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/w" } },
        { .label = "repos", .paths = { "/r/contour", "/r/other" } },
    };
    struct Row
    {
        std::string_view path;
        std::optional<std::string_view> root;
    };
    for (auto const& row: std::array {
             Row { .path = "/w", .root = "worktrees"sv },
             Row { .path = "/w/a", .root = "worktrees"sv },
             Row { .path = "/wx", .root = std::nullopt },
             Row { .path = "/r/other/x/y", .root = "repos"sv },
             Row { .path = "/r", .root = std::nullopt },
             Row { .path = "/", .root = std::nullopt },
         })
    {
        INFO(row.path);
        CHECK(mostSpecificRoot(flat, row.path) == row.root);
    }

    auto const nested = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/r/contour/.claude/worktrees" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    CHECK(mostSpecificRoot(nested, "/r/contour/.claude/worktrees/a") == "worktrees"sv);
    CHECK(mostSpecificRoot(nested, "/r/contour/.claude/worktreesx") == "repos"sv);
    CHECK(mostSpecificRoot(nested, "/r/contour/.claude") == "repos"sv);
}

TEST_CASE("RootResolution: equal roots are refused", "[daemon][processes]")
{
    auto const equal = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/r/contour" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    CHECK(vtextension::checkRootsDistinct(equal) == std::unexpected(RootError::EqualRoots));

    auto const distinct = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/r/contour/wt" } },
        { .label = "repos", .paths = { "/r/contour", "/r/other" } },
    };
    CHECK(vtextension::checkRootsDistinct(distinct).has_value());
}

TEST_CASE("RootResolution: a directory is judged by its resolved path", "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/w" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    auto opener = FakeDirectoryOpener {};
    opener.add("/w/a");
    opener.link("/w/link", "/etc");
    opener.add("/r/contour");

    CHECK(vtextension::openInRoot(opener, roots, "worktrees", "/w/a")->resolvedPath() == "/w/a");
    CHECK(vtextension::openInRoot(opener, roots, "worktrees", "rel").error() == RootError::NotAbsolute);
    CHECK(vtextension::openInRoot(opener, roots, "worktrees", "/w/../etc").error()
          == RootError::Unnormalized);
    CHECK(vtextension::openInRoot(opener, roots, "worktrees", "/w/link").error() == RootError::OutsideRoots);
    CHECK(vtextension::openInRoot(opener, roots, "worktrees", "/r/contour").error() == RootError::NotItsRoot);
    CHECK(vtextension::openInRoot(opener, roots, "worktrees", "/w/gone").error() == RootError::Missing);
}
