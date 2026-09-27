// SPDX-License-Identifier: Apache-2.0
//
// What a blocking socket must do: answer every verb before its awaitable exists, never raise a
// fatal signal when the peer goes away, and honour the receive deadline the interface defines --
// including the zero that removes it.
//
// The server end is this platform's LOOP socket, accepted on a loop driven from this thread: a
// loopback dial completes out of the kernel's backlog before anything calls `accept`, so the
// blocking dial and the loop's accept never wait on each other, and the case needs no second
// thread except where a blocking call has to be answered while it blocks.
//
// Origin: fastcached `src/FastCache/Net/BlockingSocket_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`): the hung-up-peer, SIGPIPE-disposition and
// round-trip cases. Its listener-poll and `AcceptRaw` cases test `BlockingListener` and
// `Detail::AcceptRaw`, which core-cpp does not import; its send-flag case asserted a helper that no
// longer exists, and the hung-up-peer case below is the behaviour that helper was for.
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/net/IListener.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifndef _WIN32
    #include <csignal>
#endif

using namespace std::chrono_literals;
using core::async::syncRun;
using core::async::Task;
using core::net::BlockingConnector;
using core::net::BlockingConnectorOptions;
using core::net::DialOptions;
using core::net::IoResult;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::SocketResult;

namespace
{

/// A blocking client connected to a loop socket, both ends in hand.
struct Connected
{
    core::net::PlatformLoop loop; ///< Drives the accepted end.
    std::unique_ptr<core::net::IListener> listener;
    std::unique_ptr<ISocket> client;   ///< The @c BlockingSocket under test.
    std::unique_ptr<ISocket> accepted; ///< The loop's socket at the other end.
};

Task<SocketResult> acceptOne(core::net::IListener* listener)
{
    co_return co_await listener->accept();
}

Task<IoResult> writeOnce(ISocket* socket, std::span<std::byte const> bytes)
{
    co_return co_await socket->write(bytes);
}

Task<IoResult> readOnce(ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->read(buffer);
}

Task<IoResult> waitOnce(ISocket* socket)
{
    co_return co_await socket->waitReadable();
}

Task<std::expected<void, core::net::NetError>> shutOnce(ISocket* socket)
{
    co_return co_await socket->shutdownWrite();
}

/// How long any one wait in this file may take before the case fails instead of hanging.
constexpr auto WaitBound = 5s;

/// Reads once on the loop, bounded by @c WaitBound.
/// @param loop The loop @p socket belongs to.
/// @param socket The loop socket to read.
/// @param buffer Where the bytes go.
/// @return The read's answer, or `std::nullopt` when @c WaitBound passed first.
std::optional<IoResult> readOnLoop(core::net::EventLoop& loop, ISocket* socket, std::span<std::byte> buffer)
{
    return loop.blockOn(core::net::withTimeout(&loop, readOnce(socket, buffer), WaitBound));
}

/// Dials a loopback listener with a @c BlockingConnector and accepts the connection on a loop.
///
/// The client's reads and writes are bounded by @c WaitBound through the connector's `ioTimeout`,
/// so a blocking verb that should have returned fails the case rather than hanging it.
/// @param out Where both ends go; left without a client when the dial failed.
void connectPair(Connected& out)
{
    auto listener = core::net::listen(out.loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    out.listener = std::move(*listener);

    auto connector = BlockingConnector { core::net::defaultAddressResolver(),
                                         BlockingConnectorOptions { .ioTimeout = WaitBound } };
    auto client = syncRun(
        connector.connect("127.0.0.1", out.listener->boundPort(), DialOptions { .connectTimeout = 2s }));
    INFO("dial: " << (client.has_value() ? std::string { "connected" } : client.error().toString()));
    REQUIRE(client.has_value());
    out.client = std::move(*client);

    auto accepted = out.loop.blockOn(acceptOne(out.listener.get()));
    REQUIRE(accepted.has_value());
    out.accepted = std::move(*accepted);
}

} // namespace

TEST_CASE("A blocking socket round-trips bytes, and every verb answers before it is awaited",
          "[net][socket][blocking]")
{
    // Guards the over-correction: `MSG_NOSIGNAL` is passed on every send now, and a platform that
    // rejected it would fail every write -- which the hung-up-peer case below cannot tell from the
    // failure it is looking for.
    auto pair = Connected();
    connectPair(pair);

    auto const payload = std::array { std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 4 } };
    // `await_ready` is the whole of "never suspends", asserted on the awaitable itself: a blocking
    // transport that parked would make every `syncRun` over it throw.
    auto write = pair.client->write(payload);
    CHECK(write.await_ready());
    std::ignore = write.await_resume();

