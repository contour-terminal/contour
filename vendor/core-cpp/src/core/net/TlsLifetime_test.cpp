// SPDX-License-Identifier: Apache-2.0
//
// The TLS socket's two gates -- one coroutine through the handshake, one through the outbound
// flush -- and what happens to the operations parked on them when something other than the gate's
// holder ends their wait: the socket being destroyed, another waiter destroying it, a stop token,
// and `cancelRead`.
//
// Each of these was a defect before Task B11's first fix round, and each is the ordinary shape of a
// consumer rather than a contrived one: a read pump and a `WriteQueue` drain both start on a new
// connection, so one of them drives the handshake and the other parks on the gate -- and then the
// connection is dropped, timed out, or cancelled before the peer answers.
#include <core/async/AsTask.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/WriteQueue.hpp>
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
#include <span>
#include <string>
#include <string_view>
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

std::span<std::byte const> bytesOf(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

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

/// Writes @p text and records how it ended.
DetachedTask observeWrite(ISocket* socket, std::string text, Outcome* out)
{
    try
    {
        auto const written = co_await socket->write(bytesOf(text));
        if (written)
            out->result = text;
        else
            out->result = std::unexpected(written.error());
    }
    catch (core::async::OperationCancelled const&)
    {
        out->threw = true;
    }
    out->settled = true;
}

/// Drives the handshake and records how it ended.
DetachedTask observeHandshake(ISocket* socket, Outcome* out)
{
    try
    {
        auto const done = co_await socket->handshakeIfNeeded();
        if (done)
            out->result = std::string {};
        else
            out->result = std::unexpected(done.error());
    }
    catch (core::async::OperationCancelled const&)
    {
        out->threw = true;
    }
    out->settled = true;
}

/// Reads once on a socket this flow OWNS, and drops it the moment the read answers -- what a
/// connection handler does with a connection whose handshake failed.
DetachedTask readThenDrop(std::shared_ptr<ISocket> owner, Outcome* out)
{
    try
    {
        auto buffer = std::array<std::byte, 64> {};
        auto const n = co_await owner->read(buffer);
        out->result = n ? std::expected<std::string, core::net::NetError> { std::string {} }
                        : std::unexpected(n.error());
    }
    catch (core::async::OperationCancelled const&)
    {
        out->threw = true;
    }
    owner.reset();
    out->settled = true;
}

/// Reads once into @p buffer and records that the flow is over however it ended, rethrowing its
/// cancellation so a `whenAny` race still sees a cancelled loser.
Task<core::net::IoResult> flaggedRead(ISocket* socket, std::span<std::byte> buffer, bool* settled)
{
    try
    {
        auto result = co_await socket->read(buffer);
        *settled = true;
        co_return result;
    }
    catch (core::async::OperationCancelled const&)
    {
        *settled = true;
        throw;
    }
}

Task<std::optional<core::net::IoResult>> readWithin(EventLoop* loop,
                                                    ISocket* socket,
                                                    std::span<std::byte> buffer,
                                                    bool* settled,
                                                    std::chrono::milliseconds deadline)
{
    co_return co_await core::net::withTimeout(loop, flaggedRead(socket, buffer, settled), deadline);
}

/// Runs the peer's handshake, then says @p text.
Task<bool> peerHandshakesAndSays(StrictTlsPeer* peer, ISocket* wire, std::string text)
{
    if (!co_await peer->handshake(wire))
        co_return false;
    co_return co_await peer->write(wire, std::move(text));
}

/// Closes the socket and destroys it in one turn, as `conn->close(); connections.erase(id);` does.
Task<void> closeAndDestroy(std::unique_ptr<ISocket>* socket)
{
    (*socket)->close();
    socket->reset();
    co_return;
}

} // namespace

