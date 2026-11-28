// SPDX-License-Identifier: Apache-2.0
//
// The native relation across mount boundaries on Linux: the kernel's own `..` leaves a mount for
// the directory it is mounted on, so a mount -- a bind mount of a directory beneath the root
// included -- is beneath what it is mounted beneath, whatever device it is on.
#include <core/Environment.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

using core::fs::DescriptorFlags;
using core::fs::FsResult;
using core::fs::IDescriptor;
using core::fs::Relation;
using core::fs::test::DescriptorFixture;

namespace
{

/// What a child process exits with to say its case could not run here.
constexpr int SkippedExit = 77;

/// @return The device @p path is on, or nullopt where it cannot be stat'ed.
[[nodiscard]] std::optional<::dev_t> deviceOf(char const* path)
{
    struct ::stat status {};
    if (::stat(path, &status) != 0)
        return std::nullopt;
    return status.st_dev;
}

/// @return A preopen of @p path, the host's, or null where it cannot be opened: no assertion, so a
///         forked child may call it.
[[nodiscard]] std::unique_ptr<IDescriptor> tryPreopen(DescriptorFixture const& fixture,
                                                      std::filesystem::path path)
{
    auto opened = fixture.run(fixture.fileSystem().openPreopen(std::move(path), DescriptorFlags::Read));
    return opened ? std::move(*opened) : nullptr;
}

/// @return A preopen of @p path, the host's.
[[nodiscard]] std::unique_ptr<IDescriptor> preopen(DescriptorFixture const& fixture,
                                                   std::filesystem::path path)
{
    auto opened = tryPreopen(fixture, std::move(path));
    REQUIRE(opened != nullptr);
    return opened;
}

/// Writes @p text to @p path, as a process's id maps are written.
/// @return Whether it was written.
[[nodiscard]] bool writeFile(char const* path, std::string const& text)
{
    auto file = std::ofstream { path };
    file << text;
    file.flush();
    return file.good();
}

/// @return Whether @p answer is @p expected.
[[nodiscard]] bool is(FsResult<Relation> const& answer, Relation expected)
{
    return answer.has_value() && *answer == expected;
}

} // namespace

TEST_CASE("relation crosses a mount upward", "[fs][native][relation]")
{
    // /proc is a mount of its own wherever Linux runs: its `..` is the root of another device.
    auto const rootDevice = deviceOf("/");
    auto const procDevice = deviceOf("/proc");
    if (!rootDevice || !procDevice || *rootDevice == *procDevice)
        SKIP("/proc is not a mount of its own here");
    auto fixture = DescriptorFixture::createNative();
    auto const root = preopen(fixture, "/");
    auto const proc = preopen(fixture, "/proc");
    CHECK(is(fixture.run(fixture.fileSystem().relation(*root, *proc)), Relation::FirstContainsSecond));
    CHECK(is(fixture.run(fixture.fileSystem().relation(*proc, *root)), Relation::SecondContainsFirst));
}

namespace
{

/// What a row of the namespace case expects.
enum class Expect : std::uint8_t
{
    Same,
    FirstContainsSecond,
    Unrelated,
    Unsupported,
};

/// One row of the namespace case: two directories by their path beneath the host (empty for the
/// host itself), and the answer.
struct NamespaceRow
{
    char const* name;
    char const* first;
    char const* second;
    Expect expect;
};

constexpr auto NamespaceRows = std::to_array<NamespaceRow>({
    { "the root contains a tmpfs mounted whole", "", "t/x", Expect::FirstContainsSecond },
    { "a directory beside the tmpfs", "c", "t/x", Expect::Unrelated },
    { "a sub-bind hides a from a walk up from its key", "a", "m/key", Expect::Unsupported },
    { "the root contains the sub-bind mounted beneath it", "", "m", Expect::FirstContainsSecond },
    { "a/sub and its bind at m", "a/sub", "m", Expect::Same },
    { "two directories on the sub-bind", "m/x", "m/y", Expect::Unrelated },
    { "the sub-bind contains its own", "m", "m/y", Expect::FirstContainsSecond },
    { "a tmpfs directory against the sub-bind", "t/x", "m/key", Expect::Unrelated },
    { "the sub-bind against a tmpfs directory", "m/key", "t/x", Expect::Unrelated },
    { "two directories on the overlay", "o/x", "o/y", Expect::Unrelated },
    { "the overlay's lower directory and what it shows", "lower", "o/x", Expect::Unsupported },
    { "the overlay against another file system's directory", "o/x", "c", Expect::Unsupported },
    { "the overlay contains its own", "o", "o/x", Expect::FirstContainsSecond },
    { "a bind beneath the grant shows the key's file system", "g", "k/secret", Expect::Unsupported },
    { "the key, against a grant a bind beneath shows it through", "k/secret", "g", Expect::Unsupported },
    { "a tmpfs beneath the grant shows no other file system", "h", "c", Expect::Unrelated },
    { "a mount beneath the grant shows none of the key's", "g", "t/x", Expect::Unrelated },
});

/// What a forked child exits with where it could not run its rows: an open failed, something threw,
/// or no unprivileged overlay could be mounted. A row that fails exits with its number, from 1.
constexpr int OpenFailedExit = 100;
constexpr int ThrewExit = 101;
constexpr int NoOverlayExit = 102;

/// @return Whether @p answer is what @p expect says.
[[nodiscard]] bool meets(FsResult<Relation> const& answer, Expect expect)
{
    switch (expect)
    {
        case Expect::Same: return is(answer, Relation::Same);
        case Expect::FirstContainsSecond: return is(answer, Relation::FirstContainsSecond);
        case Expect::Unrelated: return is(answer, Relation::Unrelated);
        case Expect::Unsupported:
            return !answer.has_value() && answer.error().code == core::fs::FsErrorCode::Unsupported;
    }
    return false;
}

} // namespace

