// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/PathResolution.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using core::platform::testing::InMemoryFileSystem;

namespace
{
/// An absolute path here. On Windows a root directory without a drive is relative to the current drive.
#ifdef _WIN32
constexpr auto AbsoluteHosts = std::string_view { "C:/etc/hosts" };
#else
constexpr auto AbsoluteHosts = std::string_view { "/etc/hosts" };
#endif
} // namespace

TEST_CASE("PathResolution.candidates", "[links]")
{
    auto const bases = std::array { std::string { "/a" }, std::string { "/b" } };
    CHECK(pathCandidates(bases, "/home/u", "x.txt").paths
          == std::vector<std::filesystem::path> { "/a/x.txt", "/b/x.txt" });
    CHECK(pathCandidates(bases, "/home/u", "~/n.md").paths
          == std::vector<std::filesystem::path> { "/home/u/n.md" });
    CHECK(pathCandidates(bases, "", "~/n.md").paths.empty());
    CHECK(pathCandidates(bases, "/home/u", AbsoluteHosts).paths
          == std::vector<std::filesystem::path> { std::filesystem::path { AbsoluteHosts } });
    CHECK(pathCandidates({}, "/home/u", "x.txt").paths.empty());
    CHECK(pathCandidates(bases, "/home/u", "../c/./y").paths
          == std::vector<std::filesystem::path> { "/c/y", "/c/y" });
}

TEST_CASE("PathResolution.candidates.withoutBases", "[links]")
{
    // An absolute and a `~/` path name one place whatever the command's directories were.
    CHECK(pathCandidates({}, "/home/u", AbsoluteHosts).paths
          == std::vector<std::filesystem::path> { std::filesystem::path { AbsoluteHosts } });
    CHECK(pathCandidates({}, "/home/u", "~/n.md").paths
          == std::vector<std::filesystem::path> { "/home/u/n.md" });
}

TEST_CASE("PathResolution.candidates.nameTheBaseEachWasJoinedTo", "[links]")
{
    // A base that yields no candidate (an empty one; on Windows one that is not UTF-8) is skipped, so a
    // candidate's index is not its base's: the directory travels with the candidate.
    auto const bases = std::array { std::string {}, std::string { "/w" } };
    auto const relative = pathCandidates(bases, "/home/u", "x.txt");
    CHECK(relative.paths == std::vector<std::filesystem::path> { "/w/x.txt" });
    CHECK(relative.directories == std::vector<std::string> { "/w" });

    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/w/x.txt", "");
    auto const found = resolveLocalPath(fileSystem, relative.paths);
    REQUIRE(found);
    CHECK(relative.directories[found->candidateIndex] == "/w");

    // An absolute and a `~/` path were joined to no base.
    CHECK(pathCandidates(bases, "/home/u", AbsoluteHosts).directories == std::vector<std::string> { "" });
    CHECK(pathCandidates(bases, "/home/u", "~/n.md").directories == std::vector<std::string> { "" });
    CHECK(pathCandidates(bases, "", "~/n.md").directories.empty());
}

TEST_CASE("PathResolution.firstExistingWins", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/b/x.txt", "");
    fileSystem.addDirectory("/a/src");

    auto const both =
        std::array { std::filesystem::path { "/a/x.txt" }, std::filesystem::path { "/b/x.txt" } };
    auto const found = resolveLocalPath(fileSystem, both);
    REQUIRE(found);
    CHECK(found->path == "/b/x.txt");
    CHECK(found->kind == PathKind::File);
    CHECK(found->candidateIndex == 1);

    auto const directory = std::array { std::filesystem::path { "/a/src" } };
    CHECK(resolveLocalPath(fileSystem, directory)->kind == PathKind::Directory);

    auto const missing = std::array { std::filesystem::path { "/nope" } };
    CHECK_FALSE(resolveLocalPath(fileSystem, missing));
}

TEST_CASE("PathResolution.firstExistingWins.whenSeveralExist", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/a/x.txt", "");
    fileSystem.addFile("/b/x.txt", "");
    fileSystem.addDirectory("/c/x");
    fileSystem.addFile("/d/x", "");

    auto const bothFiles =
        std::array { std::filesystem::path { "/a/x.txt" }, std::filesystem::path { "/b/x.txt" } };
    auto const first = resolveLocalPath(fileSystem, bothFiles);
    REQUIRE(first);
    CHECK(first->path == "/a/x.txt");
    CHECK(first->kind == PathKind::File);
    CHECK(first->candidateIndex == 0);

    auto const directoryFirst =
        std::array { std::filesystem::path { "/c/x" }, std::filesystem::path { "/d/x" } };
    auto const directory = resolveLocalPath(fileSystem, directoryFirst);
    REQUIRE(directory);
    CHECK(directory->path == "/c/x");
    CHECK(directory->kind == PathKind::Directory);
    CHECK(directory->candidateIndex == 0);

    auto const fileFirst = std::array { std::filesystem::path { "/d/x" }, std::filesystem::path { "/c/x" } };
    auto const file = resolveLocalPath(fileSystem, fileFirst);
    REQUIRE(file);
    CHECK(file->kind == PathKind::File);
    CHECK(file->candidateIndex == 0);
}
