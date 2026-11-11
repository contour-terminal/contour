// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChildProcess.hpp>

#include <core/codec/Stream.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <sys/stat.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vthost/keeper/Codec.hpp>
#include <vthost/keeper/Frame.hpp>
#include <vthost/keeperlink/KeeperAttachment.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>

using namespace std::chrono_literals;
using vthost::keeper::UniqueFd;
using vthost::keeperlink::KeeperChannel;
using vthost::keeperlink::KeeperLink;
using vthost::keeperlink::ReceivedMessage;

namespace keeper = vthost::keeper;

namespace
{

/// send()'s flag for no SIGPIPE from a dead peer; none where there is none, as KeeperChannel then sets
/// SO_NOSIGPIPE on the socket instead (macOS).
#ifdef MSG_NOSIGNAL
constexpr auto NoSigPipe = MSG_NOSIGNAL;
#else
constexpr auto NoSigPipe = 0;
#endif

/// The two ends of a channel: the daemon's, for the link under test, and the keeper's, played by the test.
struct ChannelPair
{
    UniqueFd daemon;
    UniqueFd keeper;
};

ChannelPair makeChannelPair()
{
    auto ends = std::array { -1, -1 };
    // Close-on-exec set after the fact: macOS has no SOCK_CLOEXEC.
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0);
    for (auto const fd: ends)
        REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    return ChannelPair { .daemon = UniqueFd { ends[0] }, .keeper = UniqueFd { ends[1] } };
}

/// The keeper's end, as the test plays it.
struct KeeperEnd
{
    explicit KeeperEnd(UniqueFd socket): channel { std::move(socket) } {}

    core::platform::SteadyClock clock;
    KeeperChannel channel;

    /// For a thread of the test's other than its main one, which may not assert.
    /// @return The next message, or nullopt.
    [[nodiscard]] std::optional<ReceivedMessage> tryReceive()
    {
        auto received = channel.receive(clock, clock.now() + 5s);
        if (!received || !received->message)
            return std::nullopt;
        return std::move(*received);
    }

    /// For a thread other than the test's main one, which may not assert.
    /// @return Whether it went.
    bool trySend(keeper::Message const& message, std::vector<int> const& fds = {})
    {
        return channel.sendNow(message, fds, clock, clock.now() + 5s).has_value();
    }

    [[nodiscard]] ReceivedMessage receive()
    {
        auto received = channel.receive(clock, clock.now() + 5s);
        REQUIRE(received.has_value());
        REQUIRE(received->message.has_value());
        return std::move(*received);
    }

    void send(keeper::Message const& message, std::vector<int> const& fds = {})
    {
        REQUIRE(channel.sendNow(message, fds, clock, clock.now() + 5s).has_value());
    }
};

/// Turns @p loop until @p done holds, for at most five seconds.
/// @return Whether it came to hold.
bool turnUntil(core::net::EventLoop& loop, std::function<bool()> const& done)
{
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::ignore = loop.runOnce(10ms);
    }
    return true;
}

/// @return Whether @p a and @p b name the same open file.
bool sameFile(int a, int b)
{
    struct stat first {};
    struct stat second {};
    REQUIRE(::fstat(a, &first) == 0);
    REQUIRE(::fstat(b, &second) == 0);
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

/// A pipe, whose read end a test sends and compares against.
struct Pipe
{
    UniqueFd reader;
    UniqueFd writer;
};

Pipe makePipe()
{
    auto fds = std::array { -1, -1 };
    // Close-on-exec set after the fact: macOS has no pipe2().
    REQUIRE(::pipe(fds.data()) == 0);
    for (auto const fd: fds)
        REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    return Pipe { .reader = UniqueFd { fds[0] }, .writer = UniqueFd { fds[1] } };
}

/// A link on a loop of its own, with the keeper's end beside it.
struct LinkHarness
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    ChannelPair ends = makeChannelPair();
    KeeperEnd keeperEnd { std::move(ends.keeper) };
    int losses = 0;
    std::optional<KeeperLink> link;

    /// @param spawnReplyBound How long a spawn waits for the keeper's answer.
    /// @param heartbeatInterval How often a wait for an answer beats.
    explicit LinkHarness(std::chrono::milliseconds spawnReplyBound = vthost::keeperlink::SpawnReplyBound,
                         std::chrono::milliseconds heartbeatInterval = vthost::KeeperHeartbeatInterval)
    {
        link.emplace(
            loop,
            KeeperChannel { std::move(ends.daemon) },
            keeper::ChannelVersion,
            [this] { ++losses; },
            spawnReplyBound,
            heartbeatInterval);
    }
};

} // namespace

