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

#ifdef _WIN32
    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/testing/Junction.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/platform/NativeFileSystem.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <algorithm>
    #include <filesystem>
    #include <format>

    #include <Windows.h>

    #include <vtextension/Manifest.hpp>
    #include <vtextension/RootPointer.hpp>
#endif

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

TEST_CASE("RootResolution: a path outside the root is refused before anything is opened",
          "[daemon][processes]")
{
    // Opening is not harmless: on Windows a UNC path is a connection to its server, which is handed the
    // user's credentials, and on POSIX an automounter or a FUSE mount answers a lookup. So a guest's path is
    // judged as written first.
    struct Row
    {
        PathStyle style;
        std::string_view root;
        std::string_view other;
        std::string_view outside;
        std::string_view inOther;
    };
    for (auto const& row: std::array {
             Row { .style = PathStyle::Windows,
                   .root = R"(C:\wt)",
                   .other = R"(C:\repos)",
                   .outside = R"(\\attacker\s\x)",
                   .inOther = R"(C:\repos\x)" },
             Row { .style = PathStyle::Posix,
                   .root = "/w",
                   .other = "/r",
                   .outside = "/net/attacker/x",
                   .inOther = "/r/x" },
         })
    {
        INFO(row.outside);
        auto const roots = std::vector<ResolvedRoot> {
            { .label = "worktrees", .paths = { std::string { row.root } } },
            { .label = "repos", .paths = { std::string { row.other } } },
        };
        auto opener = FakeDirectoryOpener { row.style };
        opener.add(std::string { row.root });
        opener.add(std::string { row.other });
        opener.add(std::string { row.outside });
        opener.add(std::string { row.inOther });

        CHECK(vtextension::openInRoot(opener, roots, "worktrees", row.outside)
              == std::unexpected(RootError::OutsideRoots));
        CHECK(vtextension::resolvePathArgument(opener, roots, "worktrees", row.outside)
              == std::unexpected(RootError::OutsideRoots));
        CHECK(vtextension::createPathParents(opener, roots, "worktrees", row.outside)
              == std::unexpected(RootError::OutsideRoots));
        CHECK(vtextension::openInRoot(opener, roots, "worktrees", row.inOther)
              == std::unexpected(RootError::NotItsRoot));
        CHECK(vtextension::resolvePathArgument(opener, roots, "worktrees", row.inOther)
              == std::unexpected(RootError::NotItsRoot));
        CHECK(vtextension::openInRoot(opener, roots, "unknown", row.outside)
              == std::unexpected(RootError::OutsideRoots));
        CHECK(opener.opened.empty());
        CHECK(opener.created.empty());
    }
}

