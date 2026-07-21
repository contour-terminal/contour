// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vtpty/MockPty.h>

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>

    #include <array>
    #include <chrono>
    #include <cstring>
    #include <format>
    #include <functional>
    #include <memory>
    #include <string>
    #include <string_view>
    #include <vector>

    #include <fcntl.h>
    #include <unistd.h>

    #include <coro/Task.hpp>
    #include <coro/WhenAll.hpp>
    #include <muxserver/SessionHost.h>
    #include <muxserver/imsg/CommandArgv.h>
    #include <muxserver/imsg/Identify.h>
    #include <muxserver/imsg/ImsgCodec.h>
    #include <muxserver/tmux/ImsgServer.h>
    #include <net/EventLoop.h>
    #include <net/PollEventSource.h>
    #include <net/Sockets.h>

using coro::Task;
using namespace std::chrono_literals;
namespace imsg = muxserver::imsg;

namespace
{

/// Sends @p wire (with optional @p fd via SCM_RIGHTS) on the raw socket.
void sendRaw(int socketFd, std::span<std::byte const> wire, int fd = -1)
{
    auto writable = std::vector<std::byte> { wire.begin(), wire.end() };
    auto iov = ::iovec { .iov_base = writable.data(), .iov_len = writable.size() };
    auto control = std::array<char, CMSG_SPACE(sizeof(int))> {};
    auto msg = ::msghdr {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fd >= 0)
    {
        msg.msg_control = control.data();
        msg.msg_controllen = CMSG_SPACE(sizeof(int));
        auto* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    REQUIRE(::sendmsg(socketFd, &msg, 0) == static_cast<ssize_t>(writable.size()));
}

[[nodiscard]] std::vector<std::byte> cstringPayload(std::string_view text)
{
    auto const* begin = reinterpret_cast<std::byte const*>(text.data());
    auto out = std::vector<std::byte> { begin, begin + text.size() };
    out.push_back(std::byte { 0 });
    return out;
}

[[nodiscard]] std::vector<std::byte> u64Payload(uint64_t value)
{
    auto out = std::vector<std::byte>(sizeof(uint64_t));
    std::memcpy(out.data(), &value, sizeof(uint64_t));
    return out;
}

void makeNonBlocking(int fd)
{
    REQUIRE(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
}

/// The hand-rolled tmux client half: an imsg socketpair end plus the two
/// pipes whose far ends were passed as the client's stdin/stdout.
struct FakeTmuxClient
{
    int imsgFd = -1;     ///< Our end of the imsg socketpair.
    int stdinWrite = -1; ///< We write control-mode COMMANDS here.
    int stdoutRead = -1; ///< We read %-notifications and guards here.

    ~FakeTmuxClient()
    {
        for (auto const fd: { imsgFd, stdinWrite, stdoutRead })
            if (fd >= 0)
                ::close(fd);
    }

    /// Runs the full 3.7b identify burst plus the startup command.
    void handshake(uint64_t flags, std::string_view startupCommand = "attach-session")
    {
        auto stdinPipe = std::array<int, 2> { -1, -1 };
        auto stdoutPipe = std::array<int, 2> { -1, -1 };
        REQUIRE(::pipe(stdinPipe.data()) == 0);
        REQUIRE(::pipe(stdoutPipe.data()) == 0);
        stdinWrite = stdinPipe[1];
        stdoutRead = stdoutPipe[0];
        makeNonBlocking(stdoutRead);
        makeNonBlocking(imsgFd);

        auto const send = [this](uint32_t type, std::span<std::byte const> payload, int fd = -1) {
            sendRaw(imsgFd, imsg::encodeFrame(type, payload, fd >= 0), fd);
        };
        send(imsg::msgtype::IdentifyLongFlags, u64Payload(flags));
        send(imsg::msgtype::IdentifyLongFlags, u64Payload(flags)); // the real client sends it twice
        send(imsg::msgtype::IdentifyTerm, cstringPayload("tmux-256color"));
        send(imsg::msgtype::IdentifyTtyName, cstringPayload("/dev/pts/9"));
        send(imsg::msgtype::IdentifyStdin, {}, stdinPipe[0]);
        send(imsg::msgtype::IdentifyStdout, {}, stdoutPipe[1]);
        send(imsg::msgtype::IdentifyDone, {});
        ::close(stdinPipe[0]);
        ::close(stdoutPipe[1]);

        if (!startupCommand.empty())
        {
            auto const argv = std::vector<std::string> { std::string { startupCommand } };
            send(imsg::msgtype::Command, imsg::packArgv(argv));
        }
    }
};

// The client helpers POLL non-blocking fds between loop ticks: the scenario
// coroutine runs on the SAME thread as the server, so a blocking read here
// would deadlock the reactor.

/// Reads (bounded) from @p fd until @p needle appears (or EOF/timeout).
Task<std::string> readUntil(net::EventLoop* loop, int fd, std::string needle)
{
    auto collected = std::string {};
    auto buffer = std::array<char, 4096> {};
    for (auto i = 0; i < 2000 && !collected.contains(needle); ++i)
    {
        auto const n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0)
        {
            collected.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
            break; // EOF: whatever arrived is what we judge
        co_await loop->delay(1ms);
    }
    co_return collected;
}

/// Reads one imsg frame from @p fd (bounded).
Task<std::optional<imsg::ImsgFrame>> readImsgFrame(net::EventLoop* loop, int fd)
{
    auto decoder = imsg::ImsgDecoder {};
    auto buffer = std::array<std::byte, 4096> {};
    for (auto i = 0; i < 2000; ++i)
    {
        auto frame = decoder.next();
        if (!frame.has_value())
            co_return std::nullopt;
        if (frame->has_value())
            co_return std::move(*frame);
        auto const n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0)
        {
            decoder.feed(std::span { buffer.data(), static_cast<std::size_t>(n) });
            continue;
        }
        if (n == 0)
            co_return std::nullopt;
        co_await loop->delay(1ms);
    }
    co_return std::nullopt;
}

/// The served side: a SessionHost plus the imsg handler over a socketpair.
struct ImsgHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    muxserver::SessionHost host { loop,
                                  [](vtbackend::PageSize size) {
                                      return std::make_unique<vtpty::MockPty>(size);
                                  },
                                  vtbackend::Settings {},
                                  /*startPumps=*/false };
    FakeTmuxClient client;
    std::unique_ptr<net::ISocket> serverEnd;

    ImsgHarness()
    {
        auto fds = std::array<int, 2> { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
        auto adopted = net::adoptFd(loop, fds[0]);
        REQUIRE(adopted.has_value());
        serverEnd = std::move(*adopted);
        client.imsgFd = fds[1];
        host.createTab();
    }

    /// Runs the server handler to completion alongside @p scenario.
    void run(std::function<Task<void>(ImsgHarness*)> scenario)
    {
        auto handler = muxserver::tmux::makeTmuxImsgHandler(loop, host);
        auto drive = [](ImsgHarness* h,
                        std::function<Task<void>(ImsgHarness*)> inner,
                        std::function<Task<void>(std::unique_ptr<net::ISocket>)>* serve) -> Task<void> {
            co_await coro::whenAll((*serve)(std::move(h->serverEnd)), inner(h));
        };
        auto serve = std::function<Task<void>(std::unique_ptr<net::ISocket>)> {
            [&handler](std::unique_ptr<net::ISocket> s) { return handler(std::move(s)); }
        };
        loop.blockOn(drive(this, std::move(scenario), &serve));
    }
};

Task<void> happyScenario(ImsgHarness* h)
{
    // The preamble guard pair carries flag 0 (MSG_COMMAND-originated).
    auto const preamble = co_await readUntil(&h->loop, h->client.stdoutRead, "%session-changed");
    CHECK(preamble.contains(" 0 0\n"));
    CHECK(preamble.contains("%begin"));
    CHECK(preamble.contains("%end"));
    CHECK(preamble.contains("%session-changed $0 0"));

    // A command typed on the passed stdin runs guarded with flag 1.
    REQUIRE(::write(h->client.stdinWrite, "list-sessions\n", 14) >= 0);
    auto const listing = co_await readUntil(&h->loop, h->client.stdoutRead, "(attached)");
    CHECK(listing.contains("windows] (attached)"));
    CHECK(listing.contains(" 1\n"));

    // An empty line detaches: the stdout drains (NO %exit — the real client
    // binary prints that itself), then MSG_EXIT arrives on the imsg socket.
    REQUIRE(::write(h->client.stdinWrite, "\n", 1) >= 0);
    auto const frame = co_await readImsgFrame(&h->loop, h->client.imsgFd);
    REQUIRE(frame.has_value());
    CHECK(frame->type == imsg::msgtype::Exit);
    auto retval = int32_t { -1 };
    REQUIRE(frame->payload.size() >= sizeof(int32_t));
    std::memcpy(&retval, frame->payload.data(), sizeof(int32_t));
    CHECK(retval == 0);
    auto const tail = co_await readUntil(&h->loop, h->client.stdoutRead, "%exit");
    CHECK(!tail.contains("%exit"));

    ::close(h->client.imsgFd); // the client exits; the server unwinds
    h->client.imsgFd = -1;
}

Task<void> expectExitScenario(ImsgHarness* h, std::string needle)
{
    auto const frame = co_await readImsgFrame(&h->loop, h->client.imsgFd);
    REQUIRE(frame.has_value());
    CHECK(frame->type == imsg::msgtype::Exit);
    if (!needle.empty())
    {
        auto const text =
            std::string_view { reinterpret_cast<char const*>(frame->payload.data()), frame->payload.size() };
        CHECK(text.contains(needle));
    }
}

Task<void> expectVersionScenario(ImsgHarness* h)
{
    auto const frame = co_await readImsgFrame(&h->loop, h->client.imsgFd);
    REQUIRE(frame.has_value());
    CHECK(frame->type == imsg::msgtype::Version);
}

} // namespace

TEST_CASE("a control client handshakes into a served control session", "[muxserver][imsgserver]")
{
    auto h = ImsgHarness {};
    h.client.handshake(imsg::ClientControl);
    h.run([](ImsgHarness* inner) { return happyScenario(inner); });
}

TEST_CASE("non-control and -CC clients are rejected with MSG_EXIT", "[muxserver][imsgserver]")
{
    auto const rejects = [](uint64_t flags, std::string const& needle) {
        auto h = ImsgHarness {};
        h.client.handshake(flags, /*startupCommand=*/"");
        h.run([&needle](ImsgHarness* inner) { return expectExitScenario(inner, needle); });
    };
    rejects(0, "only control-mode");
    rejects(imsg::ClientControl | imsg::ClientControlControl, "-CC");
}

TEST_CASE("an unsupported startup command is rejected", "[muxserver][imsgserver]")
{
    auto h = ImsgHarness {};
    h.client.handshake(imsg::ClientControl, "kill-server");
    h.run([](ImsgHarness* inner) { return expectExitScenario(inner, {}); });
}

TEST_CASE("a version mismatch answers MSG_VERSION and drops", "[muxserver][imsgserver]")
{
    auto h = ImsgHarness {};
    // Hand-craft a frame whose peerid carries version 7.
    auto wire = imsg::encodeFrame(imsg::msgtype::IdentifyDone, {});
    auto const badVersion = uint32_t { 7 };
    std::memcpy(wire.data() + 8, &badVersion, sizeof(uint32_t));
    sendRaw(h.client.imsgFd, wire);
    makeNonBlocking(h.client.imsgFd);
    h.run([](ImsgHarness* inner) { return expectVersionScenario(inner); });
}

#endif // !_WIN32