TEST_CASE("relation across mounts made in a namespace of its own", "[fs][native][relation]")
{
    // In a child's own user and mount namespace, with a file system built there so it reads that
    // namespace's mountinfo:
    // - a tmpfs mounted whole at root/t is walked across: t/x lies beneath the root;
    // - root/a/sub bound at root/m hides a: the kernel's `..` of m is the root, so a walk from m/key
    //   never meets a, though m/key lies beneath it. That is Unsupported, never Unrelated (the root,
    //   which m is mounted beneath, still contains it);
    // - but two directories both on m are walked whole inside it, and a directory on the tmpfs is on
    //   another file system, which nothing m hides can be: those answers stand;
    // - an overlay at root/o shows root/lower's directories as its own: only its own mount decides;
    // - root/k bound at root/g/inner puts the key root/k/secret beneath the grant root/g, though
    //   neither walk up meets the other: a mount beneath a directory that shows the other's file
    //   system is Unsupported, and one of another file system (a tmpfs at root/h/tm) changes nothing.
    auto const uid = ::getuid();
    auto const gid = ::getgid();
    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
            ::_exit(SkippedExit);
        if (!writeFile("/proc/self/setgroups", "deny")
            || !writeFile("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1")
            || !writeFile("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1"))
            ::_exit(SkippedExit);
        if (::mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
            ::_exit(SkippedExit);
        try
        {
            auto fixture = DescriptorFixture::createNative();
            auto const& host = fixture.hostDirectory();
            for (auto const* directory: { "a/sub/key",
                                          "a/sub/x",
                                          "a/sub/y",
                                          "m",
                                          "t",
                                          "c",
                                          "lower/x",
                                          "lower/y",
                                          "upper",
                                          "work",
                                          "o",
                                          "k/secret",
                                          "g/inner",
                                          "h/tm" })
                std::filesystem::create_directories(host / directory);
            if (::mount((host / "a" / "sub").c_str(), (host / "m").c_str(), nullptr, MS_BIND, nullptr) != 0
                || ::mount("tmpfs", (host / "t").c_str(), "tmpfs", 0, "size=1m") != 0
                || ::mount((host / "k").c_str(), (host / "g" / "inner").c_str(), nullptr, MS_BIND, nullptr)
                       != 0
                || ::mount("tmpfs", (host / "h" / "tm").c_str(), "tmpfs", 0, "size=1m") != 0)
                ::_exit(SkippedExit);
            auto const overlayOptions = "lowerdir=" + (host / "lower").string() + ",upperdir="
                                        + (host / "upper").string() + ",workdir=" + (host / "work").string();
            if (::mount("overlay", (host / "o").c_str(), "overlay", 0, overlayOptions.c_str()) != 0)
                ::_exit(NoOverlayExit);
            std::filesystem::create_directories(host / "t" / "x");
            auto& system = fixture.fileSystem();
            for (auto const& [index, row]: std::views::zip(std::views::iota(1), NamespaceRows))
            {
                auto const first = tryPreopen(fixture, host / row.first);
                auto const second = tryPreopen(fixture, host / row.second);
                if (!first || !second)
                    ::_exit(OpenFailedExit);
                if (!meets(fixture.run(system.relation(*first, *second)), row.expect))
                    ::_exit(index);
            }
        }
        catch (...)
        {
            ::_exit(ThrewExit);
        }
        ::_exit(0);
    }
    auto status = 0;
    while (::waitpid(child, &status, 0) < 0)
        REQUIRE(errno == EINTR);
    REQUIRE(WIFEXITED(status));
    auto const exit = WEXITSTATUS(status);
    if (exit == SkippedExit || exit == NoOverlayExit)
    {
        auto const why = exit == SkippedExit ? "no user and mount namespace for an unprivileged mount here"
                                             : "no unprivileged overlay mount here";
        // CI's Linux jobs allow unprivileged user namespaces and set this, so the rows cannot skip
        // there unseen.
        if (core::LiveEnvironment {}.get("CORE_TEST_REQUIRE_USERNS") == "1")
            FAIL("CORE_TEST_REQUIRE_USERNS=1, but " << why);
        SKIP(why);
    }
    REQUIRE(exit != OpenFailedExit);
    REQUIRE(exit != ThrewExit);
    auto const failedRow = exit > 0 ? static_cast<std::size_t>(exit) : std::size_t { 0 };
    INFO("the first row that failed: " << (failedRow >= 1 && failedRow <= NamespaceRows.size()
                                               ? NamespaceRows.at(failedRow - 1).name
                                               : "none"));
    CHECK(exit == 0);
}