TEST_CASE("A TLS read whose inner read settled with data is not resumed into a destroyed socket",
          "[net][tls][tlsgate][resume]")
{
    // The inner read settles WITH DATA in the drain that runs its readiness, and its waiter -- the
    // TLS socket's `feedIn` -- is queued behind whatever that drain already held (G2). Here that is
    // an owner closing and destroying the TLS socket. `feedIn` then resumed and wrote the
    // ciphertext into the freed session's BIO: a heap-use-after-free under AddressSanitizer,
    // garbage without. It must see the socket is gone and unwind.
    auto c = Conversation {};
    auto handshake = Outcome {};
    observeHandshake(c.tls.get(), &handshake);
    auto const peerDone = c.run(c.peer->handshake(c.wire.get()));
    REQUIRE(peerDone.has_value());
    REQUIRE(peerDone->has_value());
    REQUIRE(c.pumpUntil([&] { return handshake.settled; }));
    REQUIRE((handshake.result.has_value() && handshake.result->has_value()));

    auto read = Outcome {};
    observeRead(c.tls.get(), &read);
    c.settle();
    REQUIRE_FALSE(read.settled); // parked on the inner read

    // The peer's record goes out WITHOUT a loop turn: a small write to a writable socket completes
    // inline, so the TLS side has not run yet.
    auto say = c.peer->write(c.wire.get(), "late");
    say.handle().resume();
    REQUIRE(say.handle().done());

    // Turn until the inner read's readiness is queued for the next drain, and no further.
    auto turns = 0;
    while (c.loop.readyCount() == 0 && turns < 100)
    {
        std::ignore = c.loop.runOnce(std::chrono::milliseconds { 10 });
        ++turns;
    }
    REQUIRE(c.loop.readyCount() > 0);
    REQUIRE_FALSE(read.settled);

    // Queued BEHIND the readiness. Until 0.3.0 it ran after the inner read settled and queued
    // `feedIn`, and before `feedIn` ran, which is the destroyed-socket window this case was written
    // for: `feedIn` had to see the socket gone and unwind. A waiter a readiness callback completes
    // now resumes in the callback's position (async-and-net.md), so `feedIn` runs first, the read
    // answers the record, and the close that follows finds nothing parked. What still holds, and
    // what ASan watches, is that nothing is resumed into the destroyed socket.
    c.loop.spawn(closeAndDestroy(&c.tls));
    REQUIRE(c.pumpUntil([&] { return read.settled; }));
    CHECK(c.tls == nullptr);
    CHECK_FALSE(read.threw);
    REQUIRE(read.result.has_value());
    REQUIRE(read.result->has_value());
    CHECK(**read.result == "late");
}

TEST_CASE("Destroying a TLS socket mid-handshake resolves every operation parked on it",
          "[net][tls][tlsgate]")
{
    // A WriteQueue drain drives the handshake and parks on the inner read; a read pump parks on the
    // handshake gate behind it. The connection is then dropped before the peer says a word.
    auto c = Conversation {};
    auto queue = std::make_unique<core::net::WriteQueue>(c.loop, c.tls.get(), std::size_t { 1 } << 20U);
    REQUIRE(queue->enqueue("hello"));
    auto read = Outcome {};
    observeRead(c.tls.get(), &read);
    c.settle();
    REQUIRE(queue->draining());
    REQUIRE_FALSE(read.settled);

    c.tls.reset();

    REQUIRE(c.pumpUntil([&] { return read.settled && !queue->draining(); }));
    CHECK(read.threw); // a destroyed socket ABANDONS: the flow unwinds rather than reading a value
}

