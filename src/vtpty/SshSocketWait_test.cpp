// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vtpty/SshSocketWait.hpp>

    #include <core/Utils.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>

    #include <array>
    #include <cerrno>
    #include <chrono>

    #include <poll.h>
    #include <unistd.h>

using namespace std::chrono;

TEST_CASE("a descriptor that never becomes ready times out at the deadline", "[ssh]")
{
    // A socket pair whose other end never writes: an agent that accepted and never answers.
    auto fds = std::array<int, 2> {};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    auto const closeBoth = core::Finally([&]() {
        ::close(fds[0]);
        ::close(fds[1]);
    });

    auto const start = steady_clock::now();
    auto const waited = vtpty::waitForDescriptor(fds[0], POLLIN, start + milliseconds { 200 });
    auto const took = steady_clock::now() - start;
    REQUIRE_FALSE(waited.has_value());
    CHECK(waited.error() == ETIMEDOUT);
    CHECK(took >= milliseconds { 200 });
    CHECK(took < seconds { 5 });

    SECTION("a deadline already passed does not wait at all")
    {
        auto const late = vtpty::waitForDescriptor(fds[0], POLLIN, steady_clock::now() - seconds { 1 });
        REQUIRE_FALSE(late.has_value());
        CHECK(late.error() == ETIMEDOUT);
    }

    SECTION("a descriptor with data waiting is ready at once")
    {
        REQUIRE(::write(fds[1], "x", 1) == 1);
        CHECK(vtpty::waitForDescriptor(fds[0], POLLIN, steady_clock::now() + seconds { 30 }).has_value());
    }

    SECTION("a writable descriptor is ready for output")
    {
        CHECK(vtpty::waitForDescriptor(fds[0], POLLOUT, steady_clock::now() + seconds { 30 }).has_value());
    }
}

#endif
