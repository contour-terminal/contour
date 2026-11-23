// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PathStyle.hpp>

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
using vtpty::PathStyle;

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
        CHECK(mostSpecificRoot(flat, row.path, PathStyle::Posix) == row.root);
    }

    auto const nested = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/r/contour/.claude/worktrees" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    CHECK(mostSpecificRoot(nested, "/r/contour/.claude/worktrees/a", PathStyle::Posix) == "worktrees"sv);
    CHECK(mostSpecificRoot(nested, "/r/contour/.claude/worktreesx", PathStyle::Posix) == "repos"sv);
    CHECK(mostSpecificRoot(nested, "/r/contour/.claude", PathStyle::Posix) == "repos"sv);
}

TEST_CASE("RootResolution: equal roots are refused", "[daemon][processes]")
{
    auto const equal = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/r/contour" } },
        { .label = "repos", .paths = { "/r/contour" } },
    };
    CHECK(vtextension::checkRootsDistinct(equal, PathStyle::Posix) == std::unexpected(RootError::EqualRoots));

    auto const distinct = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { "/r/contour/wt" } },
        { .label = "repos", .paths = { "/r/contour", "/r/other" } },
    };
    CHECK(vtextension::checkRootsDistinct(distinct, PathStyle::Posix).has_value());
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

TEST_CASE("RootResolution: Windows most specific root, case-insensitively", "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { R"(C:\w)" } },
        { .label = "repos", .paths = { R"(C:\r\contour)", R"(\\srv\share\r)" } },
    };
    struct Row
    {
        std::string_view path;
        std::optional<std::string_view> root;
    };
    for (auto const& row: std::array {
             Row { .path = R"(c:\W\a)", .root = "worktrees"sv },
             Row { .path = R"(C:\wx)", .root = std::nullopt },
             Row { .path = R"(D:\w)", .root = std::nullopt },
             Row { .path = R"(\\SRV\Share\r\x)", .root = "repos"sv },
             Row { .path = R"(\\srv\other\r)", .root = std::nullopt },
             Row { .path = R"(C:\)", .root = std::nullopt },
         })
    {
        INFO(row.path);
        CHECK(mostSpecificRoot(roots, row.path, PathStyle::Windows) == row.root);
    }
}

TEST_CASE("RootResolution: Windows roots equal but for case are refused", "[daemon][processes]")
{
    auto const windows = std::vector<ResolvedRoot> {
        { .label = "a", .paths = { R"(C:\WT)" } },
        { .label = "b", .paths = { R"(c:\wt)" } },
    };
    CHECK(vtextension::checkRootsDistinct(windows, PathStyle::Windows)
          == std::unexpected(RootError::EqualRoots));

    auto const posix = std::vector<ResolvedRoot> {
        { .label = "a", .paths = { "/WT" } },
        { .label = "b", .paths = { "/wt" } },
    };
    CHECK(vtextension::checkRootsDistinct(posix, PathStyle::Posix).has_value());
}

TEST_CASE("RootResolution: a Windows path argument naming the root itself in another case is refused",
          "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { R"(C:\wt\Work)" } },
    };
    auto opener = FakeDirectoryOpener { PathStyle::Windows };
    opener.add(R"(C:\wt)");
    CHECK(vtextension::resolvePathArgument(opener, roots, "worktrees", R"(C:\wt\work)")
          == std::unexpected(RootError::OutsideRoots));

    opener.add(R"(C:\wt\Work)");
    CHECK(vtextension::resolvePathArgument(opener, roots, "worktrees", R"(C:\WT\WORK)")
          == std::unexpected(RootError::OutsideRoots));
}

TEST_CASE("RootResolution: a Windows create root is made beneath its drive", "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { R"(C:\wt\a\b)" } },
    };
    auto opener = FakeDirectoryOpener { PathStyle::Windows };
    opener.add(R"(C:\wt)");
    CHECK(vtextension::ensureRoot(opener, roots, "worktrees").has_value());
    CHECK(opener.created == std::vector<std::string> { R"(C:\wt\a)", R"(C:\wt\a\b)" });
}

TEST_CASE("RootResolution: Windows path parents join with backslashes", "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { R"(C:\wt)" } },
    };
    auto opener = FakeDirectoryOpener { PathStyle::Windows };
    opener.add(R"(C:\wt)");
    CHECK(vtextension::createPathParents(opener, roots, "worktrees", R"(C:\wt\contour\run)")
          == std::string { R"(C:\wt\contour\run)" });
    CHECK(opener.created == std::vector<std::string> { R"(C:\wt\contour)" });
}

TEST_CASE("RootResolution: Windows syntax refusals", "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { R"(C:\wt)" } },
    };
    auto opener = FakeDirectoryOpener { PathStyle::Windows };
    opener.add(R"(C:\wt)");
    struct Row
    {
        std::string_view path;
        RootError error;
    };
    for (auto const& row: std::array {
             Row { .path = R"(C:wt)", .error = RootError::NotAbsolute },
             Row { .path = R"(\wt)", .error = RootError::NotAbsolute },
             Row { .path = R"(C:/wt/x)", .error = RootError::NotAbsolute },
             Row { .path = R"(/wt)", .error = RootError::NotAbsolute },
             Row { .path = R"(C:\wt\..\x)", .error = RootError::Unnormalized },
             Row { .path = R"(C:\wt\x:y)", .error = RootError::Unnormalized },
             Row { .path = R"(C:\wt\con)", .error = RootError::Unnormalized },
             Row { .path = R"(C:\wt\x.)", .error = RootError::Unnormalized },
             Row { .path = R"(C:\wt\)", .error = RootError::Unnormalized },
             Row { .path = R"(\\?\C:\wt)", .error = RootError::Unnormalized },
         })
    {
        INFO(row.path);
        CHECK(vtextension::openInRoot(opener, roots, "worktrees", row.path) == std::unexpected(row.error));
    }
}

TEST_CASE("RootResolution: a Windows directory is judged by its resolved path", "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> {
        { .label = "worktrees", .paths = { R"(C:\wt)" } },
    };
    auto opener = FakeDirectoryOpener { PathStyle::Windows };
    opener.add(R"(C:\wt\a)");
    opener.link(R"(C:\wt\link)", R"(D:\x)");

    CHECK(vtextension::openInRoot(opener, roots, "worktrees", R"(C:\wt\link)").error()
          == RootError::OutsideRoots);
    auto const opened = vtextension::openInRoot(opener, roots, "worktrees", R"(c:\wt\A)");
    REQUIRE(opened.has_value());
    CHECK(opened->resolvedPath() == R"(C:\wt\a)");
}
