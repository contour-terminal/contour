// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

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
    auto const spawned = vthost::tmux::spawnControlMode(loop, "", "/nonexistent-contour-test-path");
    REQUIRE_FALSE(spawned.has_value());
    CHECK(spawned.error().contains("tmux"));
}
#endif