TEST_CASE("an Exited arriving during a spawn is handled after it, not inside", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    auto const child = makePipe();
    auto const pidfd = makePipe();

    // The keeper answers the Spawn, but tells of a shell's exit first.
    auto spawnFdCount = std::size_t { 0 };
    auto keeperSide = std::thread { [&] {
        auto const received = h.keeperEnd.tryReceive();
        auto const* spawn = received ? std::get_if<keeper::Spawn>(&*received->message) : nullptr;
        if (spawn == nullptr)
            return;
        spawnFdCount = received->fds.size();
        h.keeperEnd.trySend(
            keeper::Exited { .session = 7, .status = { .kind = keeper::ExitKind::Normal, .value = 3 } });
        h.keeperEnd.trySend(keeper::Spawned { .request = spawn->request, .session = 8, .pid = 4242 },
                            { pidfd.reader.get() });
    } };

    enum class Where : uint8_t
    {
        Outside = 0,
        InsideSpawn,
    };
    auto where = Where::Outside;
    auto exits = std::vector<std::pair<keeper::KeeperSessionId, Where>> {};
    h.link->attachChildren(vthost::keeperlink::ChildEvents {
        .exited =
            [&](keeper::KeeperSessionId session, vtpty::ExitStatus status) {
                CHECK(std::get<vtpty::NormalExit>(status).exitCode == 3);
                exits.emplace_back(session, where);
            },
        .orphaned = {},
    });

    auto reply = std::optional<std::expected<vthost::keeperlink::SpawnReply, keeper::SpawnError>> {};
    h.loop.post([&] {
        where = Where::InsideSpawn;
        reply = h.link->spawn(keeper::Spawn { .request = 0,
                                              .argv = { "/bin/sh" },
                                              .executablePath = "/bin/sh",
                                              .childFds = { 0 },
                                              .kept = {} },
                              { child.writer.get() });
        where = Where::Outside;
        CHECK(exits.empty()); // not inside
    });
    REQUIRE(turnUntil(h.loop, [&] { return !exits.empty(); }));
    keeperSide.join();

    CHECK(spawnFdCount == 1);
    REQUIRE(reply.has_value());
    REQUIRE(reply->has_value());
    CHECK((*reply)->spawned.pid == 4242);
    CHECK((*reply)->spawned.session == 8);
    REQUIRE((*reply)->processHandle.isOpen());
    CHECK(sameFile((*reply)->processHandle.get(), pidfd.reader.get()));
    REQUIRE(exits.size() == 1);
    CHECK(exits.front().first == keeper::KeeperSessionId { 7 });
    CHECK(exits.front().second == Where::Outside);
    CHECK(h.losses == 0);
}

TEST_CASE("every frame that carries descriptors is sent alone, its descriptors on its first byte",
          "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    auto const first = makePipe();
    auto const second = makePipe();

    // Queued before the loop turns, so that the writer sends them as one run: plain frames around and
    // between two that carry a descriptor each.
    h.link->post(keeper::Heartbeat {});
    h.link->post(keeper::Listener { .name = "a", .tlsIdentity = std::nullopt, .path = "/a" },
                 { first.reader.get() });
    h.link->post(keeper::Heartbeat {});
    h.link->post(keeper::Listener { .name = "b", .tlsIdentity = std::nullopt, .path = "/b" },
                 { second.reader.get() });
    h.link->post(keeper::Heartbeat {});
    std::ignore = h.loop.runOnce(10ms);

    // The keeper's FrameAssembler binds a read's descriptors to the first frame in it that announces any: two
    // such frames in one send would hand both descriptors to the first, and break the stream.
    auto names = std::vector<std::string> {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 5))
    {
        auto const received = h.keeperEnd.receive();
        if (auto const* listener = std::get_if<keeper::Listener>(&*received.message))
        {
            names.push_back(listener->name);
            REQUIRE(received.fds.size() == 1);
            CHECK(
                sameFile(received.fds.front().get(), (listener->name == "a" ? first : second).reader.get()));
        }
        else
        {
            CHECK(std::holds_alternative<keeper::Heartbeat>(*received.message));
            CHECK(received.fds.empty());
        }
    }
    CHECK(names == std::vector<std::string> { "a", "b" });
}

