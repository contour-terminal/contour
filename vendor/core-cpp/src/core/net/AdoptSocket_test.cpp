// SPDX-License-Identifier: Apache-2.0
//
// `adoptSocket`: a connected socket accepted outside core-cpp, handed to a loop of the caller's
// choosing. It is how a Windows server spreads connections over several loops -- one thread
// accepts and deals each handle out -- since Windows has no SO_REUSEPORT and a completion-port
// association is one socket to one port. The cases accept with plain sockets calls, so nothing of
// core-cpp's own accept path is involved, and adopt onto a loop that did not accept.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/RawSockets.hpp>
#include <core/platform/Types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::testing::BackendMatrix;
using core::net::testing::closeRawSocket;
using core::net::testing::openRawTcpSocket;
using core::net::testing::rawLoopbackConnection;
using core::net::testing::rawReceive;
using core::net::testing::rawSendAll;
using core::net::testing::RawSocket;

namespace
{

/// Reads exactly @p expected.size() bytes from @p socket into @p matched's verdict.
Task<void> readExactly(ISocket* socket, std::string_view expected, bool* matched)
{
    auto buffer = std::array<std::byte, 16> {};
    auto total = std::size_t { 0 };
    while (total < expected.size())
    {
        auto const got = co_await socket->read(std::span<std::byte> { buffer }.subspan(total));
        if (!got.has_value() || *got == 0)
            break;
        total += *got;
    }
    *matched = total == expected.size() && std::memcmp(buffer.data(), expected.data(), expected.size()) == 0;
}

/// Writes @p payload to @p socket, recording whether all of it went.
Task<void> writeAll(ISocket* socket, std::string_view payload, bool* wrote)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(payload.data()), payload.size() };
    auto const sent = co_await socket->write(bytes);
    *wrote = sent.has_value() && *sent == payload.size();
}

} // namespace

TEST_CASE("A socket accepted outside core-cpp reads and writes on the loop it is adopted onto",
          "[net][adopt]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto servingBackend = core::net::makeBackend(backend.kind);
        if (!servingBackend)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            // The accept happened on this thread with no loop at all, as it does on a Windows
            // server's accepting thread; the loop the socket lands on is the caller's choice.
            auto servingLoop = EventLoop { *servingBackend };

            auto const connection = rawLoopbackConnection();
            REQUIRE(connection.client != core::platform::InvalidHandle);
            REQUIRE(connection.accepted != core::platform::InvalidHandle);
            auto const client = RawSocket { connection.client };

            auto adopted = core::net::adoptSocket(servingLoop, connection.accepted, "127.0.0.1:test");
            REQUIRE(adopted.has_value());
            CHECK((*adopted)->peerAddress() == "127.0.0.1:test");

            // The peer writes with a plain blocking send; the adopted socket reads it on its loop.
            constexpr auto Ping = std::string_view { "ping" };
            REQUIRE(rawSendAll(client.get(), Ping));
            auto matched = false;
            servingLoop.blockOn(readExactly(adopted->get(), Ping, &matched));
            CHECK(matched);

            // And the other way: the adopted socket writes on its loop, the peer reads blocking.
            constexpr auto Pong = std::string_view { "pong" };
            auto wrote = false;
            servingLoop.blockOn(writeAll(adopted->get(), Pong, &wrote));
            CHECK(wrote);
            auto reply = std::array<char, 4> {};
            auto received = std::size_t { 0 };
            while (received < reply.size())
            {
                auto const got = rawReceive(client.get(), std::span<char> { reply }.subspan(received));
                if (got <= 0)
                    break;
                received += static_cast<std::size_t>(got);
            }
            CHECK(std::string_view { reply.data(), received } == Pong);
        }
    }
}

TEST_CASE("Adopting an invalid handle is an error value, not an exception or a crash", "[net][adopt]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const refused = core::net::adoptSocket(loop, core::platform::InvalidHandle, {});
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == core::net::NetErrorCode::BadHandle);
        }
    }
}

TEST_CASE("Adopting a handle that is no longer a socket is refused on every backend", "[net][adopt]")
{
    // A handle value that passes the invalid-handle check and names nothing: each backend's first
    // use of it fails -- `fcntl` on POSIX, the completion-port association under IOCP, and
    // `WSAEventSelect` under WFMO, whose socket used to record that failure and adopt anyway, so the
    // caller got a socket that would never become ready.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            // The loop first, so nothing it opens can take the number the closed handle frees.
            auto loop = EventLoop { *source };
            auto const stale = openRawTcpSocket();
            REQUIRE(stale != core::platform::InvalidHandle);
            closeRawSocket(stale);

            auto const refused = core::net::adoptSocket(loop, stale, {});
            CHECK_FALSE(refused.has_value());
        }
    }
}
