// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/GitHead.hpp>
#include <vtbackend/testing/InMemoryGitFileSystem.hpp>

#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        { "ref: refs/heads/", std::nullopt },
        // reftable's stub: the real HEAD is in .git/reftable/, which this reader does not decode.
        { "ref: refs/heads/.invalid\n", std::nullopt },
        { std::string { Sha1.substr(1) }, std::nullopt },
    }));
    INFO(contents);
    CHECK(parseGitHead(contents) == expected);
}

namespace
{
/// A GitHeadCache over in-memory files, whose lookups run when a test drains its executor.
struct GitFixture
{
    InMemoryFileSystem files;
    test::InMemoryGitFileSystem git { files };
    core::async::testing::ManualExecutor executor;
    int changes = 0; ///< How often the cache said its branch changed.
    GitHeadCache cache { GitHeadServices { .executor = executor, .fileSystem = git }, [this] { ++changes; } };

    /// What the cache shows for @p directory once its lookups ran.
    [[nodiscard]] std::string branchOf(std::optional<std::filesystem::path> const& directory)
    {
        cache.refresh(directory);
        executor.drain();
        return cache.branch();
    }
};
} // namespace

TEST_CASE("GitHead.findsTheRepositoryFromADeepDirectory", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto const git = test::InMemoryGitFileSystem { files };
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    auto deep = std::filesystem::path { "/r" };
    for (auto const level: std::views::iota(0, 100))
        deep /= std::format("d{}", level);
    REQUIRE(files.createDirectories(deep));

    CHECK(findGitHeadFile(git, "/r") == std::filesystem::path { "/r/.git/HEAD" });
    CHECK(findGitHeadFile(git, deep) == std::filesystem::path { "/r/.git/HEAD" });
    CHECK(findGitHeadFile(git, "/elsewhere") == std::nullopt);
    CHECK(findGitHeadFile(git, "r") == std::nullopt);
}

TEST_CASE("GitHead.stopsAtAMountBoundary", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto git = test::InMemoryGitFileSystem { files };
    files.addFile("/.git/HEAD", "ref: refs/heads/root\n");
    files.addFile("/net/host/repo/.git/HEAD", "ref: refs/heads/main\n");
    REQUIRE(files.createDirectories("/net/host/repo/src"));
    REQUIRE(files.createDirectories("/net/host/plain"));
    git.mount("/net", 5);
    git.mount("/net/host", 6);

    // Inside one filesystem the walk finds the repository as before.
    CHECK(findGitHeadFile(git, "/net/host/repo/src") == std::filesystem::path { "/net/host/repo/.git/HEAD" });
    // It does not climb out of the mount, to /net (an automount root) or to the repository at /.
    CHECK(findGitHeadFile(git, "/net/host/plain") == std::nullopt);
    CHECK(findGitHeadFile(git, "/net") == std::nullopt);
}

TEST_CASE("GitHead.followsAWorktreeFile", "[git]")
{
    auto files = InMemoryFileSystem {};
    auto const git = test::InMemoryGitFileSystem { files };
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    files.addFile("/r/.git/worktrees/w/HEAD", "ref: refs/heads/w\n");
    files.addFile("/r/.git/modules/s/HEAD", "ref: refs/heads/s\n");

    files.addFile("/w/.git", "gitdir: /r/.git/worktrees/w\n");
    CHECK(findGitHeadFile(git, "/w") == std::filesystem::path { "/r/.git/worktrees/w/HEAD" });

    files.addFile("/r/s/.git", "gitdir: ../.git/modules/s");
    CHECK(findGitHeadFile(git, "/r/s") == std::filesystem::path { "/r/.git/modules/s/HEAD" });

    // The first .git decides: a broken one does not fall through to the repository above it.
    files.addFile("/r/broken/.git", "not a gitdir line\n");
    CHECK(findGitHeadFile(git, "/r/broken") == std::nullopt);

    files.addFile("/r/huge/.git", "gitdir: /r/.git\n" + std::string(5000, 'x'));
    CHECK(findGitHeadFile(git, "/r/huge") == std::nullopt);
}

TEST_CASE("GitHeadCache.isStaleUntilInvalidated", "[git]")
{
    auto fixture = GitFixture {};
    fixture.files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    REQUIRE(fixture.files.createDirectories("/r/src"));
    auto& cache = fixture.cache;

    // A refresh reads nothing itself: the lookup runs on the executor, and its answer is published there.
    cache.refresh(std::filesystem::path { "/r/src" });
    CHECK(fixture.git.calls() == 0);
    CHECK(cache.branch().empty());
    fixture.executor.drain();
    CHECK(cache.branch() == "main");
    CHECK(fixture.changes == 1);

    REQUIRE(fixture.files.writeFile("/r/.git/HEAD", "ref: refs/heads/next"));
    cache.refresh(std::filesystem::path { "/r/src" });
    cache.refresh(std::filesystem::path { "/r/src" });
    CHECK(fixture.executor.pending() == 0);
    CHECK(cache.branch() == "main");

    cache.invalidate();
    CHECK(fixture.branchOf(std::filesystem::path { "/r/src" }) == "next");
    CHECK(fixture.changes == 2);

    CHECK(fixture.branchOf(std::filesystem::path { "/other" }).empty());
    CHECK(fixture.branchOf(std::filesystem::path { "/r/src" }) == "next");

    // No directory needs no lookup: the branch goes at once.
    cache.refresh(std::nullopt);
    CHECK(fixture.executor.pending() == 0);
    CHECK(cache.branch().empty());
}