#ifdef _WIN32
namespace
{

/// @return @p text with every ASCII letter upper-cased.
[[nodiscard]] std::string asciiUpper(std::string text)
{
    std::ranges::transform(text, text.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    return text;
}

/// @return @p text with every ASCII letter lower-cased.
[[nodiscard]] std::string asciiLower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return text;
}

/// @return @p text as the body of a JSON string: every `\` and `"` escaped.
[[nodiscard]] std::string jsonEscaped(std::string_view text)
{
    auto escaped = std::string {};
    for (auto const c: text)
    {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    return escaped;
}

/// @return @p path's 8.3 spelling; @p path itself when it has none.
[[nodiscard]] std::filesystem::path shortPathOf(std::filesystem::path const& path)
{
    auto shortName = std::wstring(MAX_PATH, L'\0');
    auto const length =
        GetShortPathNameW(path.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    if (length == 0 || length >= shortName.size())
        return path;
    shortName.resize(length);
    return std::filesystem::path { shortName };
}

} // namespace

TEST_CASE("RootResolution: a Windows root, end to end through the native opener", "[daemon][processes]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-a2-roots" };
    auto const& top = directory.path();
    std::filesystem::create_directories(top / "repos" / "contour");
    std::filesystem::create_directories(top / "elsewhere");
    REQUIRE(vtpty::testing::createJunction(top / "repos" / "contour" / "out", top / "elsewhere").has_value());

    auto const manifest =
        vtextension::parseManifestText("name = orchestrator\nversion = 2\nplacement daemon\n"
                                       "root worktrees create = config:worktree_root\n"
                                       "root repos = config:repos/*/path\n");
    REQUIRE(manifest.has_value());
    auto slashed = asciiLower(core::pathToUtf8(top));
    std::ranges::replace(slashed, '\\', '/');
    auto const config = std::format(R"({{"worktree_root":"{}/wt","repos":[{{"path":"{}"}}]}})",
                                    jsonEscaped(slashed),
                                    jsonEscaped(core::pathToUtf8(top / "repos" / "contour")));
    auto const roots = vtextension::resolveRoots(
        *manifest, config, "", core::platform::NativeFileSystem::instance(), PathStyle::Windows);
    REQUIRE(roots.has_value());
    REQUIRE(roots->size() == 2);
    auto const& wt = roots->at(0).paths.at(0);
    auto const& contour = roots->at(1).paths.at(0);
    INFO("worktrees: " << wt << ", repos: " << contour);
    auto& opener = vtpty::nativeDirectoryOpener();
    REQUIRE(opener.pathStyle() == PathStyle::Windows);

    CHECK(vtextension::ensureRoot(opener, *roots, "worktrees").has_value());
    CHECK(std::filesystem::is_directory(top / "wt"));

    auto const opened = vtextension::openInRoot(opener, *roots, "repos", asciiUpper(contour));
    REQUIRE(opened.has_value());
    CHECK(opened->resolvedPath() == contour);

    CHECK(vtextension::openInRoot(opener, *roots, "repos", contour + R"(\out)")
          == std::unexpected(RootError::OutsideRoots));

    auto const run = vtextension::createPathParents(opener, *roots, "worktrees", wt + R"(\contour\r1)");
    REQUIRE(run.has_value());
    CHECK(*run == wt + R"(\contour\r1)");
    CHECK_FALSE(run->contains('/'));
    CHECK(std::filesystem::is_directory(core::pathFromTrustedUtf8(*run).parent_path()));

    auto wtSlashed = wt;
    std::ranges::replace(wtSlashed, '\\', '/');
    CHECK(vtextension::resolvePathArgument(opener, *roots, "worktrees", wtSlashed)
          == std::unexpected(RootError::NotAbsolute));
    CHECK(vtextension::resolvePathArgument(opener, *roots, "worktrees", asciiUpper(wt))
          == std::unexpected(RootError::OutsideRoots));

    // A root is resolved to its long names, and so is a directory named beneath it by an 8.3 name. The root
    // itself spelled by its short name is not beneath it as written, and is refused before it is opened.
    CHECK_FALSE(wt.contains('~'));
    CHECK_FALSE(contour.contains('~'));
    auto const longLeaf = top / "repos" / "contour" / "a-long-directory-name";
    std::filesystem::create_directories(longLeaf);
    auto const shortLeaf = core::pathToUtf8(shortPathOf(longLeaf).filename());
    if (!shortLeaf.contains('~'))
        SKIP("8.3 names are disabled on this volume");
    auto const throughShort = vtextension::openInRoot(opener, *roots, "repos", contour + R"(\)" + shortLeaf);
    INFO("through " << shortLeaf);
    REQUIRE(throughShort.has_value());
    CHECK(throughShort->resolvedPath() == contour + R"(\a-long-directory-name)");
    CHECK_FALSE(throughShort->resolvedPath().contains('~'));

    auto const shortContour = core::pathToUtf8(shortPathOf(top / "repos" / "contour"));
    if (shortContour.contains('~'))
        CHECK(vtextension::openInRoot(opener, *roots, "repos", shortContour)
              == std::unexpected(RootError::OutsideRoots));
}
#endif