TEST_CASE("frames posted from another thread reach the keeper in order", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    constexpr auto Count = 200;
    auto received = std::vector<int> {};
    auto readAll = std::atomic<bool> { false };
    auto reader = std::thread { [&] {
        for ([[maybe_unused]] auto const index: std::views::iota(0, Count))
        {
            auto const message = h.keeperEnd.tryReceive();
            auto const* shutdown = message ? std::get_if<keeper::Shutdown>(&*message->message) : nullptr;
            if (shutdown == nullptr)
                break;
            received.push_back(shutdown->cause);
        }
        readAll.store(true);
    } };
    auto poster = std::thread { [&] {
        for (auto const index: std::views::iota(0, Count))
            h.link->post(keeper::Shutdown { .cause = static_cast<uint8_t>(index) });
    } };
    // The loop flushes while the poster still enqueues: the queue is raced from both sides.
    REQUIRE(turnUntil(h.loop, [&] { return readAll.load(); }));
    poster.join();
    reader.join();
    auto const expected = std::ranges::to<std::vector>(std::views::iota(0, Count));
    CHECK(received == expected);
}

TEST_CASE("a keeper that closes its end is reported lost once", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    h.keeperEnd.channel = KeeperChannel { UniqueFd {} };
    REQUIRE(turnUntil(h.loop, [&] { return h.losses > 0; }));
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    CHECK(h.losses == 1);
    CHECK(h.link->lost());
    // A spawn on a lost link fails at once, rather than waiting for an answer that cannot come.
    auto const spawned = h.link->spawn(keeper::Spawn {}, {});
    REQUIRE_FALSE(spawned.has_value());
    CHECK(spawned.error() == keeper::SpawnError::Unknown);
}

TEST_CASE("a link that goes without an announced shutdown sends nothing still queued", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    // Posted while the loop does not run, as a host that an exception unwinds out of runDaemon() releases its
    // sessions: queued, not sent.
    h.link->post(keeper::Release { .session = 7, .graceMs = 0 });
    h.link.reset();
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    // The keeper sees its daemon's end without a Release: a crash, after which it keeps the session.
    auto const received = h.keeperEnd.channel.receive(h.keeperEnd.clock, h.keeperEnd.clock.now() + 5s);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error() == vthost::keeperlink::ChannelError::Closed);
}

TEST_CASE("a sealed link sends nothing posted after it, even as the loop turns", "[keeper][keeperlink]")
{
    // As the host goes after an end the daemon did not announce: its sessions' Releases, and a turn of the
    // loop (a host with extensions turns it once).
    auto h = LinkHarness {};
    h.link->seal();
    h.link->post(keeper::Release { .session = 7, .graceMs = 0 });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    auto const received = h.keeperEnd.channel.receive(h.keeperEnd.clock, h.keeperEnd.clock.now() + 200ms);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error() == vthost::keeperlink::ChannelError::TimedOut);
}

TEST_CASE("a link sealed after an announced shutdown sends on", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    h.link->announceShutdown(3, 50ms); // the keeper takes no note: the wait ends at the bound
    h.link->seal();
    h.link->post(keeper::Release { .session = 7, .graceMs = 0 });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    CHECK(std::holds_alternative<keeper::Shutdown>(*h.keeperEnd.receive().message));
    CHECK(std::holds_alternative<keeper::Release>(*h.keeperEnd.receive().message));
}

