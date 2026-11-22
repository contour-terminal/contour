// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/ShellPtyFactory.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace
{

#ifndef _WIN32
// The helpers of the POSIX-only cases below: a Windows build compiles none of those cases, and an unused
// helper there is a warning.
constexpr auto PageSize = vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) };

/// @return What @p path holds once it exists, waiting up to ten seconds while @p turn runs; empty if never.
std::string awaitFile(fs::path const& path, std::function<void()> const& turn = [] {})
{
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!fs::exists(path) && std::chrono::steady_clock::now() < deadline)
    {
        turn();
        std::this_thread::sleep_for(10ms);
    }
    auto in = std::ifstream { path };
    return std::string { std::istreambuf_iterator<char> { in }, {} };
}

/// A directory opened by handle, then moved away with a symlink to /tmp put at its name: a child entered by
/// the path would start in /tmp, one entered by the handle starts where it went.
struct SwappedDirectory
{
    core::testing::ScopedTempDir scratch { "contour-spawn" };
    fs::path named = scratch.path() / "worktree";
    fs::path moved = scratch.path() / "moved";
    std::shared_ptr<vtpty::DirectoryHandle const> handle;

    SwappedDirectory()
    {
        fs::create_directory(named);
        auto opened = vtpty::nativeDirectoryOpener().open(named.string());
        REQUIRE(opened.has_value());
        handle = std::make_shared<vtpty::DirectoryHandle const>(std::move(*opened));
        fs::rename(named, moved);
        fs::create_directory_symlink("/tmp", named);
    }

    /// @return The file the child reports into.
    [[nodiscard]] fs::path report() const { return scratch.path() / "report"; }
};

/// The names an agent's pane withholds, set in this process for its children to inherit -- and one kept.
struct InheritedRepositorySelection
{
    core::testing::ScopedEnv gitDir { "GIT_DIR", "/elsewhere/.git" };
    core::testing::ScopedEnv ghRepo { "GH_REPO", "someone/else" };
    core::testing::ScopedEnv sshCommand { "GIT_SSH_COMMAND", "ssh -i kept" };
};

/// @return An agent pane's override: a program reporting its directory and the three names into @p report,
///         started in @p directory by its handle, GIT_DIR and GH_REPO withheld.
vtpty::Process::ExecInfo agentOverride(SwappedDirectory const& directory)
{
    auto const script = std::format(
        R"sh(printf '%s|%s|%s|%s' "$(pwd -P)" "${{GIT_DIR-unset}}" "${{GH_REPO-unset}}" "${{GIT_SSH_COMMAND-unset}}" > '{0}.part' && mv '{0}.part' '{0}')sh",
        directory.report().string());
    auto exec = vtpty::testing::execInfo("/bin/sh", { "-c", script });
    exec.workingDirectory = directory.named;
    exec.removedEnvironment = { "GIT_DIR", "GH_REPO" };
    exec.directory = directory.handle;
    return exec;
}

/// @return What the agent's report must read.
std::string expectedReport(SwappedDirectory const& directory)
{
    return std::format("{}|unset|unset|ssh -i kept", fs::canonical(directory.moved).string());
}
#endif

/// A host spawning through the daemon's own factories, with or without a profile catalog.
struct SpawningHost
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    vthost::SessionHost host;

    explicit SpawningHost(std::shared_ptr<vthost::ProfileCatalog const> const& profiles = {}):
        host { loop,
               vthost::makeShellPtyFactory(vtpty::testing::execInfo("/bin/sh"),
                                           vthost::SandboxEscape::Contained,
                                           "/nonexistent/socket",
                                           vthost::ownShellDevices(std::make_shared<vtpty::NoPlacement>())),
               vtbackend::Settings {},
               core::defaultEnvironment(),
               vthost::SessionHostConfig {
                   .profiles = profiles,
                   .profilePtys = profiles
                                      ? vthost::makeProfilePtyFactory(
                                            vtpty::testing::execInfo("/bin/sh"),
                                            "/nonexistent/socket",
                                            vthost::ownShellDevices(std::make_shared<vtpty::NoPlacement>()))
                                      : vthost::ProfilePtyFactory {} } }
    {
    }

    void turn() { std::ignore = loop.runOnce(0ms); }
};

