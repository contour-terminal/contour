// SPDX-License-Identifier: Apache-2.0
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

    LinkHarness()
    {
        link.emplace(
            loop, KeeperChannel { std::move(ends.daemon) }, keeper::ChannelVersion, [this] { ++losses; });
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
    h.link->onExited([&](keeper::KeeperSessionId session, vtpty::ExitStatus status) {
        CHECK(std::get<vtpty::NormalExit>(status).exitCode == 3);
        exits.emplace_back(session, where);
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
    auto const closed = [] {
        auto const pipe = makePipe();
        return pipe.reader.get(); // closed as the pipe goes: no longer a descriptor of this process
    }();
    h.link->post(keeper::Structure {}, { closed });
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
    h.link->onExited([&](keeper::KeeperSessionId session, vtpty::ExitStatus) { exits.push_back(session); });
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
