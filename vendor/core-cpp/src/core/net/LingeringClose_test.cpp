// SPDX-License-Identifier: Apache-2.0
//
// core-cpp#35, from fastcached's `Net/LingeringClose_test.cpp` at `0708dd54` (fastcached#1554).
// `closeLingering` is what keeps a refusal a server wrote over a request it did not finish reading
// from being destroyed by the reset a bare close sends. These cases pin its exits -- the peer
// closed first, the peer finished while it listened, the read cap, the byte cap, the deadline on
// each kind of socket -- and, over a real loopback pair, the refusal arriving intact where a bare
// close loses it.
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/LingeringClose.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

using core::async::syncRun;
using core::async::Task;
using core::net::closeLingering;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::LingerBounds;
using core::net::LingerEnd;
using core::net::LingerOutcome;
using core::net::testing::InMemorySocketPair;

namespace
{

using namespace std::chrono_literals;

/// @return @p text as the bytes a socket writes.
[[nodiscard]] std::span<std::byte const> asBytes(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

Task<bool> writeString(ISocket* socket, std::string_view payload)
{
    auto const result = co_await socket->write(asBytes(payload));
    co_return result.has_value() && *result == payload.size();
}

Task<bool> halfClose(ISocket* socket)
{
    co_return (co_await socket->shutdownWrite()).has_value();
}

/// One read, as it answered.
Task<core::net::IoResult> readOnce(ISocket* socket, std::span<std::byte> into)
{
    co_return co_await socket->read(into);
}

/// What the peer reads back: the text, and what ended it.
struct Received
{
    std::string text;           ///< Every byte read, in order.
    core::net::IoResult ending; ///< EOF as `0`, or the error that ended it.
};

/// Reads until EOF or an error, and keeps what ended it.
Task<Received> readToEnd(ISocket* socket)
{
    auto out = Received { .text = {}, .ending = core::net::IoResult { std::size_t { 0 } } };
    auto chunk = std::array<std::byte, 256> {};
    while (true)
    {
        auto const got = co_await socket->read(std::span<std::byte> { chunk });
        if (!got.has_value() || *got == 0)
        {
            out.ending = got;
            co_return out;
        }
        out.text.append(reinterpret_cast<char const*>(chunk.data()), *got);
    }
}

/// Lingers over @p socket and records how it ended, for a case that drives the loop by hand.
Task<void> lingerInto(ISocket* socket,
                      EventLoop* loop,
                      LingerBounds bounds,
                      std::optional<LingerOutcome>* out)
{
    *out = co_await closeLingering(socket, loop, bounds);
}

constexpr auto Generous = LingerBounds { .total = 1000ms, .maxBytes = std::size_t { 1 } << 20, .reads = 8 };

/// A request longer than the server reads of it before refusing, which is the whole shape.
constexpr std::string_view Request = "a request the server stopped reading after eight bytes, and refused";

/// What the server reads of `Request` before it refuses.
constexpr std::size_t ReadBeforeRefusing = 8;

/// The refusal.
constexpr std::string_view Refusal = "refused";

/// How the server end of a real pair closes after refusing.
enum class Closing : std::uint8_t
{
    Lingering, ///< Through `closeLingering`, as production closes.
    Bare,      ///< `close()` alone, which is the defect.
};

/// The server end of a real refusal, on the loop: read a little of the request, refuse, close.
/// @return How the linger ended, when it lingered.
Task<std::optional<LingerOutcome>> refuseOnLoop(EventLoop* loop,
                                                core::net::IListener* listener,
                                                Closing closing)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return std::nullopt;
    auto socket = std::move(*accepted);
    auto head = std::array<std::byte, ReadBeforeRefusing> {};
    static_cast<void>(co_await socket->read(std::span<std::byte> { head }));
    static_cast<void>(co_await socket->write(asBytes(Refusal)));
    if (closing == Closing::Lingering)
        co_return co_await closeLingering(socket.get(), loop, Generous);
    socket->close();
    co_return std::nullopt;
}

/// What a real refusal came to, on each side.
struct Refused
{
    bool served = false;                  ///< The server finished within its bound.
    std::optional<LingerOutcome> outcome; ///< How the linger ended, when it lingered.
    Received received;                    ///< What the client read.
};

/// Refuses a request over a real loopback pair, and returns what each side saw.
///
/// The server is a loop socket; the client is a blocking one on its own thread, which writes the
/// whole request at once -- so the part the server never reads is in its receive buffer when it
/// closes -- then reads to the end, then closes, as a well-behaved client does on seeing the
/// server's FIN.
/// @param closing How the server closes after refusing.
[[nodiscard]] Refused refuseOverLoopback(Closing closing)
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    REQUIRE(port != 0);