TEST_CASE("after an announced shutdown, what is still queued goes as the link does", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    h.link->announceShutdown(3, 50ms); // the keeper takes no note: the wait ends at the bound
    h.link->post(keeper::Release { .session = 7, .graceMs = 0 });
    h.link.reset();
    auto const shutdown = h.keeperEnd.receive();
    REQUIRE(std::holds_alternative<keeper::Shutdown>(*shutdown.message));
    CHECK(std::get<keeper::Shutdown>(*shutdown.message).cause == 3);
    auto const release = h.keeperEnd.receive();
    REQUIRE(std::holds_alternative<keeper::Release>(*release.message));
    CHECK(std::get<keeper::Release>(*release.message).session == 7);
}

TEST_CASE("a lost keeper orphans the children before the loss is reported", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    auto order = std::vector<std::string> {};
    h.link->attachChildren(vthost::keeperlink::ChildEvents {
        .exited = {},
        .orphaned = [&] { order.emplace_back(h.losses == 0 ? "orphaned" : "orphaned after the loss"); },
    });
    h.keeperEnd.channel = KeeperChannel { UniqueFd {} };
    REQUIRE(turnUntil(h.loop, [&] { return h.losses > 0; }));
    CHECK(order == std::vector<std::string> { "orphaned" });
}

TEST_CASE("a spawn the keeper answers after it was given up on is released at once", "[keeper][keeperlink]")
{
    auto h = LinkHarness { 100ms };
    auto const spawned = h.link->spawn(keeper::Spawn {}, {});
    REQUIRE_FALSE(spawned.has_value());
    CHECK(spawned.error() == keeper::SpawnError::Unknown);

    // The keeper took the Spawn, and answers it late.
    auto const request = std::get<keeper::Spawn>(*h.keeperEnd.receive().message).request;
    h.keeperEnd.send(keeper::Spawned { .request = request, .session = 5, .pid = 4242 });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    auto const released = h.keeperEnd.receive();
    CHECK(std::get<keeper::Release>(*released.message) == keeper::Release { .session = 5, .graceMs = 0 });

    // Answered once: the same answer again releases nothing more.
    h.keeperEnd.send(keeper::Spawned { .request = request, .session = 5, .pid = 4242 });
    h.keeperEnd.send(keeper::SpawnFailed { .request = request + 1, .error = keeper::SpawnError::ForkFailed });
    h.link->post(keeper::Heartbeat {});
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    CHECK(std::holds_alternative<keeper::Heartbeat>(*h.keeperEnd.receive().message));
    CHECK_FALSE(h.link->lost());
}

TEST_CASE("a spawn that waits for the keeper beats for the loop it holds", "[keeper][keeperlink]")
{
    // A keeper restarts a daemon whose heartbeats stop for its timeout, which may be as short as
    // keeper::MinHeartbeatTimeout: a spawn that held the loop longer, silent, would be taken for a hang.
    auto h = LinkHarness { 300ms, 40ms };
    REQUIRE_FALSE(h.link->spawn(keeper::Spawn {}, {}).has_value());
    CHECK(std::holds_alternative<keeper::Spawn>(*h.keeperEnd.receive().message));
    auto beats = 0;
    while (true)
    {
        auto const received = h.keeperEnd.channel.receive(h.keeperEnd.clock, h.keeperEnd.clock.now() + 100ms);
        if (!received)
            break;
        REQUIRE(std::holds_alternative<keeper::Heartbeat>(*received->message));
        ++beats;
    }
    CHECK(beats >= 3);
    CHECK_FALSE(h.link->lost());
}

TEST_CASE("a late SpawnFailed for a spawn given up on forgets it", "[keeper][keeperlink]")
{
    auto h = LinkHarness { 100ms };
    REQUIRE_FALSE(h.link->spawn(keeper::Spawn {}, {}).has_value());
    auto const request = std::get<keeper::Spawn>(*h.keeperEnd.receive().message).request;

    // The keeper refuses it late: the request is answered, and forgotten.
    h.keeperEnd.send(keeper::SpawnFailed { .request = request, .error = keeper::SpawnError::ForkFailed });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    // A Spawned under the same number now names nothing given up on: nothing is released for it.
    h.keeperEnd.send(keeper::Spawned { .request = request, .session = 5, .pid = 4242 });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    // Posted once the Spawned was handled: a Release it caused would be on the channel before it.
    h.link->post(keeper::Heartbeat {});
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);
    CHECK(std::holds_alternative<keeper::Heartbeat>(*h.keeperEnd.receive().message));
    CHECK_FALSE(h.link->lost());
}

