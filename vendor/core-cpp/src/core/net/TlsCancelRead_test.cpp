// SPDX-License-Identifier: Apache-2.0
//
// `cancelRead` during a TLS handshake that a READ drives: the one TlsLifetime_test case that needs
// the inner socket's own `cancelRead`. It ran on POSIX only while `WindowsSocket` inherited the
// no-op; every transport now declares it -- `tests/cmake/check-cancel-read-declared.cmake` holds
// that -- so it runs wherever TLS does, over whatever socket the default backend hands out. The
// helpers are TlsLifetime_test.cpp's, trimmed to what this case uses.
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/StrictTlsPeer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using core::async::DetachedTask;
using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::testing::StrictTlsPeer;
using namespace std::chrono_literals;

namespace
{

/// Every exchange is bounded: a lifetime defect here does not fail, it hangs or it corrupts.
constexpr auto ExchangeBound = std::chrono::milliseconds { 10'000 };

/// How long to let parked operations settle into their parks. What the cases wait this long for is
/// something that must NOT happen in the meantime.
constexpr auto QuietPeriod = 30ms;

/// A TLS server over one end of a socket pair, a strict client over the other, and the loop. The
/// client stays silent until a case makes it speak, so the server's handshake parks.
struct Conversation
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    EventLoop loop { *backend };
    std::unique_ptr<ISocket> wire;
    std::unique_ptr<ISocket> tls;
    std::unique_ptr<StrictTlsPeer> peer;

    Conversation()
    {
        auto pair = core::net::testing::makeSocketPair(loop);
        REQUIRE(pair.has_value());
        auto context = core::net::makeSelfSignedServerContext();
        REQUIRE(context.has_value());
        tls = (*context)->wrap(std::move(pair->first), loop);
        REQUIRE(tls != nullptr);
        wire = std::move(pair->second);
        auto client = StrictTlsPeer::client();
        REQUIRE(client.has_value());
        peer = std::move(*client);
    }

    template <typename T>
    std::optional<T> run(Task<T> task)
    {
        return loop.blockOn(core::net::withTimeout(&loop, std::move(task), ExchangeBound));
    }

    bool run(Task<void> task)
    {
        return loop.blockOn(core::net::withTimeout(&loop, std::move(task), ExchangeBound));
    }

    template <typename Predicate>
    bool pumpUntil(Predicate ready)
    {
        return loop.blockOn(core::net::testing::waitUntil(&loop, std::move(ready), 5000));
    }

    void settle() { run(core::net::testing::sleepFor(&loop, QuietPeriod)); }
};

/// How one operation ended, and whether it has ended yet.
struct Outcome
{
    bool settled = false; ///< The flow is over, one way or another.
    bool threw = false;   ///< It unwound through `OperationCancelled`.
    std::optional<std::expected<std::string, core::net::NetError>> result; ///< What it answered.

    /// @return The error code it answered, or `Ok` where it answered a value or threw.
    [[nodiscard]] NetErrorCode code() const noexcept
    {
        return result && !result->has_value() ? result->error().code : NetErrorCode::Ok;
    }
};

/// Reads once and records how it ended. Detached, so it catches its own cancellation: one escaping
/// a `DetachedTask` terminates the binary.
DetachedTask observeRead(ISocket* socket, Outcome* out)
{
    try
    {
        auto buffer = std::array<std::byte, 256> {};
        auto const n = co_await socket->read(buffer);
        if (n)
            out->result = std::string { reinterpret_cast<char const*>(buffer.data()), *n };
        else
            out->result = std::unexpected(n.error());
    }
    catch (core::async::OperationCancelled const&)
    {
        out->threw = true;
    }
    out->settled = true;
}

/// Runs the peer's handshake, then says @p text.
Task<bool> peerHandshakesAndSays(StrictTlsPeer* peer, ISocket* wire, std::string text)
{
    if (!co_await peer->handshake(wire))
        co_return false;
    co_return co_await peer->write(wire, std::move(text));
}

} // namespace

TEST_CASE("cancelRead during a handshake the read drives leaves the socket usable", "[net][tls][tlsgate]")
{
    // `cancelRead` is not a close: the socket stays open and a later read works. A cancelled inner
    // read consumed no ciphertext, so the handshake can simply be driven again.
    auto c = Conversation {};
    auto read = Outcome {};
    observeRead(c.tls.get(), &read);
    c.settle();
    REQUIRE_FALSE(read.settled);

    c.tls->cancelRead();
    REQUIRE(c.pumpUntil([&] { return read.settled; }));
    CHECK(read.code() == NetErrorCode::Cancelled);

    auto next = Outcome {};
    observeRead(c.tls.get(), &next);
    REQUIRE(c.run(peerHandshakesAndSays(c.peer.get(), c.wire.get(), "after the cancel"))
            == std::optional<bool> { true });
    REQUIRE(c.pumpUntil([&] { return next.settled; }));
    REQUIRE(next.result.has_value());
    REQUIRE(next.result->has_value());
    CHECK(**next.result == "after the cancel");
}
