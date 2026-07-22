// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>

    #include <array>
    #include <cstddef>
    #include <cstring>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>

    #include <unistd.h>

    #include <coro/Task.hpp>
    #include <net/EventLoop.h>
    #include <net/PollEventSource.h>
    #include <net/Sockets.h>
    #include <net/SplitSocket.h>
    #include <net/testing/InMemoryTransport.h>

using coro::Task;

namespace
{

/// Sends @p payload plus @p fds via one blocking sendmsg on @p socketFd.
void sendWithFds(int socketFd, std::string_view payload, std::span<int const> fds)
{
    auto writable = std::string { payload }; // iovec wants a mutable pointer
    auto iov = ::iovec { .iov_base = writable.data(), .iov_len = writable.size() };
    auto control = std::array<char, CMSG_SPACE(8 * sizeof(int))> {};
    auto msg = ::msghdr {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (!fds.empty())
    {
        msg.msg_control = control.data();
        msg.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        auto* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));
    }
    REQUIRE(::sendmsg(socketFd, &msg, 0) == static_cast<ssize_t>(payload.size()));
}

/// One connected AF_UNIX socketpair: ours adopted into the reactor, theirs raw.
struct Pair
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    std::unique_ptr<net::ISocket> ours;
    int theirs = -1;

    Pair()
    {
        auto fds = std::array<int, 2> { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
        auto adopted = net::adoptFd(loop, fds[0]);
        REQUIRE(adopted.has_value());
        ours = std::move(*adopted);
        theirs = fds[1];
    }

    ~Pair()
    {
        if (theirs >= 0)
            ::close(theirs);
    }

    Pair(Pair const&) = delete;
    Pair& operator=(Pair const&) = delete;
    Pair(Pair&&) = delete;
    Pair& operator=(Pair&&) = delete;
};

} // namespace

TEST_CASE("readWithFd receives bytes and one SCM_RIGHTS descriptor", "[net][fdpass]")
{
    auto pair = Pair {};

    auto pipeFds = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(pipeFds.data()) == 0);
    REQUIRE(::write(pipeFds[1], "thru", 4) == 4);

    sendWithFds(pair.theirs, "hello", std::array { pipeFds[0] });
    ::close(pipeFds[0]); // the receiver owns its own copy now

    auto buffer = std::array<std::byte, 64> {};
    auto const result = pair.loop.blockOn(pair.ours->readWithFd(buffer));
    REQUIRE(result.has_value());
    CHECK(result->bytesRead == 5);
    REQUIRE(result->fd >= 0);

    // The received descriptor really is the pipe's read end.
    auto proof = std::array<char, 8> {};
    CHECK(::read(result->fd, proof.data(), proof.size()) == 4);
    CHECK(std::string_view(proof.data(), 4) == "thru");
    ::close(result->fd);
    ::close(pipeFds[1]);
}

TEST_CASE("readWithFd keeps the first descriptor and closes extras", "[net][fdpass]")
{
    auto pair = Pair {};

    auto keepPipe = std::array<int, 2> { -1, -1 };
    auto extraPipe = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(keepPipe.data()) == 0);
    REQUIRE(::pipe(extraPipe.data()) == 0);

    // Pass the KEEP pipe's read end and the EXTRA pipe's write end together.
    sendWithFds(pair.theirs, "x", std::array { keepPipe[0], extraPipe[1] });
    ::close(keepPipe[0]);
    ::close(extraPipe[1]); // ours was the last local copy of the write end...

    auto buffer = std::array<std::byte, 8> {};
    auto const result = pair.loop.blockOn(pair.ours->readWithFd(buffer));
    REQUIRE(result.has_value());
    REQUIRE(result->fd >= 0);

    // ...so if the receiver closed ITS copy (the extra), the read end sees EOF.
    auto proof = std::array<char, 8> {};
    CHECK(::read(extraPipe[0], proof.data(), proof.size()) == 0);

    ::close(result->fd);
    ::close(keepPipe[1]);
    ::close(extraPipe[0]);
}

TEST_CASE("readWithFd without ancillary data reports fd -1", "[net][fdpass]")
{
    auto pair = Pair {};
    REQUIRE(::write(pair.theirs, "plain", 5) == 5);

    auto buffer = std::array<std::byte, 8> {};
    auto const result = pair.loop.blockOn(pair.ours->readWithFd(buffer));
    REQUIRE(result.has_value());
    CHECK(result->bytesRead == 5);
    CHECK(result->fd == -1);
}