TEST_CASE("the handshake records the channel version both ends speak", "[keeper][keeperlink]")
{
    auto ends = makeChannelPair();
    auto keeperEnd = KeeperEnd { std::move(ends.keeper) };
    keeperEnd.send(keeper::Hello { .version = keeper::ChannelVersion + 3, .capabilities = 0 });
    keeperEnd.send(keeper::Launch {}); // a fresh start
    auto const clock = core::platform::SteadyClock {};
    auto const attached = vthost::keeperlink::attachToKeeper(std::move(ends.daemon), clock, 5s);
    REQUIRE(attached.has_value());
    CHECK(attached->version == keeper::ChannelVersion); // the lower of the two
}

TEST_CASE("the channel's socket is close-on-exec", "[keeper][keeperlink]")
{
    // Made without close-on-exec, so that the flag is the channel's doing.
    auto ends = std::array { -1, -1 };
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0);
    auto const peer = UniqueFd { ends[1] };
    auto const channel = KeeperChannel { UniqueFd { ends[0] } };
    CHECK((::fcntl(channel.handle(), F_GETFD) & FD_CLOEXEC) != 0);
}

TEST_CASE("a frame whose descriptor cannot be duplicated is dropped and the link stays up",
          "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    // No descriptor at all, so that nothing opened meanwhile can make it one.
    auto const none = -1;
    h.link->post(keeper::Structure {}, { none });
    h.link->post(keeper::Heartbeat {});
    std::ignore = h.loop.runOnce(10ms);

    // Only the frame after it arrives: the dropped one never reached the socket.
    auto const next = h.keeperEnd.receive();
    CHECK(std::holds_alternative<keeper::Heartbeat>(*next.message));
    CHECK(next.fds.empty());
    CHECK_FALSE(h.link->lost());
    CHECK(h.losses == 0);
}

TEST_CASE("a message the daemon cannot read is answered Unsupported", "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    auto expected = uint64_t { 0 };
    auto surplus = std::optional<Pipe> {};
    auto bytes = std::vector<uint8_t> {};
    auto fds = std::vector<int> {};
    SECTION("an unknown tag")
    {
        auto body = core::codec::Writer {};
        body.varint(4000);
        expected = 4000;
        bytes = keeper::encodeFrame(keeper::Frame { .body = std::move(body).take(), .fds = {} });
    }
    SECTION("an enum value beyond the wire table")
    {
        auto body =
            keeper::encode(keeper::SpawnFailed { .request = 1, .error = keeper::SpawnError::ForkFailed });
        REQUIRE(body.back() == 1); // the error's wire index, last
        body.back() = 9;           // a value a later version appended
        expected = keeper::Message { keeper::SpawnFailed {} }.index() + 1;
        bytes = keeper::encodeFrame(keeper::Frame { .body = std::move(body), .fds = {} });
    }
    SECTION("more descriptors than the message may carry")
    {
        surplus.emplace(makePipe());
        fds = { surplus->writer.get() };
        expected = keeper::Message { keeper::Heartbeat {} }.index() + 1;
        bytes =
            keeper::encodeFrame(keeper::Frame { .body = keeper::encode(keeper::Heartbeat {}), .fds = fds });
    }
    auto const runs = std::array { std::span<uint8_t const> { bytes } };
    REQUIRE(h.keeperEnd.channel.sendSome(runs, fds) == bytes.size());
    if (surplus)
        surplus->writer.reset(); // the daemon holds the one copy left

    auto answer = std::optional<keeper::Unsupported> {};
    auto answered = std::atomic<bool> { false };
    auto reader = std::thread { [&] {
        if (auto const received = h.keeperEnd.tryReceive())
            if (auto const* unsupported = std::get_if<keeper::Unsupported>(&*received->message))
                answer = *unsupported;
        answered.store(true);
    } };
    REQUIRE(turnUntil(h.loop, [&] { return answered.load(); }));
    reader.join();
    CHECK(answer == keeper::Unsupported { .tag = expected });
    CHECK(h.losses == 0);
    if (surplus)
    {
        // Closed with the message: the read end sees the end of the file.
        auto byte = char {};
        CHECK(::read(surplus->reader.get(), &byte, 1) == 0);
    }
}

