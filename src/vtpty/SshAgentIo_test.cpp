// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vtpty/SshAgentIo.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>

    #include <array>
    #include <cerrno>
    #include <chrono>
    #include <csignal>

    #include <unistd.h>

using namespace std::chrono;

namespace
{
/// libssh2's default functions, as far as these tests need them: plain recv()/send(), -errno on failure.
ssize_t plainRecv(int socket, void* buffer, size_t length, int flags, void** /*abstract*/)
{
    auto const rc = ::recv(socket, buffer, length, flags);
    return rc < 0 ? -errno : rc;
}

ssize_t plainSend(int socket, void const* buffer, size_t length, int flags, void** /*abstract*/)
{
    auto const rc = ::send(socket, buffer, length, flags);
    return rc < 0 ? -errno : rc;
}

/// A connected socket pair, closed at the end of the scope.
struct SocketPair
{
    std::array<int, 2> fds {};
    SocketPair() { REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0); }
    ~SocketPair()
    {
        for (auto const fd: fds)
            if (fd >= 0)
                ::close(fd);
    }
    SocketPair(SocketPair const&) = delete;
    SocketPair& operator=(SocketPair const&) = delete;
    SocketPair(SocketPair&&) = delete;
    SocketPair& operator=(SocketPair&&) = delete;

    void closePeer()
    {
        ::close(fds[1]);
        fds[1] = -1;
    }
};
} // namespace

TEST_CASE("the session's I/O functions bound the agent's socket, not the connection's", "[ssh]")
{
    auto pair = SocketPair {};
    auto io = vtpty::SshIoContext {};
    io.recv = &plainRecv;
    io.send = &plainSend;
    void* abstract = &io;
    auto buffer = std::array<char, 16> {};

    SECTION("the connection's socket passes through, even with the agent's deadline passed")
    {
        io.transport = pair.fds[0];
        io.agentDeadline = steady_clock::now() - seconds { 1 };
        REQUIRE(::write(pair.fds[1], "abc", 3) == 3);
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract) == 3);
        CHECK(vtpty::sshAgentBoundedSend(pair.fds[0], "x", 1, 0, &abstract) == 1);
        // Read what was sent before closing: on Linux, a socket closed with unread data resets its peer.
        REQUIRE(::read(pair.fds[1], buffer.data(), buffer.size()) == 1);
        // A closed peer is the transport's own business: libssh2 sees the 0 it would have seen.
        pair.closePeer();
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract) == 0);
        CHECK(io.agentIo == vtpty::SshAgentIo::Fine);
    }

    SECTION("an agent socket past the deadline times out at once")
    {
        io.agentDeadline = steady_clock::now() - seconds { 1 };
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract)
              == -ETIMEDOUT);
        CHECK(io.agentIo == vtpty::SshAgentIo::TimedOut);
        CHECK(vtpty::sshAgentBoundedSend(pair.fds[0], "x", 1, 0, &abstract) == -ETIMEDOUT);
    }

    SECTION("an agent socket that stays silent times out at the deadline")
    {
        auto const start = steady_clock::now();
        io.agentDeadline = start + milliseconds { 200 };
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract)
              == -ETIMEDOUT);
        CHECK(steady_clock::now() - start >= milliseconds { 200 });
    }

    SECTION("an agent socket passes through without a deadline")
    {
        REQUIRE(::write(pair.fds[1], "abc", 3) == 3);
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract) == 3);
        CHECK(vtpty::sshAgentBoundedSend(pair.fds[0], "x", 1, 0, &abstract) == 1);
        CHECK(io.agentIo == vtpty::SshAgentIo::Fine);
    }

    SECTION("an agent that closed its end is reported, not read again")
    {
        pair.closePeer();
        SECTION("with a deadline")
        {
            io.agentDeadline = steady_clock::now() + seconds { 30 };
        }
        SECTION("without one")
        {
        }
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract)
              == -ECONNRESET);
        CHECK(io.agentIo == vtpty::SshAgentIo::Closed);
        // Sending to it fails, and raises no SIGPIPE -- which, at its default disposition, would end this
        // test run. Contour does not ignore SIGPIPE, so neither does this.
        auto* const previous = std::signal(SIGPIPE, SIG_DFL);
        io.agentIo = vtpty::SshAgentIo::Fine;
        auto const sent = vtpty::sshAgentBoundedSend(pair.fds[0], "x", 1, 0, &abstract);
        std::signal(SIGPIPE, previous);
        CHECK((sent == -EPIPE || sent == -ECONNRESET));
        CHECK(io.agentIo == vtpty::SshAgentIo::Closed);
    }

    SECTION("an agent that went away with our request unread is reported, however the system says so")
    {
        // Linux resets a socket whose peer closed with data unread (recv fails with ECONNRESET); macOS
        // reads the end of the stream. Either way the agent is gone.
        REQUIRE(vtpty::sshAgentBoundedSend(pair.fds[0], "request", 7, 0, &abstract) == 7);
        pair.closePeer();
        CHECK(vtpty::sshAgentBoundedRecv(pair.fds[0], buffer.data(), buffer.size(), 0, &abstract)
              == -ECONNRESET);
        CHECK(io.agentIo == vtpty::SshAgentIo::Closed);
    }
}

#endif