TEST_CASE("the default readWithFd never yields a descriptor", "[net][fdpass]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto scenario = [](net::EventLoop* loop, net::ISocket* a, net::ISocket* b) -> Task<void> {
        auto const payload = std::string_view { "mem" };
        std::ignore = co_await a->write(std::as_bytes(std::span { payload }));
        auto buffer = std::array<std::byte, 8> {};
        auto const result = co_await b->readWithFd(buffer);
        REQUIRE(result.has_value());
        CHECK(result->bytesRead == 3);
        CHECK(result->fd == -1);
        static_cast<void>(loop);
    };
    loop.blockOn(scenario(&loop, pair->first.get(), pair->second.get()));
}

TEST_CASE("a split socket reads one half and writes the other", "[net][fdpass]")
{
    auto pair = Pair {};

    // Two pipes: inbound (they write, we read) and outbound (we write, they read).
    auto inbound = std::array<int, 2> { -1, -1 };
    auto outbound = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(inbound.data()) == 0);
    REQUIRE(::pipe(outbound.data()) == 0);

    auto readHalf = net::adoptFd(pair.loop, inbound[0]);
    auto writeHalf = net::adoptFd(pair.loop, outbound[1]);
    REQUIRE(readHalf.has_value());
    REQUIRE(writeHalf.has_value());
    auto split = net::combineHalves(std::move(*readHalf), std::move(*writeHalf));

    REQUIRE(::write(inbound[1], "in", 2) == 2);
    auto buffer = std::array<std::byte, 8> {};
    auto const got = pair.loop.blockOn(split->read(buffer));
    REQUIRE(got.has_value());
    CHECK(*got == 2);

    auto const payload = std::string_view { "out" };
    auto const wrote = pair.loop.blockOn(split->write(std::as_bytes(std::span { payload })));
    REQUIRE(wrote.has_value());
    auto proof = std::array<char, 8> {};
    CHECK(::read(outbound[0], proof.data(), proof.size()) == 3);
    CHECK(std::string_view(proof.data(), 3) == "out");

    split->close();
    CHECK(split->isClosed());
    ::close(inbound[1]);
    ::close(outbound[0]);
}

TEST_CASE("a split socket forwards an fd received on its read half", "[net][fdpass]")
{
    auto pair = Pair {};

    // The read half is the fd-passing AF_UNIX socket; an outbound pipe is a real write half.
    auto outbound = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(outbound.data()) == 0);
    auto writeHalf = net::adoptFd(pair.loop, outbound[1]);
    REQUIRE(writeHalf.has_value());
    auto split = net::combineHalves(std::move(pair.ours), std::move(*writeHalf));

    auto pipeFds = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(pipeFds.data()) == 0);
    REQUIRE(::write(pipeFds[1], "thru", 4) == 4);
    sendWithFds(pair.theirs, "hello", std::array { pipeFds[0] });
    ::close(pipeFds[0]);

    auto buffer = std::array<std::byte, 64> {};
    auto const result = pair.loop.blockOn(split->readWithFd(buffer));
    REQUIRE(result.has_value());
    CHECK(result->bytesRead == 5);
    REQUIRE(result->fd >= 0); // the base default would have dropped it as -1

    auto proof = std::array<char, 8> {};
    CHECK(::read(result->fd, proof.data(), proof.size()) == 4);
    CHECK(std::string_view(proof.data(), 4) == "thru");
    ::close(result->fd);
    ::close(pipeFds[1]);
    ::close(outbound[0]);
}

TEST_CASE("a split socket is closed once either half closes", "[net][fdpass]")
{
    auto pair = Pair {};

    auto outbound = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(outbound.data()) == 0);
    auto writeHalf = net::adoptFd(pair.loop, outbound[1]);
    REQUIRE(writeHalf.has_value());

    auto* const writeHalfPtr = writeHalf->get(); // stays valid after ownership moves
    auto split = net::combineHalves(std::move(pair.ours), std::move(*writeHalf));

    CHECK_FALSE(split->isClosed());
    writeHalfPtr->close();    // close ONLY the write half
    CHECK(split->isClosed()); // the duplex socket is now unusable either way
    ::close(outbound[0]);
}

#endif // !_WIN32