TEST_CASE("an Exited read together with a spawn's answer is handled without more from the keeper",
          "[keeper][keeperlink]")
{
    auto h = LinkHarness {};
    // The link's reader parks on the empty socket first.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 3))
        std::ignore = h.loop.runOnce(10ms);

    // The keeper answers while spawn() waits, and the answer and an Exited go in one send: the read that
    // brings the answer brings the Exited too, and then the socket is empty again.
    auto answered = std::atomic<bool> { false };
    auto keeperSide = std::thread { [&] {
        auto const received = h.keeperEnd.tryReceive();
        auto const* spawn = received ? std::get_if<keeper::Spawn>(&*received->message) : nullptr;
        if (spawn == nullptr)
            return;
        auto const answer = vthost::keeperlink::frameBytesOf(
            keeper::SpawnFailed { .request = spawn->request, .error = keeper::SpawnError::ForkFailed }, 0);
        auto const exited = vthost::keeperlink::frameBytesOf(
            keeper::Exited { .session = 5, .status = { .kind = keeper::ExitKind::Normal, .value = 0 } }, 0);
        auto const runs =
            std::array { std::span<uint8_t const> { answer }, std::span<uint8_t const> { exited } };
        answered.store(h.keeperEnd.channel.sendSome(runs, {}) == answer.size() + exited.size());
    } };

    auto exits = std::vector<keeper::KeeperSessionId> {};
    h.link->attachChildren(vthost::keeperlink::ChildEvents {
        .exited = [&](keeper::KeeperSessionId session, vtpty::ExitStatus) { exits.push_back(session); },
        .orphaned = {},
    });
    auto reply = std::optional<std::expected<vthost::keeperlink::SpawnReply, keeper::SpawnError>> {};
    h.loop.post([&] { reply = h.link->spawn(keeper::Spawn {}, {}); });
    REQUIRE(turnUntil(h.loop, [&] { return reply.has_value(); }));
    keeperSide.join();
    REQUIRE(answered.load());
    REQUIRE_FALSE(reply->has_value());
    CHECK(reply->error() == keeper::SpawnError::ForkFailed);

    // The keeper sends nothing more: the Exited must not wait on the channel for its next frame.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 20))
        std::ignore = h.loop.runOnce(10ms);
    CHECK(exits == std::vector { keeper::KeeperSessionId { 5 } });
}

TEST_CASE("a descriptor that is no keeper's channel is refused before anything is written to it",
          "[keeper][keeperlink]")
{
    auto pipe = makePipe();
    REQUIRE(::fcntl(pipe.reader.get(), F_SETFL, O_NONBLOCK) == 0);
    auto const clock = core::platform::SteadyClock {};
    auto const attached = vthost::keeperlink::attachToKeeper(std::move(pipe.writer), clock, 1s);
    REQUIRE_FALSE(attached.has_value());
    CHECK(attached.error() == vthost::keeperlink::AttachError::NotAChannel);
    auto byte = char {};
    CHECK(::read(pipe.reader.get(), &byte, 1) == 0); // the write end is closed, and nothing is in the pipe
}

namespace
{
/// Frames large enough that a few fill any socket buffer, so that one is cut short by a full socket.
constexpr auto BulkFrames = 48;

/// @return A frame of about 128 KiB that carries no descriptor.
[[nodiscard]] keeper::Message bulkFrame(int index)
{
    return keeper::Listener { .name = std::string(std::size_t { 128 } * 1024,
                                                  static_cast<char>('a' + (index % 26))),
                              .tlsIdentity = std::nullopt,
                              .path = std::to_string(index) };
}

/// Reads frames off the keeper's end on a thread of its own until an Exec, which it records.
/// Whether the keeper the test plays takes note of an Exec.
enum class Noting : uint8_t
{
    Silent = 0, ///< It reads the Exec and says nothing: a keeper stuck, or one that knows no ExecNoted.
    Notes,      ///< It answers ExecNoted, as the keeper does.
};

struct ExecReader
{
    explicit ExecReader(KeeperEnd& end, Noting noting = Noting::Notes): _end { end }, _noting { noting } {}
    ExecReader(ExecReader const&) = delete;
    ExecReader& operator=(ExecReader const&) = delete;
    ExecReader(ExecReader&&) = delete;
    ExecReader& operator=(ExecReader&&) = delete;
    ~ExecReader() { join(); }