/// A host on MockPty that records every override it was asked to spawn.
struct RecordingHost
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    std::vector<vtpty::Process::ExecInfo> spawned;
    vthost::SessionHost host { loop,
                               [this](vtbackend::PageSize size,
                                      std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                                   spawned.push_back(commandOverride.value_or(vtpty::Process::ExecInfo {}));
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
};

} // namespace

#ifndef _WIN32
TEST_CASE("the daemon's shell factory spawns in the handle's directory with the withheld names removed",
          "[vthost][spawn]")
{
    auto const directory = SwappedDirectory {};
    auto const inherited = InheritedRepositorySelection {};
    auto const factory =
        vthost::makeShellPtyFactory(vtpty::testing::execInfo("/bin/sh", { "-l" }),
                                    vthost::SandboxEscape::Contained,
                                    "/nonexistent/socket",
                                    vthost::ownShellDevices(std::make_shared<vtpty::NoPlacement>()));

    auto const pty = factory(PageSize, agentOverride(directory));
    REQUIRE(pty != nullptr);
    REQUIRE(pty->start().has_value());

    CHECK(awaitFile(directory.report()) == expectedReport(directory));
}

TEST_CASE("a profile's spawn keeps them too", "[vthost][spawn][profiles]")
{
    auto const directory = SwappedDirectory {};
    auto const inherited = InheritedRepositorySelection {};
    auto shell = vthost::testing::fakeProfile("/bin/sh", vtbackend::LineCount(100));
    shell.sandbox = vthost::SandboxEscape::Contained;
    auto h = SpawningHost { std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> { { "main", std::move(shell) } }, "main") };

    auto const* tab = h.host.createTab(
        { .commandOverride = agentOverride(directory), .startDirectory = vthost::StartDirectory::Required });
    REQUIRE(tab != nullptr);

    CHECK(awaitFile(directory.report(), [&] { h.turn(); }) == expectedReport(directory));
}

TEST_CASE("a host without profiles spawns an agent in the handle's directory", "[vthost][spawn]")
{
    auto const directory = SwappedDirectory {};
    auto const inherited = InheritedRepositorySelection {};
    auto h = SpawningHost {};

    auto const* tab = h.host.createTab(
        { .commandOverride = agentOverride(directory), .startDirectory = vthost::StartDirectory::Required });
    REQUIRE(tab != nullptr);

    CHECK(awaitFile(directory.report(), [&] { h.turn(); }) == expectedReport(directory));
}
#endif

TEST_CASE("a Required start directory that is gone fails the spawn", "[vthost][spawn]")
{
    auto h = RecordingHost {};
    auto exec = vtpty::testing::execInfo("claude");
    exec.workingDirectory = "/nonexistent/worktree";

    CHECK(h.host.createTab({ .commandOverride = exec, .startDirectory = vthost::StartDirectory::Required })
          == nullptr);
    CHECK(h.spawned.empty());

    // Naming no directory at all is no less gone.
    CHECK(h.host.createTab({ .commandOverride = vtpty::testing::execInfo("claude"),
                             .startDirectory = vthost::StartDirectory::Required })
          == nullptr);
    CHECK(h.spawned.empty());
}

TEST_CASE("a Preferred one still falls back", "[vthost][spawn]")
{
    auto h = RecordingHost {};
    auto exec = vtpty::testing::execInfo("/bin/sh");
    exec.workingDirectory = "/nonexistent/worktree";

    CHECK(h.host.createTab({ .commandOverride = exec }) != nullptr);
    REQUIRE(h.spawned.size() == 1);
    CHECK(h.spawned.front().program == "/bin/sh");
    CHECK(h.spawned.front().workingDirectory.empty());
}