    auto received = std::array<std::byte, 8> {};
    auto const got = pair.loop.blockOn(readOnce(pair.accepted.get(), received));
    REQUIRE(got.has_value());
    CHECK(*got == payload.size());
    CHECK(std::ranges::equal(std::span { received }.first(*got), payload));
}

TEST_CASE("A write to a peer that hung up fails instead of killing the process", "[net][socket][blocking]")
{
    // Reaching the CHECK at all proves no SIGPIPE was raised -- a process-wide default disposition
    // would have ended the binary with signal 13 -- and the CHECK proves the failure came back as a
    // value. Chunked, because the first write after a hang-up is routinely accepted: it is the
    // peer's reset, arriving in response, that breaks the pipe.
    auto pair = Connected();
    connectPair(pair);
    pair.accepted->close();

    constexpr auto ChunkBytes = std::size_t { 256 } * 1024;
    constexpr int MaxChunks = 64; // 16 MiB is far past any loopback send buffer
    auto const chunk = std::vector<std::byte>(ChunkBytes, std::byte { 0xAB });

    auto const reported = std::ranges::any_of(std::views::iota(0, MaxChunks), [&](int /*attempt*/) {
        return !syncRun(writeOnce(pair.client.get(), chunk)).has_value();
    });
    CHECK(reported);
}

#ifndef _WIN32
namespace
{
[[nodiscard]] auto readSigPipeDisposition() noexcept -> void (*)(int)
{
    struct sigaction current {};
    if (::sigaction(SIGPIPE, nullptr, &current) != 0)
        return SIG_ERR;
    return current.sa_handler;
}

/// What this process was HANDED, read before any case runs. The case asserts a DELTA against it:
/// an ignored disposition is inherited across fork and exec, so the absolute `SIG_DFL` is a claim
/// about the whole process ancestry rather than about this library (fastcached#1229).
auto const InheritedSigPipeDisposition = readSigPipeDisposition();
} // namespace
#endif

TEST_CASE("Using a blocking socket leaves the process SIGPIPE disposition alone", "[net][socket][blocking]")
{
    // The obvious way to keep a broken pipe from killing a server is one `signal(SIGPIPE, SIG_IGN)`
    // at start-up, and it is wrong for any process that also spawns a child: the ignore is inherited
    // across exec, so it becomes a property of every program the process launches.
#ifdef _WIN32
    SKIP("Windows has no SIGPIPE; a write to a broken connection is an error return there");
#else
    auto const before = readSigPipeDisposition();
    REQUIRE(before != SIG_ERR);

    auto pair = Connected();
    connectPair(pair);
    std::ignore = syncRun(writeOnce(pair.client.get(), std::array { std::byte { 7 } }));

    auto const after = readSigPipeDisposition();
    CHECK(after == before);
    CHECK(after == InheritedSigPipeDisposition);
    if (InheritedSigPipeDisposition != SIG_DFL)
        WARN("this process was handed a non-default SIGPIPE disposition, so this case could only assert that "
             "nothing changed it, not that it is SIG_DFL");
#endif
}

TEST_CASE("A blocking socket's waitReadable blocks, and its count says which readable",
          "[net][socket][blocking]")
{
    // Inheriting the interface's answer-`1`-at-once default would turn a watch over a blocking socket
    // into a spin, and would report a peer that had gone as one with data waiting.
    auto pair = Connected();
    connectPair(pair);

    auto const payload = std::array { std::byte { 9 }, std::byte { 9 }, std::byte { 9 } };
    REQUIRE(pair.loop.blockOn(writeOnce(pair.accepted.get(), payload)).has_value());
    auto const pending = syncRun(waitOnce(pair.client.get()));
    REQUIRE(pending.has_value());
    CHECK(*pending == payload.size()); // consumed nothing

    auto drained = std::array<std::byte, 8> {};
    REQUIRE(syncRun(readOnce(pair.client.get(), drained)).has_value());

    pair.accepted->close();
    auto const eof = syncRun(waitOnce(pair.client.get()));
    REQUIRE(eof.has_value());
    CHECK(*eof == 0);
}

