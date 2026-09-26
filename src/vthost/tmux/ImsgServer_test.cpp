// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vtpty/MockPty.hpp>

    #include <core/async/Task.hpp>
    #include <core/async/WhenAll.hpp>
    #include <core/net/EventLoop.hpp>
    #include <core/net/IoBackend.hpp>
    #include <core/net/Sockets.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>
    #include <sys/wait.h>

    #include <algorithm>
    #include <array>
    #include <cerrno>
    #include <charconv>
    #include <chrono>
    #include <csignal>
    #include <cstdlib>
    #include <cstring>
    #include <format>
    #include <functional>
    #include <memory>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
    #include <unistd.h>

    #include <vthost/LoopDrain.hpp>
    #include <vthost/SessionHost.hpp>
    #include <vthost/imsg/CommandArgv.hpp>
    #include <vthost/imsg/Identify.hpp>
    #include <vthost/imsg/ImsgCodec.hpp>
    #include <vthost/tmux/ImsgServer.hpp>

using core::async::Task;
using namespace std::chrono_literals;
namespace imsg = vthost::imsg;

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
Task<std::string> readUntil(core::net::EventLoop* loop, int fd, std::string needle)
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
Task<std::optional<imsg::ImsgFrame>> readImsgFrame(core::net::EventLoop* loop, int fd)
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
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               /*startPumps=*/false };
    FakeTmuxClient client;
    std::unique_ptr<core::net::ISocket> serverEnd;
    int serverFd = -1; ///< serverEnd's descriptor (not owned), for a test that fills it first.

    ImsgHarness()
    {
        auto fds = std::array<int, 2> { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
        auto adopted = core::net::adoptFd(loop, fds[0]);
        REQUIRE(adopted.has_value());
        serverEnd = std::move(*adopted);
        serverFd = fds[0];
        client.imsgFd = fds[1];
        host.createTab();
    }

    /// Runs the server handler to completion alongside @p scenario.
    void run(std::function<Task<void>(ImsgHarness*)> scenario)
    {
        auto handler = vthost::tmux::makeTmuxImsgHandler(loop, host);
        auto drive = [](ImsgHarness* h,
                        std::function<Task<void>(ImsgHarness*)> inner,
                        std::function<Task<void>(std::unique_ptr<core::net::ISocket>)>* serve) -> Task<void> {
            co_await core::async::whenAll((*serve)(std::move(h->serverEnd)), inner(h));
        };
        auto serve = std::function<Task<void>(std::unique_ptr<core::net::ISocket>)> {
            [&handler](std::unique_ptr<core::net::ISocket> s) {
                return handler(vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(s));
            }
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

TEST_CASE("a control client handshakes into a served control session", "[vthost][imsgserver]")
{
    auto h = ImsgHarness {};
    h.client.handshake(imsg::ClientControl);
    h.run([](ImsgHarness* inner) { return happyScenario(inner); });
}

TEST_CASE("non-control and -CC clients are rejected with MSG_EXIT", "[vthost][imsgserver]")
{
    auto const rejects = [](uint64_t flags, std::string const& needle) {
        auto h = ImsgHarness {};
        h.client.handshake(flags, /*startupCommand=*/"");
        h.run([&needle](ImsgHarness* inner) { return expectExitScenario(inner, needle); });
    };
    rejects(0, "only control-mode");
    rejects(imsg::ClientControl | imsg::ClientControlControl, "-CC");
}

TEST_CASE("an unsupported startup command is rejected", "[vthost][imsgserver]")
{
    auto h = ImsgHarness {};
    h.client.handshake(imsg::ClientControl, "kill-server");
    h.run([](ImsgHarness* inner) { return expectExitScenario(inner, {}); });
}

TEST_CASE("a version mismatch answers MSG_VERSION and drops", "[vthost][imsgserver]")
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

namespace
{

/// Reads (bounded) from @p fd until it reports EOF.
/// @return Whether EOF was observed within the bound.
Task<bool> awaitEof(core::net::EventLoop* loop, int fd)
{
    auto buffer = std::array<char, 4096> {};
    for (auto i = 0; i < 2000; ++i)
    {
        auto const n = ::read(fd, buffer.data(), buffer.size());
        if (n == 0)
            co_return true;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            co_return true; // the far end is gone; EPIPE/ECONNRESET is an EOF too
        co_await loop->delay(1ms);
    }
    co_return false;
}

/// Attaches, then sends ONE malformed imsg frame on the control socket.
Task<void> framingViolationScenario(ImsgHarness* h)
{
    co_await readUntil(&h->loop, h->client.stdoutRead, "%session-changed");

    // A header whose declared length is below the header size itself: ImsgDecoder::next() rejects
    // it, which ends the imsg arm of the connection's whenAll.
    auto wire = imsg::encodeFrame(imsg::msgtype::Exiting, {});
    auto const badLength = uint16_t { 1 };
    std::memcpy(wire.data() + 4, &badLength, sizeof(uint16_t));
    sendRaw(h->client.imsgFd, wire);

    // The server must unwind the CONTROL SESSION too — it can only be ended through its transport,
    // so the bridge closing is the observable. Without it, run() stayed parked on the bridge
    // forever: the whenAll never resolved, the connection was never closed, and the session plus
    // every pane it drives leaked for the daemon's whole life — one per malformed frame.
    auto const closed = co_await awaitEof(&h->loop, h->client.stdoutRead);

    // Unconditionally give the control session its own EOF, so a REGRESSION fails this test
    // instead of hanging the suite on a flow that can never complete.
    if (h->client.stdinWrite >= 0)
    {
        ::close(h->client.stdinWrite);
        h->client.stdinWrite = -1;
    }
    CHECK(closed);
}

} // namespace

TEST_CASE("a framing violation after attach unwinds the control session", "[vthost][imsgserver]")
{
    auto h = ImsgHarness {};
    h.client.handshake(imsg::ClientControl);
    h.run([](ImsgHarness* inner) { return framingViolationScenario(inner); });
}

namespace
{

/// Attaches, then sends MSG_EXITING — the client announcing its own exit.
Task<void> clientExitingScenario(ImsgHarness* h)
{
    co_await readUntil(&h->loop, h->client.stdoutRead, "%session-changed");
    sendRaw(h->client.imsgFd, imsg::encodeFrame(imsg::msgtype::Exiting, {}));

    // Answered with MSG_EXITED...
    auto const frame = co_await readImsgFrame(&h->loop, h->client.imsgFd);
    REQUIRE(frame.has_value());
    CHECK(frame->type == imsg::msgtype::Exited);
    // ...and the control session unwound, exactly as on the EOF path.
    auto const closed = co_await awaitEof(&h->loop, h->client.stdoutRead);
    if (h->client.stdinWrite >= 0)
    {
        ::close(h->client.stdinWrite);
        h->client.stdinWrite = -1;
    }
    CHECK(closed);
}

} // namespace

TEST_CASE("a client-sent MSG_EXITING unwinds the control session", "[vthost][imsgserver]")
{
    auto h = ImsgHarness {};
    h.client.handshake(imsg::ClientControl);
    h.run([](ImsgHarness* inner) { return clientExitingScenario(inner); });
}

namespace
{

/// Writes to @p fd until a write would block, so that the next write the server makes parks.
/// @return How many bytes that took; the client discards exactly these before reading frames.
std::size_t fillUntilBlocked(int fd)
{
    auto const chunk = std::array<char, 4096> {};
    auto total = std::size_t { 0 };
    while (true)
    {
        auto const n = ::write(fd, chunk.data(), chunk.size());
        if (n <= 0)
            return total;
        total += static_cast<std::size_t>(n);
    }
}

/// Reads and discards exactly @p count bytes from @p fd (bounded).
/// @return Whether all of them arrived within the bound.
Task<bool> discardBytes(core::net::EventLoop* loop, int fd, std::size_t count)
{
    auto buffer = std::array<std::byte, 4096> {};
    for (auto i = 0; i < 2000 && count > 0; ++i)
    {
        auto const n = ::read(fd, buffer.data(), std::min(count, buffer.size()));
        if (n > 0)
        {
            count -= static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0)
            break;
        co_await loop->delay(1ms);
    }
    co_return count == 0;
}

/// Reads @p count imsg frames from @p fd through ONE decoder (bounded): frames written back to
/// back arrive in one read, which a decoder per frame would split and lose.
Task<std::vector<imsg::ImsgFrame>> readImsgFrames(core::net::EventLoop* loop, int fd, std::size_t count)
{
    auto decoder = imsg::ImsgDecoder {};
    auto frames = std::vector<imsg::ImsgFrame> {};
    auto buffer = std::array<std::byte, 4096> {};
    for (auto i = 0; i < 2000 && frames.size() < count; ++i)
    {
        auto frame = decoder.next();
        if (!frame.has_value())
            break;
        if (frame->has_value())
        {
            frames.push_back(std::move(**frame));
            continue;
        }
        auto const n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0)
        {
            decoder.feed(std::span { buffer.data(), static_cast<std::size_t>(n) });
            continue;
        }
        if (n == 0)
            break;
        co_await loop->delay(1ms);
    }
    co_return frames;
}

/// MSG_EXIT parks on a full socket, and MSG_EXITING arrives before it drains, so the server
/// answers MSG_EXITED while MSG_EXIT is still being written.
/// @return 0 when the client received MSG_EXIT and then MSG_EXITED; otherwise the failed step.
Task<int> overlappingExitScenario(ImsgHarness* h, std::size_t filler)
{
    co_await readUntil(&h->loop, h->client.stdoutRead, "%session-changed");

    // An empty line detaches: run() returns and MSG_EXIT goes onto the full socket, where it parks.
    if (::write(h->client.stdinWrite, "\n", 1) != 1)
        co_return 1;
    // Time for run() to return and the write to park. The socket stays full meanwhile, so waiting
    // longer changes nothing; too short a wait would only make the case pass without the overlap.
    co_await h->loop.delay(200ms);

    // MSG_EXITING now: the server answers it while MSG_EXIT is still parked.
    sendRaw(h->client.imsgFd, imsg::encodeFrame(imsg::msgtype::Exiting, {}));
    co_await h->loop.delay(50ms);

    if (!co_await discardBytes(&h->loop, h->client.imsgFd, filler))
        co_return 2;
    auto const frames = co_await readImsgFrames(&h->loop, h->client.imsgFd, 2);
    if (frames.size() != 2)
        co_return 3;
    if (frames[0].type != imsg::msgtype::Exit || frames[1].type != imsg::msgtype::Exited)
        co_return 4;

    auto const closed = co_await awaitEof(&h->loop, h->client.stdoutRead);
    if (h->client.stdinWrite >= 0)
    {
        ::close(h->client.stdinWrite);
        h->client.stdinWrite = -1;
    }
    co_return closed ? 0 : 5;
}

Task<void> recordOverlappingExit(ImsgHarness* h, std::size_t filler, int* outcome)
{
    *outcome = co_await overlappingExitScenario(h, filler);
}

} // namespace

TEST_CASE("MSG_EXIT and MSG_EXITED sent over each other both arrive, in order", "[vthost][imsgserver]")
{
    // Two writes armed on one socket at once is a contract violation core-cpp ends the process
    // for, so the scenario runs in a child and its exit status is the verdict.
    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // The child inherits Catch2's handler, which would report a second, bogus run on abort.
        std::ignore = std::signal(SIGABRT, SIG_DFL);
        auto outcome = 10;
        try
        {
            auto h = ImsgHarness {};
            makeNonBlocking(h.serverFd);
            auto const filler = fillUntilBlocked(h.serverFd);
            h.client.handshake(imsg::ClientControl);
            h.run([filler, &outcome](ImsgHarness* inner) {
                return recordOverlappingExit(inner, filler, &outcome);
            });
        }
        catch (...)
        {
            outcome = 11;
        }
        std::_Exit(outcome);
    }

    auto status = 0;
    auto waited = ::pid_t { 0 };
    for (auto i = 0; i < 300 && waited == 0; ++i)
    {
        waited = ::waitpid(child, &status, WNOHANG);
        if (waited == 0)
            std::this_thread::sleep_for(100ms);
    }
    if (waited == 0)
    {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
        FAIL("the child did not finish within 30 s");
    }
    INFO((WIFSIGNALED(status) ? std::format("the child was killed by signal {}", WTERMSIG(status))
                              : std::format("the child exited with {}", WEXITSTATUS(status))));
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

// {{{ live oracle: the REAL tmux client binary attaches to OUR imsg endpoint
    #include <cstdio>
    #include <filesystem>

    #ifdef __APPLE__
        #include <util.h>
    #elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
        #include <libutil.h>
    #else
        #include <pty.h>
    #endif

    #include <vthost/ConnectionAcceptor.hpp>

namespace
{

/// Runs a shell command via popen, returning captured stdout ("" on failure).
std::string runShellCapture(std::string const& command)
{
    auto* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
        return {};
    auto output = std::string {};
    auto buffer = std::array<char, 256> {};
    while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        output += buffer.data();
    std::ignore = ::pclose(pipe);
    return output;
}

/// The oracle's client side: drives the tmux binary's pty and reaps it.
Task<void> oracleScenario(core::net::EventLoop* loop,
                          int master,
                          pid_t child,
                          vthost::ConnectionAcceptor* server)
{
    makeNonBlocking(master);

    // The tmux client relays our preamble and session state verbatim.
    auto const preamble = co_await readUntil(loop, master, "%session-changed");
    CHECK(preamble.contains("%begin"));
    CHECK(preamble.contains("%end"));
    CHECK(preamble.contains("%session-changed $0 0"));

    // A command typed at the client round-trips through imsg-passed fds.
    REQUIRE(::write(master, "list-sessions\n", 14) == 14);
    auto const listing = co_await readUntil(loop, master, "(attached)");
    CHECK(listing.contains("windows] (attached)"));

    // An empty line detaches; the CLIENT prints %exit itself and exits 0.
    REQUIRE(::write(master, "\n", 1) == 1);
    auto const tail = co_await readUntil(loop, master, "%exit");
    CHECK(tail.contains("%exit"));

    auto status = -1;
    for (auto i = 0; i < 15000 && ::waitpid(child, &status, WNOHANG) == 0; ++i)
        co_await loop->delay(1ms);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    server->close(); // ends the accept loop; the drive's whenAll completes
}

} // namespace

TEST_CASE("a real tmux binary attaches over imsg", "[vthost][imsgserver][oracle]")
{
    if (!runShellCapture("command -v tmux && echo have-tmux").contains("have-tmux"))
    {
        SKIP("tmux not available");
    }
    // The rewritten imsg arrived around tmux 3.6; older clients speak the classic framing and cannot
    // talk to this endpoint.
    //
    // The version is PARSED rather than matched against a list of old spellings. `tmux -V` prints
    // things like "tmux 3.4", "tmux 3.5a" and "tmux next-3.4", and a list of " 3.4"-style substrings
    // silently misses every one of the prefixed forms -- so a `next-3.4` build sailed past the guard
    // and failed the test outright instead of skipping it.
    auto const version = runShellCapture("tmux -V");
    if (auto const digits = version.find_first_of("0123456789"); digits != std::string::npos)
    {
        auto const rest = std::string_view { version }.substr(digits);
        auto major = 0;
        auto minor = 0;
        auto const* const first = rest.data();
        auto const* const last = first + rest.size();
        if (auto const [ptr, ec] = std::from_chars(first, last, major);
            ec == std::errc {} && ptr != last && *ptr == '.')
            std::ignore = std::from_chars(ptr + 1, last, minor);
        if (std::pair { major, minor } < std::pair { 3, 6 })
            SKIP("tmux too old for the rewritten imsg protocol");
    }

    auto const socketDir = std::format("/tmp/contour-imsg-{}", ::getpid());
    auto const socketPath = socketDir + "/tmux.sock";
    std::filesystem::remove_all(socketDir);

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = vthost::SessionHost {
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
            return std::make_unique<vtpty::MockPty>(size);
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        /*startPumps=*/false,
    };
    host.createTab();
    // Declared after the host, so the connection flows the server spawns end while it exists.
    auto const drain = vthost::LoopDrain { loop };

    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());
    auto server = vthost::ConnectionAcceptor {
        loop, "test", std::move(*listener), vthost::tmux::makeTmuxImsgHandler(loop, host)
    };

    // The REAL tmux client on its own pty, pointed at OUR socket.
    auto master = -1;
    auto const child = ::forkpty(&master, nullptr, nullptr, nullptr);
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::execlp("tmux", "tmux", "-S", socketPath.c_str(), "-C", "attach-session", nullptr);
        ::_exit(127);
    }

    auto drive = [](vthost::ConnectionAcceptor* srv, Task<void> scenario) -> Task<void> {
        co_await core::async::whenAll(srv->serve(), std::move(scenario));
    };
    loop.blockOn(drive(&server, oracleScenario(&loop, master, child, &server)));

    ::close(master);
    std::filesystem::remove_all(socketDir);
}
// }}}

#endif // !_WIN32