    void start()
    {
        _thread = std::thread { [this] {
            while (auto message = _end.tryReceive())
            {
                if (auto const* exec = std::get_if<keeper::Exec>(&*message->message))
                {
                    execPath = exec->executablePath;
                    if (_noting == Noting::Notes)
                        _end.trySend(keeper::ExecNoted {});
                    break;
                }
                if (auto const* listener = std::get_if<keeper::Listener>(&*message->message))
                    paths.push_back(listener->path);
            }
            done.store(true);
        } };
    }

    void join()
    {
        if (_thread.joinable())
            _thread.join();
    }

    std::vector<std::string> paths; ///< Of each bulk frame read whole, in order.
    std::string execPath;           ///< The Exec's path; empty for none.
    std::atomic<bool> done { false };

  private:
    KeeperEnd& _end;
    Noting _noting;
    std::thread _thread;
};

/// @return The paths the bulk frames carry, in order.
[[nodiscard]] std::vector<std::string> bulkPaths()
{
    return std::ranges::to<std::vector>(
        std::views::iota(0, BulkFrames)
        | std::views::transform([](int index) { return std::to_string(index); }));
}
} // namespace

TEST_CASE("a live upgrade's exec is announced only once every queued frame went whole",
          "[keeper][keeperlink][upgrade]")
{
    auto h = LinkHarness {};
    for (auto const index: std::views::iota(0, BulkFrames))
        h.link->post(bulkFrame(index));
    auto reader = ExecReader { h.keeperEnd };
    reader.start();

    auto const announced = h.link->announceExec("/state/bin/0.9.0-bbbbbbbb/contour", 10s);
    REQUIRE(announced.has_value());
    // Everything up to the Exec reached the keeper whole and in order: the new image's channel starts on a
    // frame boundary, with nothing of the old image's left to finish.
    reader.join();
    CHECK(reader.paths == bulkPaths());
    CHECK(reader.execPath == "/state/bin/0.9.0-bbbbbbbb/contour");
}

TEST_CASE("an exec the keeper does not read for in time is refused, and the frame cut short goes on whole",
          "[keeper][keeperlink][upgrade]")
{
    auto h = LinkHarness {};
    for (auto const index: std::views::iota(0, BulkFrames))
        h.link->post(bulkFrame(index));

    // The keeper reads nothing: the socket fills, a frame is cut short, and the bound passes.
    auto const announced = h.link->announceExec("/state/bin/0.9.0-bbbbbbbb/contour", 200ms);
    REQUIRE_FALSE(announced.has_value());
    CHECK(announced.error() == vthost::keeperlink::ChannelError::TimedOut);
    CHECK_FALSE(h.link->lost());

    // The daemon serves on: once the keeper reads again, the rest follows, every frame whole.
    auto reader = ExecReader { h.keeperEnd, Noting::Silent };
    reader.start();
    REQUIRE(turnUntil(h.loop, [&] { return reader.done.load(); }));
    reader.join();
    CHECK(reader.paths == bulkPaths());
}

TEST_CASE("an exec announced to a keeper that is gone is refused", "[keeper][keeperlink][upgrade]")
{
    auto h = LinkHarness {};
    h.keeperEnd.channel = KeeperChannel { UniqueFd {} };
    REQUIRE(turnUntil(h.loop, [&] { return h.link->lost(); }));
    CHECK_FALSE(h.link->announceExec("/state/bin/0.9.0-bbbbbbbb/contour").has_value());
}