TEST_CASE("A blocking socket's half-close reaches the peer as EOF, and its own writes then fail",
          "[net][socket][blocking]")
{
    auto pair = Connected();
    connectPair(pair);

    REQUIRE(syncRun(shutOnce(pair.client.get())).has_value());
    auto buffer = std::array<std::byte, 4> {};
    auto const atPeer = readOnLoop(pair.loop, pair.accepted.get(), buffer);
    INFO("the peer's read waited " << WaitBound.count() << "s for the half-close's EOF");
    REQUIRE(atPeer.has_value());
    REQUIRE(atPeer->has_value());
    CHECK(**atPeer == 0);

    CHECK_FALSE(syncRun(writeOnce(pair.client.get(), std::array { std::byte { 1 } })).has_value());
    CHECK_FALSE(pair.client->isClosed()); // a half-close is not a close
}

TEST_CASE("A blocking socket's receive deadline expires, and a zero removes it",
          "[net][socket][blocking][deadline]")
{
    constexpr auto Bound = 150ms;

    SECTION("a positive deadline expires as a deadline, not as EOF")
    {
        auto pair = Connected();
        connectPair(pair);
        pair.client->setReceiveDeadline(Bound);

        auto buffer = std::array<std::byte, 4> {};
        auto const started = std::chrono::steady_clock::now();
        auto const got = syncRun(readOnce(pair.client.get(), buffer));
        auto const elapsed = std::chrono::steady_clock::now() - started;

        REQUIRE_FALSE(got.has_value());
        CHECK(core::net::isDeadlineExpiry(got.error().code));
        CHECK(elapsed >= Bound / 2); // it waited, rather than failing at once
    }

    SECTION("a zero lifts the bound: the read outlives it and takes the peer's late bytes")
    {
        // `ISocket::setReceiveDeadline`: non-positive REMOVES the bound. The peer writes from
        // another thread well after the old bound would have expired, and the read, blocked on this
        // one, must still be there to take it.
        auto pair = Connected();
        connectPair(pair);
        pair.client->setReceiveDeadline(Bound);
        pair.client->setReceiveDeadline(0ms);

        // The read below has no bound by design, so the peer half-closes after its write whatever
        // the write answered: a write that failed still ends the read, at EOF, and the case fails
        // on the write's answer rather than hanging in `recv`. The loop is driven from this thread
        // alone while the main thread blocks in the read, so it still has one thread at a time.
        auto wrote = std::optional<IoResult> {};
        auto late = std::thread { [&pair, &wrote, Bound] {
            std::this_thread::sleep_for(Bound * 3);
            wrote = pair.loop.blockOn(writeOnce(pair.accepted.get(), std::array { std::byte { 42 } }));
            std::ignore = pair.loop.blockOn(shutOnce(pair.accepted.get()));
        } };

        auto buffer = std::array<std::byte, 4> {};
        auto const got = syncRun(readOnce(pair.client.get(), buffer));
        late.join();
        REQUIRE(wrote.has_value());
        REQUIRE(wrote->has_value());
        REQUIRE(got.has_value());
        CHECK(*got == 1);
    }
}

TEST_CASE("Every verb on a closed blocking socket answers BadHandle", "[net][socket][blocking]")
{
    auto pair = Connected();
    connectPair(pair);
    pair.client->close();
    CHECK(pair.client->isClosed());

    auto buffer = std::array<std::byte, 4> {};
    auto const read = syncRun(readOnce(pair.client.get(), buffer));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == NetErrorCode::BadHandle);

    auto const written = syncRun(writeOnce(pair.client.get(), std::array { std::byte { 1 } }));
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == NetErrorCode::BadHandle);

    // And a half-close after a close is a successful no-op, as on every socket here.
    CHECK(syncRun(shutOnce(pair.client.get())).has_value());
}