TEST_CASE("A flow released from the handshake gate may destroy the socket before the others run",
          "[net][tls][tlsgate]")
{
    // Three operations: a handshake driver, and a read and a write parked on its gate. The peer
    // answers with something that is not TLS, so the handshake fails and the gate releases both
    // waiters. The read's flow owns the socket and drops it on its answer -- which must not leave
    // the write to be resumed onto the destroyed one.
    auto c = Conversation {};
    auto owner = std::shared_ptr<ISocket> { std::move(c.tls) };
    auto* const socket = owner.get();

    auto driver = Outcome {};
    auto read = Outcome {};
    auto write = Outcome {};
    observeHandshake(socket, &driver);
    readThenDrop(std::move(owner), &read);
    observeWrite(socket, "unsent", &write);
    c.settle();
    REQUIRE_FALSE(driver.settled);
    REQUIRE_FALSE(read.settled);
    REQUIRE_FALSE(write.settled);

    auto const garbage = [](ISocket* wire) -> Task<void> {
        std::ignore = co_await wire->write(bytesOf("GET / HTTP/1.1\r\nHost: not-tls\r\n\r\n"));
    };
    REQUIRE(c.run(garbage(c.wire.get())));

    REQUIRE(c.pumpUntil([&] { return driver.settled && read.settled && write.settled; }));
    CHECK(driver.code() == NetErrorCode::SystemError);                    // the handshake failed, and said so
    CHECK_FALSE((write.result.has_value() && write.result->has_value())); // nothing was written
}

TEST_CASE("A read that loses a race while parked on the handshake leaves nothing behind",
          "[net][tls][tlsgate]")
{
    // The read waits on the gate while a write drives the handshake, and a deadline wins the race.
    // The lost read must end THEN -- not when the handshake completes, when it would read the
    // peer's first bytes into a buffer its caller has already given up on.
    auto c = Conversation {};
    auto write = Outcome {};
    observeWrite(c.tls.get(), "hello", &write);
    c.settle();
    REQUIRE_FALSE(write.settled);

    auto abandonedBuffer = std::array<std::byte, 64> {};
    auto lostSettled = false;
    auto const lost =
        c.loop.blockOn(readWithin(&c.loop, c.tls.get(), abandonedBuffer, &lostSettled, QuietPeriod));
    REQUIRE_FALSE(lost.has_value()); // the deadline won
    c.settle();
    CHECK(lostSettled); // and the read it cancelled is over, not parked

    // The peer now completes the handshake and speaks first. Its words belong to the next read.
    REQUIRE(c.run(peerHandshakesAndSays(c.peer.get(), c.wire.get(), "greeting"))
            == std::optional<bool> { true });
    auto const heard = c.run(c.peer->readAtLeast(c.wire.get(), 5));
    REQUIRE(heard.has_value());
    REQUIRE(heard->has_value());
    CHECK(**heard == "hello");

    auto next = Outcome {};
    observeRead(c.tls.get(), &next);
    REQUIRE(c.pumpUntil([&] { return next.settled; }));
    REQUIRE(next.result.has_value());
    REQUIRE(next.result->has_value());
    CHECK(**next.result == "greeting");
}

TEST_CASE("A reader's cancelRead does not reach into a write that drives the handshake",
          "[net][tls][tlsgate]")
{
    // The write drives the handshake, so the inner read parked under it is the WRITE's. A read
    // parked on the gate behind it is retired by `cancelRead`; the write is not touched.
    auto c = Conversation {};
    auto write = Outcome {};
    observeWrite(c.tls.get(), "hello", &write);
    auto read = Outcome {};
    observeRead(c.tls.get(), &read);
    c.settle();
    REQUIRE_FALSE(write.settled);
    REQUIRE_FALSE(read.settled);

    c.tls->cancelRead();
    REQUIRE(c.pumpUntil([&] { return read.settled; }));
    CHECK(read.code() == NetErrorCode::Cancelled);
    c.settle();
    CHECK_FALSE(write.settled); // still waiting for the peer, as it should be

    REQUIRE(c.run(peerHandshakesAndSays(c.peer.get(), c.wire.get(), "late")) == std::optional<bool> { true });
    auto const heard = c.run(c.peer->readAtLeast(c.wire.get(), 5));
    REQUIRE(heard.has_value());
    REQUIRE(heard->has_value());
    CHECK(**heard == "hello");
    REQUIRE(c.pumpUntil([&] { return write.settled; }));
    CHECK((write.result.has_value() && write.result->has_value()));
}