    auto refused =
        Refused { .served = false,
                  .outcome = std::nullopt,
                  .received = { .text = {}, .ending = core::net::IoResult { std::size_t { 0 } } } };
    {
        // `std::thread` with an explicit join rather than `std::jthread`: AppleClang's libc++ has no
        // `<stop_token>`, so it has no `jthread` either. The guard joins on every way out, so a
        // throw between here and the end of the block cannot leave a joinable thread behind.
        auto client = std::thread { [port, &refused] {
            auto connector = core::net::BlockingConnector {};
            auto socket = syncRun(
                connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
            if (!socket.has_value())
            {
                refused.received.ending = std::unexpected(socket.error());
                return;
            }
            // A read nothing answers is bounded rather than hung.
            (*socket)->setReceiveDeadline(5000ms);
            static_cast<void>(syncRun(writeString(socket->get(), Request)));
            refused.received = syncRun(readToEnd(socket->get()));
            (*socket)->close();
        } };
        auto const joinClient = core::net::detail::ScopeGuard { [&client]() noexcept { client.join(); } };

        // Bounded, and says so: a client that never connected leaves the accept parked.
        auto served =
            loop.blockOn(core::net::withTimeout(&loop, refuseOnLoop(&loop, listener->get(), closing), 10s));
        refused.served = served.has_value();
        if (served.has_value())
            refused.outcome = *served;
        if (!refused.served)
            (*listener)->close();
    }
    return refused;
}

} // namespace

TEST_CASE("closeLingering over a peer that closed first reads once and closes", "[net][linger]")
{
    // The ordinary goodbye, and the property that keeps it one: a peer that has finished costs a
    // single read, which answers EOF at once. A linger that waited here would turn every ending
    // into a delay.
    auto pair = InMemorySocketPair::create();
    pair.client->close();

    auto const outcome = syncRun(closeLingering(pair.server.get(), nullptr, Generous));

    CHECK(outcome.end == LingerEnd::PeerFinished);
    CHECK(outcome.reads == 1);
    CHECK(pair.server->isClosed());
}

TEST_CASE("closeLingering leaves an already closed socket closed, and reads nothing", "[net][linger]")
{
    auto pair = InMemorySocketPair::create();
    pair.server->close();

    auto const outcome = syncRun(closeLingering(pair.server.get(), nullptr, Generous));

    CHECK(outcome.end == LingerEnd::AlreadyClosed);
    CHECK(outcome.reads == 0);
    CHECK(pair.server->isClosed());
}

TEST_CASE("closeLingering drains a refused request and closes on the peer's EOF", "[net][linger]")
{
    // The reply is written while the peer's request is still unread -- a refusal of a request the
    // server did not finish reading. The peer has finished sending, so the linger reads the rest,
    // meets the EOF, and the peer reads the reply and then EOF.
    auto pair = InMemorySocketPair::create();
    REQUIRE(syncRun(writeString(pair.client.get(), Request)));
    REQUIRE(syncRun(halfClose(pair.client.get())));
    REQUIRE(syncRun(writeString(pair.server.get(), Refusal)));

    auto const outcome = syncRun(closeLingering(pair.server.get(), nullptr, Generous));
    CHECK(outcome.end == LingerEnd::PeerFinished);
    CHECK(outcome.reads == 2);

    auto const received = syncRun(readToEnd(pair.client.get()));
    CHECK(received.text == Refusal);
    REQUIRE(received.ending.has_value());
    CHECK(*received.ending == 0);
}

TEST_CASE("closeLingering stops at whichever cap a peer meets first", "[net][linger]")
{
    // Bounded best effort, and the bound is stated rather than hidden: the linger is never a
    // promise to read an unbounded upload in order to refuse it. Each cap is asserted with the
    // other set well clear of it, so the case says WHICH one stopped the drain.
    auto pair = InMemorySocketPair::create();
    auto const large = std::string(std::size_t { 64 } * 1024, 'u');
    REQUIRE(syncRun(writeString(pair.client.get(), large)));
    REQUIRE(syncRun(halfClose(pair.client.get())));

    SECTION("the reads")
    {
        auto const outcome = syncRun(
            closeLingering(pair.server.get(),
                           nullptr,
                           LingerBounds { .total = 1000ms, .maxBytes = large.size() * 2, .reads = 1 }));
        CHECK(outcome.end == LingerEnd::ReadCap);
        CHECK(outcome.reads == 1);
    }

    SECTION("the bytes")
    {
        auto const outcome = syncRun(closeLingering(
            pair.server.get(), nullptr, LingerBounds { .total = 1000ms, .maxBytes = 1, .reads = 8 }));
        CHECK(outcome.end == LingerEnd::ByteCap);
        CHECK(outcome.reads == 1);
    }

    CHECK(pair.server->isClosed());
}

