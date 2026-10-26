// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/GitHead.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

using namespace vtbackend;
using core::platform::testing::InMemoryFileSystem;

namespace
{
constexpr auto Sha1 = std::string_view { "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678" };
constexpr auto Sha256 =
    std::string_view { "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" };
} // namespace

TEST_CASE("GitHead.parsesEveryHeadForm", "[git]")
{
    auto const [contents, expected] = GENERATE(table<std::string, std::optional<GitHead>>({
        { "ref: refs/heads/main\n", GitHead { .kind = GitHeadKind::Branch, .name = "main" } },
        { "ref: refs/heads/feature/native-redesign",
          GitHead { .kind = GitHeadKind::Branch, .name = "feature/native-redesign" } },
        { "ref: refs/remotes/origin/x",
          GitHead { .kind = GitHeadKind::OtherRef, .name = "remotes/origin/x" } },
        { std::string { Sha1 } + "\n", GitHead { .kind = GitHeadKind::Detached, .name = "a1b2c3d" } },
        { std::string { Sha256 }, GitHead { .kind = GitHeadKind::Detached, .name = "0123456" } },
        { "", std::nullopt },
        { "ref:", std::nullopt },
        { "garbage", std::nullopt },
        { std::string { Sha1.substr(1) }, std::nullopt },
    }));
    INFO(contents);
    CHECK(parseGitHead(contents) == expected);
}

TEST_CASE("GitHead.findsTheRepositoryFromADeepDirectory", "[git]")
{
    auto files = InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    auto deep = std::filesystem::path { "/r" };
    for (auto const level: std::views::iota(0, 100))
        deep /= std::format("d{}", level);
    REQUIRE(files.createDirectories(deep));

    CHECK(findGitHeadFile(files, "/r") == std::filesystem::path { "/r/.git/HEAD" });
    CHECK(findGitHeadFile(files, deep) == std::filesystem::path { "/r/.git/HEAD" });
    CHECK(findGitHeadFile(files, "/elsewhere") == std::nullopt);
    CHECK(findGitHeadFile(files, "r") == std::nullopt);
}

TEST_CASE("GitHead.followsAWorktreeFile", "[git]")
{
    auto files = InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    files.addFile("/r/.git/worktrees/w/HEAD", "ref: refs/heads/w\n");
    files.addFile("/r/.git/modules/s/HEAD", "ref: refs/heads/s\n");

    files.addFile("/w/.git", "gitdir: /r/.git/worktrees/w\n");
    CHECK(findGitHeadFile(files, "/w") == std::filesystem::path { "/r/.git/worktrees/w/HEAD" });

    files.addFile("/r/s/.git", "gitdir: ../.git/modules/s");
    CHECK(findGitHeadFile(files, "/r/s") == std::filesystem::path { "/r/.git/modules/s/HEAD" });

    // The first .git decides: a broken one does not fall through to the repository above it.
    files.addFile("/r/broken/.git", "not a gitdir line\n");
    CHECK(findGitHeadFile(files, "/r/broken") == std::nullopt);

    files.addFile("/r/huge/.git", "gitdir: /r/.git\n" + std::string(5000, 'x'));
    CHECK(findGitHeadFile(files, "/r/huge") == std::nullopt);
}

TEST_CASE("GitHeadCache.isStaleUntilInvalidated", "[git]")
{
    auto files = InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    REQUIRE(files.createDirectories("/r/src"));
    auto cache = GitHeadCache { files };

    cache.refresh(std::filesystem::path { "/r/src" });
    CHECK(cache.branch() == "main");

    REQUIRE(files.writeFile("/r/.git/HEAD", "ref: refs/heads/next"));
    cache.refresh(std::filesystem::path { "/r/src" });
    cache.refresh(std::filesystem::path { "/r/src" });
    CHECK(cache.branch() == "main");

    cache.invalidate();
    cache.refresh(std::filesystem::path { "/r/src" });
    CHECK(cache.branch() == "next");

    cache.refresh(std::filesystem::path { "/other" });
    CHECK(cache.branch().empty());

    cache.refresh(std::filesystem::path { "/r/src" });
    CHECK(cache.branch() == "next");
    cache.refresh(std::nullopt);
    CHECK(cache.branch().empty());
}

TEST_CASE("GitHeadCache.showsOnlySafeText", "[git]")
{
    auto files = InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/evil\x1b[2Jx\n");
    auto cache = GitHeadCache { files };
    cache.refresh(std::filesystem::path { "/r" });
    CHECK_FALSE(cache.branch().empty());
    CHECK_FALSE(cache.branch().contains('\033'));

    files.addFile("/big/.git/HEAD", "ref: refs/heads/main\n" + std::string(5000, ' '));
    cache.refresh(std::filesystem::path { "/big" });
    CHECK(cache.branch().empty());
}
