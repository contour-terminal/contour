// SPDX-License-Identifier: Apache-2.0
//
// The one TCP client: `sendAll` and `receiveExactly` over a scripted socket, where a partial
// transfer is STATED rather than hoped for, and `connectTcp` over a real loopback connection, where
// what is asserted is that every phase is bounded.
//
// Origin: fastcached `src/FastCache/Net/TcpClient_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/TcpClient.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using core::async::syncRun;
using core::async::Task;
using core::net::IoAwaitable;
using core::net::IoResult;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::receiveExactly;
using core::net::sendAll;

namespace
{

/// A socket whose every call transfers what the script says, so a partial transfer is reproducible.
///
/// Running off the end of the script is an ERROR, which is how "it asked more times than it should
/// have" is caught rather than silently tolerated. Every answer is settled, so `syncRun` drives it.
class ScriptedSocket final: public ISocket
{
  public:
    /// @param chunks How many bytes each successive call transfers; zero is EOF for a read and a
    ///        write that took nothing.
    explicit ScriptedSocket(std::deque<std::size_t> chunks) noexcept: _chunks { std::move(chunks) } {}

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override
    {
        auto const next = take();
        if (!next.has_value())
            return IoAwaitable { std::unexpected(exhausted()) };
        auto const count = std::min(*next, buffer.size());
        std::ranges::fill(buffer.first(count), std::byte { 0x5A });
        return IoAwaitable { IoResult { count } };
    }

    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override
    {
        auto const next = take();
        if (!next.has_value())
            return IoAwaitable { std::unexpected(exhausted()) };
        auto const count = std::min(*next, buffer.size());
        _written.insert(_written.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        return IoAwaitable { IoResult { count } };
    }

    void close() noexcept override { _closed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _closed; }

    /// @return Everything @c write accepted, in order.
    [[nodiscard]] std::vector<std::byte> const& written() const noexcept { return _written; }

    /// @return How many calls the script still has left.
    [[nodiscard]] std::size_t remaining() const noexcept { return _chunks.size(); }

  private:
    [[nodiscard]] static core::net::NetError exhausted()
    {
        return core::net::makeNetError(NetErrorCode::ConnReset, 0, "script exhausted");
    }

    [[nodiscard]] std::optional<std::size_t> take()
    {
        if (_chunks.empty())
            return std::nullopt;
        auto const next = _chunks.front();
        _chunks.pop_front();
        return next;
    }

    std::deque<std::size_t> _chunks;
    std::vector<std::byte> _written;
    bool _closed { false };
};

/// @return @p count distinct-ish bytes.
[[nodiscard]] std::vector<std::byte> payload(std::size_t count)
{
    auto bytes = std::vector<std::byte>(count);
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        bytes[index] = static_cast<std::byte>(index & 0xFFU);
    return bytes;
}

Task<core::net::SocketResult> acceptOne(core::net::IListener* listener)
{
    co_return co_await listener->accept();
}

} // namespace

TEST_CASE("sendAll keeps writing until the whole buffer is gone", "[net][tcpclient]")
{
    // The defect is a SILENT one: a client that treats one short write as success sends a truncated
    // frame, and the peer then blocks waiting for the rest of a length it was promised.
    auto const bytes = payload(10);
    auto socket = ScriptedSocket { { 3, 3, 4 } };

    CHECK(syncRun(sendAll(&socket, bytes)));
    CHECK(socket.written() == bytes);
    CHECK(socket.remaining() == 0);
}

TEST_CASE("sendAll reports a write that fails part-way", "[net][tcpclient]")
{
    auto const bytes = payload(10);
    auto socket = ScriptedSocket { { 4 } }; // then the script runs out: an error
    CHECK_FALSE(syncRun(sendAll(&socket, bytes)));
}

TEST_CASE("sendAll treats a zero-length write as failure rather than looping", "[net][tcpclient]")
{
    // Without this the loop spins forever against a socket that accepts nothing but reports no
    // error -- a hang rather than a failure, the worse of the two.
    auto const bytes = payload(4);
    auto socket = ScriptedSocket { { 0, 0, 0 } };

    CHECK_FALSE(syncRun(sendAll(&socket, bytes)));
    CHECK(socket.remaining() == 2); // it gave up after the first, not after all three
}

TEST_CASE("sendAll of nothing succeeds without touching the socket", "[net][tcpclient]")
{
    auto socket = ScriptedSocket { {} };
    CHECK(syncRun(sendAll(&socket, {})));
}

TEST_CASE("receiveExactly keeps reading until it has the count it was asked for", "[net][tcpclient]")
{
    auto socket = ScriptedSocket { { 2, 5, 1 } };
    auto const got = syncRun(receiveExactly(&socket, 8));
    REQUIRE(got.has_value());
    CHECK(got->size() == 8);
    CHECK(socket.remaining() == 0);
}

TEST_CASE("receiveExactly reports a peer that closed before the count arrived", "[net][tcpclient]")
{
    // Zero from a read is EOF, and here it means a short frame: the peer declared a length it then
    // did not send. Retrying would wait forever.
    auto socket = ScriptedSocket { { 3, 0 } };
    CHECK_FALSE(syncRun(receiveExactly(&socket, 8)).has_value());
}

TEST_CASE("receiveExactly of zero bytes is not a closed peer", "[net][tcpclient]")
{
    // A zero-length payload is a reply, not the absence of one, and draining it must not read at
    // all: a read of zero is how EOF is spelled.
    auto socket = ScriptedSocket { {} };
    auto const got = syncRun(receiveExactly(&socket, 0));
    REQUIRE(got.has_value());
    CHECK(got->empty());
    CHECK(socket.remaining() == 0);
}

TEST_CASE("connectTcp reports why a dial failed, within its budget", "[net][tcpclient]")
{
    // RFC 5737 TEST-NET-1 is guaranteed not to be routable, so this exercises the failure path
    // without depending on what any host has listening. The code is not pinned -- an unroutable
    // address is refused by one network and black-holed by the next -- but it must FAIL, within the
    // budget rather than on the kernel's own retry schedule.
    constexpr auto Budget = 300ms;
    auto const started = std::chrono::steady_clock::now();
    auto const socket = syncRun(core::net::connectTcp("192.0.2.1", 9, Budget, 0ms));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(socket.has_value());
    CAPTURE(socket.error().toString());
    CHECK(elapsed < Budget * 20);
}

TEST_CASE("receiveExactly gives up on a peer that accepts and then goes silent", "[net][tcpclient]")
{
    // The property `ioTimeout` exists for, and why it is armed where the socket is minted: a dial
    // that succeeds says only that the peer accepted, and a peer that then never answers parks the
    // calling thread forever.
    auto loop = core::net::PlatformLoop {};
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());

