// SPDX-License-Identifier: Apache-2.0
//
// `ISocket::shutdownWrite` from the ACCEPTED side -- the direction every other case here misses.
//
// A socket an IOCP listener has accepted is not a socket the process opened: until its context is
// updated, `shutdown` on it fails and the half-close is a silent no-op -- no FIN reaches the peer,
// and the accepted end's own writes go on succeeding. **Cross-platform on purpose**: the defect was
// one platform's, and a case that runs only on that platform is the shape that cannot see two
// platforms disagreeing about the same verb.
//
// **Two assertions, and the second is the one that rots quietly**: the peer sees EOF, AND the
// shutting-down socket's own subsequent writes fail. An implementation that gets the first right
// and the second wrong passes a half-written port of this case.
//
// **Sequential on one loop, not two arms of a `whenAll`.** Upstream drives this over a reactor
// thread and a blocking connector on another; here a loopback `connect` completes out of the
// kernel's backlog before anything calls `accept`, so each step is already satisfiable when it is
// reached. That buys two things a raced pair does not: the accepted end provably has not CLOSED
// when the peer reads -- a close sends a FIN of its own, which would pass for the half-close's --
// and no arm can bail while its sibling is parked in `accept()` forever, turning this case's red
// into a hang (`.agent/rules/testing.md`).
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/BackendMatrix.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

using core::async::Task;
using core::net::EventLoop;
using core::net::IListener;
using core::net::IoResult;
using core::net::testing::BackendMatrix;

namespace
{

/// What the two ends observed. Every field is an `optional`, so "never got that far" and "got there
/// and the answer was wrong" are different readings rather than the same default.
struct Exchange
{
    std::optional<IoResult> writeAfterHalfClose; ///< The accepted end's write AFTER its half-close.
    std::optional<IoResult> peerRead;            ///< What the dialling end's read answered.
    bool connected = false;                      ///< Whether the dial succeeded at all.
    bool accepted = false;                       ///< Whether a connection was accepted at all.
    bool shutdownOk = false;                     ///< Whether the half-close itself reported success.
};

/// Dials, accepts, half-closes the accepted end, writes once more, and reads the peer.
///
/// The accepted socket stays alive across the peer's read -- it is a local of this flow, and the
/// read completes before the flow ends -- which is what makes the EOF the peer sees attributable
/// to `shutdownWrite` and to nothing else.
/// @param loop The loop both ends live on.
/// @param listener The bound listener to dial and accept on.
/// @param out Where the observations go.
Task<void> halfCloseExchange(EventLoop* loop, IListener* listener, Exchange* out)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", listener->boundPort());
    if (!connected.has_value())
        co_return;
    auto peer = std::move(*connected);
    out->connected = true;

    // Already sitting in the backlog, fully established: this resolves without needing the dial
    // to still be in flight.
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto server = std::move(*accepted);
    out->accepted = true;

    auto const halfClosed = co_await server->shutdownWrite();
    out->shutdownOk = halfClosed.has_value();

    // One byte, after the half-close. On a correct socket this FAILS -- the write half is gone --
    // and where the half-close was a no-op it succeeds, which is the half of the contract no peer
    // can observe.
    auto const one = std::array<std::byte, 1> { std::byte { 0x41 } };
    out->writeAfterHalfClose = co_await server->write(std::span<std::byte const> { one });

    auto buffer = std::array<std::byte, 8> {};
    out->peerRead = co_await peer->read(buffer);
}

} // namespace

TEST_CASE("An accepted socket's shutdownWrite reaches its peer as EOF, and its own writes then fail",
          "[net][socket][loopback]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not built on this platform
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());
            REQUIRE((*listener)->boundPort() != 0);

            auto exchange = Exchange {};
            loop.blockOn(halfCloseExchange(&loop, listener->get(), &exchange));

            REQUIRE(exchange.connected);
            REQUIRE(exchange.accepted);

            // Zero: the half-close itself succeeded. It is an awaitable now (it has to be, for a
            // decorator that must flush a `close_notify`), so it can report failure -- and a
            // half-close that failed would make the two assertions below meaningless rather than
            // false.
            CHECK(exchange.shutdownOk);

            // One: the FIN reached the peer, and it reads as a clean EOF rather than an error.
            REQUIRE(exchange.peerRead.has_value());
            REQUIRE(exchange.peerRead->has_value());
            CHECK(**exchange.peerRead == 0);

            // Two: the accepted end's own write, after its half-close, FAILED. This is the half a
            // peer cannot observe, and the half a no-op `shutdownWrite` gets wrong.
            REQUIRE(exchange.writeAfterHalfClose.has_value());
            CHECK_FALSE(exchange.writeAfterHalfClose->has_value());
        }
    }
}