TEST_CASE("closeLingering on a loop stops listening to a silent peer at its deadline", "[net][linger]")
{
    // A peer that neither sends nor closes: the linger parks, and only the deadline ends it.
    // Asserted parked BEFORE the clock moves, or a linger that never waited at all would pass.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto pair = InMemorySocketPair::create();

    auto outcome = std::optional<LingerOutcome> {};
    loop.spawn(lingerInto(
        pair.server.get(), &loop, LingerBounds { .total = 100ms, .maxBytes = 4096, .reads = 4 }, &outcome));
    std::ignore = loop.drain();
    REQUIRE_FALSE(outcome.has_value());
    CHECK_FALSE(pair.server->isClosed());

    clock.advance(99ms);
    std::ignore = loop.drain();
    CHECK_FALSE(outcome.has_value());

    clock.advance(1ms);
    std::ignore = loop.drain();
    REQUIRE(outcome.has_value());
    CHECK(outcome->end == LingerEnd::Expired);
    CHECK(outcome->reads == 1);
    CHECK(pair.server->isClosed());
}

TEST_CASE("closeLingering on a blocking socket stops listening to a silent peer", "[net][linger][socket]")
{
    // A blocking socket's reads BLOCK, so no loop deadline reaches them: each read carries its
    // share of the total. A real pair, because a receive deadline is a property of the kernel's
    // socket and the in-process one has no clock. The blocking end is the one that lingers; its
    // peer, accepted on a loop, stays open and silent throughout, so only the deadline can end
    // the linger, and a read with none would block this thread for good.
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    REQUIRE(port != 0);

    auto connector = core::net::BlockingConnector {};
    auto lingering =
        syncRun(connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
    REQUIRE(lingering.has_value());
    auto accepted = loop.blockOn(core::net::withTimeout(&loop, (*listener)->accept(), 5s));
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->has_value());
    auto& peer = **accepted;

    auto const outcome = syncRun(closeLingering(
        lingering->get(), nullptr, LingerBounds { .total = 200ms, .maxBytes = 4096, .reads = 2 }));
    CHECK(outcome.end == LingerEnd::Expired);
    CHECK(outcome.reads == 1);
    CHECK((*lingering)->isClosed());

    // And the close was the FIN the half-close promised: nothing of the peer's was unread.
    auto buffer = std::array<std::byte, 8> {};
    auto const got = loop.blockOn(
        core::net::withTimeout(&loop, readOnce(peer.get(), std::span<std::byte> { buffer }), 5s));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    CHECK(**got == 0);
}

TEST_CASE("A refusal written over an unread request reaches a real client intact only when the close lingers",
          "[net][linger][socket]")
{
    // The defect, on the wire rather than in a model of it. The server reads eight bytes of the
    // request, refuses, and closes with the rest still in its receive buffer. Measured on loopback
    // (fastcached#1553): the bare close is a reset, and Windows drops the refusal the client had
    // not read yet while Linux and macOS deliver it and then report the reset. So the refusal
    // arriving is not enough -- it must arrive AND end in the EOF of a close that was a FIN, which
    // is what fails on every platform when the close is bare.
    SECTION("lingering: the refusal, then EOF")
    {
        auto const refused = refuseOverLoopback(Closing::Lingering);
        REQUIRE(refused.served);
        CHECK(refused.received.text == Refusal);
        REQUIRE(refused.received.ending.has_value());
        CHECK(*refused.received.ending == 0);
        REQUIRE(refused.outcome.has_value());
        CHECK(refused.outcome->end == LingerEnd::PeerFinished);
    }

    SECTION("control, a bare close: a reset, with the refusal lost on Windows")
    {
        auto const refused = refuseOverLoopback(Closing::Bare);
        REQUIRE(refused.served);
        INFO("the client read \"" << refused.received.text << "\"");
        CHECK_FALSE(refused.received.ending.has_value());
    }
}