TEST_CASE("GitHeadCache.answersTheLatestQuestion", "[git]")
{
    auto fixture = GitFixture {};
    fixture.files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    fixture.files.addFile("/q/.git/HEAD", "ref: refs/heads/other\n");

    // Asked twice before its lookup ran: one lookup, and it answers the second question.
    fixture.cache.refresh(std::filesystem::path { "/r" });
    fixture.cache.refresh(std::filesystem::path { "/q" });
    CHECK(fixture.executor.pending() == 1);
    fixture.executor.drain();
    CHECK(fixture.cache.branch() == "other");
    CHECK(fixture.changes == 1);
}

namespace
{
/// An InMemoryGitFileSystem that runs @ref onProbe before its first probe: what the terminal does while a
/// lookup is under way on the executor.
class InterruptedGitFileSystem final: public GitFileSystem
{
  public:
    explicit InterruptedGitFileSystem(test::InMemoryGitFileSystem const& inner): _inner { inner } {}

    mutable std::function<void()> onProbe; ///< Run, once, as the first probe begins.

    [[nodiscard]] GitPathStatus status(std::filesystem::path const& path) const override
    {
        if (auto const hook = std::exchange(onProbe, nullptr))
            hook();
        return _inner.status(path);
    }

    [[nodiscard]] std::optional<std::string> readMetadata(std::filesystem::path const& path) const override
    {
        return _inner.readMetadata(path);
    }

  private:
    test::InMemoryGitFileSystem const& _inner;
};
} // namespace

TEST_CASE("GitHeadCache.asksAgainWhileALookupRuns", "[git]")
{
    // A question asked while a lookup is RUNNING -- not queued -- is answered by that lookup going round
    // again, not by a second one, and its stale answer is never shown.
    auto files = InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    files.addFile("/q/.git/HEAD", "ref: refs/heads/other\n");
    auto const inner = test::InMemoryGitFileSystem { files };
    auto git = InterruptedGitFileSystem { inner };
    auto executor = core::async::testing::ManualExecutor {};
    auto changes = 0;
    auto cache = std::optional<GitHeadCache> {};
    cache.emplace(GitHeadServices { .executor = executor, .fileSystem = git }, [&] { ++changes; });

    git.onProbe = [&] {
        cache->refresh(std::filesystem::path { "/q" });
    };
    cache->refresh(std::filesystem::path { "/r" });
    CHECK(executor.drain() == 1);
    CHECK(cache->branch() == "other");
    // One answer told, the latest: the first question's was never published.
    CHECK(changes == 1);
}

TEST_CASE("GitHeadCache.aLookupAfterItsCacheIsGoneReadsNothing", "[git]")
{
    // A lookup still queued when its terminal goes -- at shutdown -- runs, reads nothing and tells nobody.
    auto files = InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    auto const git = test::InMemoryGitFileSystem { files };
    auto executor = core::async::testing::ManualExecutor {};
    auto changes = 0;
    auto cache = std::optional<GitHeadCache> {};
    cache.emplace(GitHeadServices { .executor = executor, .fileSystem = git }, [&] { ++changes; });

    cache->refresh(std::filesystem::path { "/r" });
    REQUIRE(executor.pending() == 1);
    cache.reset();
    executor.drain();
    CHECK(git.calls() == 0);
    CHECK(changes == 0);
}

TEST_CASE("GitHeadCache.readsAReftableRepositoryAsBranchless", "[git]")
{
    // Whatever HEAD says beside a reftable directory, it is not where the branch is kept.
    auto fixture = GitFixture {};
    fixture.files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    fixture.files.addDirectory("/r/.git/reftable");
    CHECK(fixture.branchOf(std::filesystem::path { "/r" }).empty());
}

TEST_CASE("GitHeadCache.showsADetachedHeadAsAShortHash", "[git]")
{
    // In parentheses, as git's own prompt does, so a commit does not read as a branch of that name.
    auto fixture = GitFixture {};
    fixture.files.addFile("/r/.git/HEAD", std::string { Sha1 } + "\n");
    CHECK(fixture.branchOf(std::filesystem::path { "/r" }) == "(a1b2c3d)");
}

TEST_CASE("GitHeadCache.showsOnlySafeText", "[git]")
{
    auto fixture = GitFixture {};
    fixture.files.addFile("/r/.git/HEAD", "ref: refs/heads/evil\x1b[2Jx\n");
    auto const evil = fixture.branchOf(std::filesystem::path { "/r" });
    CHECK_FALSE(evil.empty());
    CHECK_FALSE(evil.contains('\033'));

    fixture.files.addFile("/big/.git/HEAD", "ref: refs/heads/main\n" + std::string(5000, ' '));
    CHECK(fixture.branchOf(std::filesystem::path { "/big" }).empty());
}
