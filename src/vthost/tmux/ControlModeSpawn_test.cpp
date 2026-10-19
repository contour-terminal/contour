// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vthost/tmux/ControlModeSpawn.hpp>

#ifndef _WIN32
TEST_CASE("ControlModeSpawn.arguments", "[vthost][tmux]")
{
    // Built before the fork, as everything the child uses is: it may not allocate.
    CHECK(vthost::tmux::controlModeArguments("")
          == std::vector<std::string> { "tmux", "-C", "attach-session" });
    CHECK(vthost::tmux::controlModeArguments("/tmp/tmux-1000/default")
          == std::vector<std::string> { "tmux", "-C", "-S", "/tmp/tmux-1000/default", "attach-session" });
}

TEST_CASE("ControlModeSpawn.a tmux that is not on PATH is reported without a fork", "[vthost][tmux]")
{
    // The program is looked up here, before forkpty(): a PATH walk in the child would allocate. Not finding
    // it is then a reason to give, rather than a child that exits 127 behind a transport that just ends.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto placement = vtpty::NoPlacement {};
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto const spawned = vthost::tmux::spawnControlMode(
        loop,
        "",
        vtpty::ProgramSearch {
            .path = "/nonexistent-contour-test-path", .workingDirectory = {}, .fileSystem = files },
        placement);
    REQUIRE_FALSE(spawned.has_value());
    CHECK(spawned.error().contains("tmux was not found"));
}

TEST_CASE("ControlModeSpawn.tmux is looked for among the files it is handed", "[vthost][tmux]")
{
    // A tmux only this in-memory filesystem has: the lookup finds it there, without asking the disk. The
    // child's exec of it then fails, and the child exits; that it was forked at all says it was found.
    auto files = core::platform::testing::InMemoryFileSystem {};
    files.addExecutable("/contour-test-only/bin/tmux");
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto placement = vtpty::NoPlacement {};
    auto spawned = vthost::tmux::spawnControlMode(loop,
                                                  "",
                                                  vtpty::ProgramSearch { .path = "/contour-test-only/bin",
                                                                         .workingDirectory = {},
                                                                         .fileSystem = files },
                                                  placement);
    REQUIRE(spawned.has_value());
    spawned->transport.reset();
    vthost::tmux::reapControlMode(spawned->pid);
}

TEST_CASE("ControlModeSpawn.the client is placed before it runs", "[vthost][tmux][placement]")
{
    // The tmux client is a child of Contour like a session's shell: an out-of-memory kill of it in
    // Contour's own unit would stop Contour. A tmux of this test's own, on the PATH it is handed.
    auto const directory = core::testing::ScopedTempDir { "contour-tmux" };
    auto const tmux = directory.path() / "tmux";
    std::ofstream { tmux } << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(tmux, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto placement = vtpty::testing::ReleasingPlacement {};
    auto const path = directory.path().string();
    auto spawned = vthost::tmux::spawnControlMode(
        loop,
        "",
        vtpty::ProgramSearch { .path = path,
                               .workingDirectory = {},
                               .fileSystem = core::platform::NativeFileSystem::instance() },
        placement);
    REQUIRE(spawned.has_value());
    CHECK(placement.pids() == std::vector { spawned->pid });
    spawned->transport.reset();
    vthost::tmux::reapControlMode(spawned->pid);
}
#endif
