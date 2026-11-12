// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/WorkspaceName.hpp>
#include <vtbackend/testing/InMemoryGitFileSystem.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using core::platform::testing::InMemoryFileSystem;

TEST_CASE("WorkspaceName.aFolderInsideARepoIsNamedAfterTheRepo", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto const git = test::InMemoryGitFileSystem { files };
    files.addFile("/home/me/contour/.git/HEAD", "ref: refs/heads/main\n");
    REQUIRE(files.createDirectories("/home/me/contour/src/vthost"));

    CHECK(defaultWorkspaceName(git, "/home/me/contour") == "contour");
    CHECK(defaultWorkspaceName(git, "/home/me/contour/src/vthost") == "contour");
    CHECK(defaultWorkspaceName(git, "/home/me/contour/src/vthost/") == "contour");
}

TEST_CASE("WorkspaceName.aLinkedWorktreeIsNamedAfterItsOwnFolder", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto const git = test::InMemoryGitFileSystem { files };
    files.addFile("/home/me/contour/.git/HEAD", "ref: refs/heads/main\n");
    files.addFile("/home/me/contour/.git/worktrees/wt/HEAD", "ref: refs/heads/feature\n");
    files.addFile("/home/me/contour-sidebar/.git", "gitdir: /home/me/contour/.git/worktrees/wt\n");
    REQUIRE(files.createDirectories("/home/me/contour-sidebar/src"));

    CHECK(defaultWorkspaceName(git, "/home/me/contour-sidebar/src") == "contour-sidebar");
}

TEST_CASE("WorkspaceName.aPlainFolderIsNamedAfterItself", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto const git = test::InMemoryGitFileSystem { files };
    REQUIRE(files.createDirectories("/srv/notes"));

    CHECK(defaultWorkspaceName(git, "/srv/notes") == "notes");
    CHECK(defaultWorkspaceName(git, "/srv/gone") == "gone");
}

TEST_CASE("WorkspaceName.rootAndEmptyGiveEmpty", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto const git = test::InMemoryGitFileSystem { files };

    CHECK(defaultWorkspaceName(git, "/").empty());
    CHECK(defaultWorkspaceName(git, "").empty());
    CHECK(defaultWorkspaceName(git, ".").empty());
}
