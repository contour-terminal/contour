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

    #include <fcntl.h>
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

TEST_CASE("readWithFd keeps at most one descriptor and leaks none", "[net][fdpass]")
{
    // readWithFd's control buffer holds exactly ONE descriptor, so a two-fd message always
    // overflows it — and whether the kernel REPORTS that overflow is where the platforms part
    // company. Linux hands over the one that fits and does not set MSG_CTRUNC; Darwin sets it, and
    // on MSG_CTRUNC this code deliberately distrusts the entire set and keeps nothing, because a
    // truncated cmsg_len describes descriptors that never arrived.
    //
    // So "keeps the FIRST descriptor" is Linux's behaviour, not a portable contract — asserting it
    // is what made this test fail the first time macOS ever ran it. What holds everywhere, and is
    // what actually matters, is below: never more than one kept, and never one leaked.
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

    if (result->fd >= 0)
    {
        // A descriptor was kept: it must be the FIRST one sent, never the extra.
        REQUIRE(::write(keepPipe[1], "keep", 4) == 4);
        auto identity = std::array<char, 8> {};
        CHECK(::read(result->fd, identity.data(), identity.size()) == 4);
        CHECK(std::string_view(identity.data(), 4) == "keep");
        ::close(result->fd);
    }

    // The extra write end must not be readable from here — but this read is NON-BLOCKING on
    // purpose. Whether the undelivered descriptor of an overflowed control message is closed by
    // the kernel is itself platform-specific: Linux closes it, so the read end reports EOF, while
    // Darwin may leave it in place. A blocking read does not FAIL that difference, it HANGS on it —
    // which is how this cost a ten-minute CI timeout the first time macOS ran it.
    auto const flags = ::fcntl(extraPipe[0], F_GETFL, 0);
    REQUIRE(flags != -1);
    REQUIRE(::fcntl(extraPipe[0], F_SETFL, flags | O_NONBLOCK) == 0);

    auto proof = std::array<char, 8> {};
    auto const fromExtra = ::read(extraPipe[0], proof.data(), proof.size());
    // Nothing is ever readable: this end only ever carried descriptors, never data.
    CHECK(fromExtra <= 0);
    #ifndef __APPLE__
    CHECK(fromExtra == 0); // and on Linux the kernel closed the write end, so it is EOF
    #endif

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