    constexpr auto IoTimeout = 300ms;
    auto client = syncRun(core::net::connectTcp("127.0.0.1", (*listener)->boundPort(), 2s, IoTimeout));
    REQUIRE(client.has_value());
    // Accepted, and then held without a word: the connection is up, and silent.
    auto accepted = loop.blockOn(acceptOne(listener->get()));
    REQUIRE(accepted.has_value());

    auto const started = std::chrono::steady_clock::now();
    auto const got = syncRun(receiveExactly(client->get(), 16));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(got.has_value());
    CHECK(elapsed >= IoTimeout / 2); // it waited for the bound, rather than failing at once
    CHECK(elapsed < 15s);
}

TEST_CASE("A socket with a timeout armed still transfers normally", "[net][tcpclient]")
{
    // Guards the over-correction: a timeout applied as an immediate deadline would fail every
    // transfer, and the case above would still pass.
    auto loop = core::net::PlatformLoop {};
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());

    auto client = syncRun(core::net::connectTcp("127.0.0.1", (*listener)->boundPort(), 2s, 5s));
    REQUIRE(client.has_value());
    auto accepted = loop.blockOn(acceptOne(listener->get()));
    REQUIRE(accepted.has_value());

    auto const bytes = payload(4);
    CHECK(syncRun(sendAll(client->get(), bytes)));

    // The accepted end is a loop socket, so its read is awaited on the loop rather than under
    // `syncRun` -- the distinction `TcpClient.hpp` draws.
    auto const got = loop.blockOn(receiveExactly(accepted->get(), bytes.size()));
    REQUIRE(got.has_value());
    CHECK(*got == bytes);
}