TEST_CASE("a keeper frame part-sent as the daemon announces its exec reaches the new image whole",
          "[keeper][keeperlink][upgrade]")
{
    auto h = LinkHarness {};
    auto exits = std::vector<uint64_t> {};
    h.link->attachChildren(vthost::keeperlink::ChildEvents {
        .exited = [&](keeper::KeeperSessionId session,
                      vtpty::ExitStatus) { exits.push_back(std::to_underlying(session)); },
        .orphaned = [] {},
    });
    // The keeper wrote half of an Exited when the daemon decided to exec: it sits in the read buffer.
    auto const exited = vthost::keeperlink::frameBytesOf(
        keeper::Exited { .session = 7, .status = { .kind = keeper::ExitKind::Normal, .value = 0 } }, 0);
    auto const half = exited.size() / 2;
    REQUIRE(::send(h.keeperEnd.channel.handle(), exited.data(), half, NoSigPipe)
            == static_cast<ssize_t>(half));
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
        std::ignore = h.loop.runOnce(10ms);

    // The keeper finishes the frame it began, then takes note, as it does on Exec.
    auto keeperSide = std::thread { [&] {
        while (auto message = h.keeperEnd.tryReceive())
            if (std::holds_alternative<keeper::Exec>(*message->message))
                break;
        std::ignore =
            ::send(h.keeperEnd.channel.handle(), exited.data() + half, exited.size() - half, NoSigPipe);
        h.keeperEnd.trySend(keeper::ExecNoted {});
    } };
    auto const announced = h.link->announceExec("/state/bin/0.9.0-bbbbbbbb/contour", 5s);
    keeperSide.join();
    REQUIRE(announced.has_value());
    // Read whole and handled by the image that execs, before the exec: nothing of it is left behind.
    CHECK(exits == std::vector<uint64_t> { 7 });

    // The new image inherits the channel on a frame boundary: what the keeper sends it next is read whole.
    auto inherited = KeeperChannel { UniqueFd { ::fcntl(h.link->channelHandle(), F_DUPFD_CLOEXEC, 0) } };
    h.keeperEnd.send(keeper::Hello {});
    auto const clock = core::platform::SteadyClock {};
    auto const received = inherited.receive(clock, clock.now() + 5s);
    REQUIRE(received.has_value());
    REQUIRE(received->message.has_value());
    CHECK(std::holds_alternative<keeper::Hello>(*received->message));
}

TEST_CASE("an exec the keeper takes no note of is called off", "[keeper][keeperlink][upgrade]")
{
    auto h = LinkHarness {};
    auto reader = ExecReader { h.keeperEnd, Noting::Silent };
    reader.start();
    auto const announced = h.link->announceExec("/state/bin/0.9.0-bbbbbbbb/contour", 300ms);
    reader.join();
    REQUIRE_FALSE(announced.has_value());
    CHECK(announced.error() == vthost::keeperlink::ChannelError::TimedOut);
    CHECK(reader.execPath == "/state/bin/0.9.0-bbbbbbbb/contour");
    // The keeper is told the exec did not happen, so that it sends what it held back.
    std::ignore = h.loop.runOnce(10ms);
    auto const aborted = h.keeperEnd.receive();
    CHECK(std::holds_alternative<keeper::ExecAborted>(*aborted.message));
    CHECK_FALSE(h.link->lost());
}

TEST_CASE("what the daemon answers to the keeper's last frames before the exec goes before the exec",
          "[keeper][keeperlink][upgrade]")
{
    auto h = LinkHarness {};
    // The keeper sends a message this daemon does not know, then takes note of the exec: the answer, an
    // Unsupported posted by the handler, must be on the wire whole before announceExec() returns.
    auto keeperSide = std::thread { [&] {
        while (auto message = h.keeperEnd.tryReceive())
            if (std::holds_alternative<keeper::Exec>(*message->message))
                break;
        auto const unknown =
            keeper::encodeFrame(keeper::Frame { .body = { 0xA0, 0x1F }, .fds = {} }); // tag 4000
        std::ignore = ::send(h.keeperEnd.channel.handle(), unknown.data(), unknown.size(), NoSigPipe);
        h.keeperEnd.trySend(keeper::ExecNoted {});
    } };
    auto const announced = h.link->announceExec("/state/bin/0.9.0-bbbbbbbb/contour", 5s);
    keeperSide.join();
    REQUIRE(announced.has_value());
    // Read without turning the loop again: it was sent before announceExec() returned.
    auto const answer = h.keeperEnd.receive();
    REQUIRE(std::holds_alternative<keeper::Unsupported>(*answer.message));
    CHECK(std::get<keeper::Unsupported>(*answer.message).tag == 4000);
}
