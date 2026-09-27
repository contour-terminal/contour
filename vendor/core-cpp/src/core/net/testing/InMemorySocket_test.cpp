// SPDX-License-Identifier: Apache-2.0
//
// The in-memory fake's own behaviour: what it does when bytes move, when an operation parks, and
// when a peer goes away while one is parked. What it answers in each CLOSED state against a real
// socket is `SocketClosedStates_test.cpp`; the parked cases are here because that table cannot
// express them -- nothing in it parks.
//
// Origin: fastcached `src/FastCache/Net/InMemoryTransport_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), plus the cases for what core-cpp's fake does that
// upstream's did not: a readability watch that parks, and a bounded write that waits rather than
// failing.
#include <core/async/DetachedTask.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/testing/InMemorySocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using core::async::DetachedTask;
using core::async::syncRun;
using core::async::Task;
using core::net::IoResult;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::testing::InMemoryListener;
using core::net::testing::InMemorySocketPair;

namespace
{

/// @return The byte count @p result carries, or nothing for an error -- `NetError` has no `==`, so
///         an `IoResult` cannot be compared whole.
[[nodiscard]] std::optional<std::size_t> countOf(IoResult const& result)
{
    return result.has_value() ? std::optional { *result } : std::nullopt;
}

[[nodiscard]] std::span<std::byte const> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span { text.data(), text.size() });
}

/// Reads until @p expected bytes have arrived or the stream ends.
Task<std::string> readAll(ISocket* socket, std::size_t expected)
{
    auto out = std::string {};
    while (out.size() < expected)
    {
        auto chunk = std::vector<std::byte>(expected - out.size());
        auto const result = co_await socket->read(chunk);
        REQUIRE(result.has_value());
        if (*result == 0)
            break;
        for (auto const byte: std::span { chunk }.first(*result))
            out.push_back(static_cast<char>(byte));
    }
    co_return out;
}

Task<IoResult> writeText(ISocket* socket, std::string text)
{
    co_return co_await socket->write(bytesOf(text));
}

Task<IoResult> readInto(ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->read(buffer);
}

Task<std::expected<void, core::net::NetError>> shutdown(ISocket* socket)
{
    co_return co_await socket->shutdownWrite();
}

Task<IoResult> waitReadable(ISocket* socket)
{
    co_return co_await socket->waitReadable();
}

Task<IoResult> writeThree(ISocket* socket, std::string a, std::string b, std::string c)
{
    auto const segments = std::array { bytesOf(a), bytesOf(b), bytesOf(c) };
    co_return co_await socket->writeVectored(segments);
}

/// What one parked `accept` answered, published from an EAGER coroutine: the point of these cases
/// is whether the awaiting coroutine is reached at all, and a value that never arrives cannot be
/// asserted on.
struct AcceptOutcome
{
    bool resolved { false };  ///< The await returned.
    bool hasValue { false };  ///< It carried a socket.
    bool cancelled { false }; ///< Its error was `Cancelled`.
};

/// `DetachedTask`, not `Task`, and that is the fixture: a lazy task does not start until awaited,
/// so the parked-accept path would never be taken.
DetachedTask observeAccept(InMemoryListener* listener, AcceptOutcome* out)
{
    auto accepted = co_await listener->accept();
    out->hasValue = accepted.has_value();
    if (!accepted.has_value())
        out->cancelled = accepted.error().code == NetErrorCode::Cancelled;
    out->resolved = true;
}

} // namespace

TEST_CASE("InMemorySocketPair shuttles bytes both directions", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();
    CHECK(countOf(syncRun(writeText(pair.client.get(), "hello"))) == 5u);
    CHECK(syncRun(readAll(pair.server.get(), 5)) == "hello");

    CHECK(countOf(syncRun(writeText(pair.server.get(), "world!"))) == 6u);
    CHECK(syncRun(readAll(pair.client.get(), 6)) == "world!");
}

TEST_CASE("writeVectored gathers segments into the same byte stream", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();
    // [header][value][trailer]: the peer sees exactly what one contiguous write would have sent.
    CHECK(countOf(syncRun(writeThree(pair.client.get(), "VALUE k 0 5\r\n", "hello", "\r\n"))) == 20u);
    CHECK(syncRun(readAll(pair.server.get(), 20)) == "VALUE k 0 5\r\nhello\r\n");
}

TEST_CASE("writeVectored skips empty segments and still reports the full count", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();
    CHECK(countOf(syncRun(writeThree(pair.client.get(), "head", "", "tail"))) == 8u);
    CHECK(syncRun(readAll(pair.server.get(), 8)) == "headtail");
}

TEST_CASE("InMemorySocketPair surfaces EOF when one side closes, and latches it", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();
    REQUIRE(syncRun(writeText(pair.client.get(), "ab")).has_value());
    pair.client->close();

    CHECK(syncRun(readAll(pair.server.get(), 2)) == "ab");
    CHECK_FALSE(pair.server->isClosed()); // drained, but no read has seen the end yet

    auto tail = std::array<std::byte, 1> {};
    CHECK(countOf(syncRun(readInto(pair.server.get(), tail))) == 0u);
    // The EOF latch `ISocket::isClosed` documents: a peer that has finished is no longer worth
    // holding, and the platform sockets say so here too.
    CHECK(pair.server->isClosed());
}

TEST_CASE("A read with nothing buffered parks and completes on the peer's write", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();

    auto chunk = std::array<std::byte, 5> {};
    auto reader = readInto(pair.server.get(), chunk);
    reader.handle().resume();
    REQUIRE_FALSE(reader.done()); // nothing buffered, so it must park rather than answer

    REQUIRE(syncRun(writeText(pair.client.get(), "hello")).has_value());

    // Completed INLINE by the peer's write, which is the fake's whole contract for a test.
    REQUIRE(reader.done());
    CHECK(countOf(reader.result()) == 5u);
    CHECK(std::string_view { reinterpret_cast<char const*>(chunk.data()), chunk.size() } == "hello");
}

TEST_CASE("A readability watch parks until the peer writes, and consumes nothing", "[net][inmemory]")
{
    // Upstream's fake answered `1` here at once, which a real socket does not: it parks. A watchdog
    // loop over the old answer spun, and a case could not reach a parked watch without a second
    // fake. The count says WHICH readable, so the watch after the write reports the bytes.
    auto pair = InMemorySocketPair::create();

    auto watcher = waitReadable(pair.server.get());
    watcher.handle().resume();
    REQUIRE_FALSE(watcher.done());

    REQUIRE(syncRun(writeText(pair.client.get(), "abc")).has_value());
    REQUIRE(watcher.done());
    CHECK(countOf(watcher.result()) == 3u);

    // Nothing was consumed: the read that follows gets every byte.
    CHECK(syncRun(readAll(pair.server.get(), 3)) == "abc");
}

TEST_CASE("A readability watch reports EOF as zero, and the peer's close wakes it", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();

    auto watcher = waitReadable(pair.server.get());
    watcher.handle().resume();
    REQUIRE_FALSE(watcher.done());

    pair.client->close();
    REQUIRE(watcher.done());
    CHECK(countOf(watcher.result()) == 0u);
    // A probe consumes nothing, including the end: the latch is the read's.
    CHECK_FALSE(pair.server->isClosed());
}

TEST_CASE("cancelRead retires a parked read with Cancelled and leaves the socket usable", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create();

    auto chunk = std::array<std::byte, 4> {};
    auto reader = readInto(pair.server.get(), chunk);
    reader.handle().resume();
    REQUIRE_FALSE(reader.done());

    pair.server->cancelRead();
    REQUIRE(reader.done());
    auto const cancelled = reader.result();
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == NetErrorCode::Cancelled);

    // Not a close: a later read works.
    REQUIRE(syncRun(writeText(pair.client.get(), "ok")).has_value());
    CHECK(syncRun(readAll(pair.server.get(), 2)) == "ok");
}

TEST_CASE("A parked read wakes to the reset, not to EOF, when its peer closes with bytes unread",
          "[net][inmemory][closed-states]")
{
    // A close with bytes still unread is a reset on the wire, and a peer parked in a read meets it
    // as an error. Delivering the FIN alone would wake the reader to EOF -- the ordinary goodbye --
    // where a real socket reports the reset.
    auto pair = InMemorySocketPair::create();
    REQUIRE(syncRun(writeText(pair.server.get(), "unread")).has_value());

    auto buffer = std::array<std::byte, 8> {};
    auto reader = readInto(pair.server.get(), buffer);
    reader.handle().resume();
    REQUIRE_FALSE(reader.done());

    pair.client->close();

    REQUIRE(reader.done());
    auto const got = reader.result();
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == NetErrorCode::ConnReset);

    // And the reset is what every later look at the socket reports.
    auto const readable = syncRun(waitReadable(pair.server.get()));
    REQUIRE_FALSE(readable.has_value());
    CHECK(readable.error().code == NetErrorCode::ConnReset);
}

TEST_CASE("A parked read wakes to EOF when its peer closes having read everything",
          "[net][inmemory][closed-states]")
{
    // The control for the case above: the same parked read and the same close, with nothing left
    // unread, is a FIN. Without it, "a close wakes the reader with an error" and "a reset wakes
    // the reader with an error" are one passing test.
    auto pair = InMemorySocketPair::create();

    auto buffer = std::array<std::byte, 8> {};
    auto reader = readInto(pair.server.get(), buffer);
    reader.handle().resume();
    REQUIRE_FALSE(reader.done());

    pair.client->close();

    REQUIRE(reader.done());
    CHECK(countOf(reader.result()) == 0u);
}

TEST_CASE("A bounded pair makes a write WAIT for the reader, not fail", "[net][inmemory]")
{
    // `ISocket::write` resolves only once every byte is gone. Upstream's fake answered a full pipe
    // with `WouldBlock` -- an answer no real socket gives a caller that awaits its write -- so a
    // flow control defect in the code under test could not be staged over it.
    auto pair = InMemorySocketPair::create(4);

    auto writer = writeText(pair.client.get(), "123456789");
    writer.handle().resume();
    REQUIRE_FALSE(writer.done()); // four fit; the rest waits

    // Each read makes room, and the writer is woken for it.
    CHECK(syncRun(readAll(pair.server.get(), 4)) == "1234");
    REQUIRE_FALSE(writer.done());
    CHECK(syncRun(readAll(pair.server.get(), 4)) == "5678");
    REQUIRE(writer.done());
    CHECK(countOf(writer.result()) == 9u);
    CHECK(syncRun(readAll(pair.server.get(), 1)) == "9");
}

TEST_CASE("A write parked on a full pipe fails with the reset when the peer closes over it",
          "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create(4);

    auto writer = writeText(pair.client.get(), "123456789");
    writer.handle().resume();
    REQUIRE_FALSE(writer.done());

    // The server closes with four of our bytes unread: a reset, which is what the parked write
    // meets, rather than succeeding into a pipe nobody will drain.
    pair.server->close();
    REQUIRE(writer.done());
    auto const written = writer.result();
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == NetErrorCode::ConnReset);
}

TEST_CASE("A half-close over a parked write fails that write's rest, as EPIPE does", "[net][inmemory]")
{
    // `ISocket::shutdownWrite`'s precondition is that no write is outstanding, and a plain socket
    // enforces nothing: the parked write's next attempt fails with EPIPE, so bytes the caller
    // believed queued never leave. The fake does the same, rather than quietly finishing the write
    // over a closed direction -- which would be the more permissive answer.
    auto pair = InMemorySocketPair::create(4);

    auto writer = writeText(pair.client.get(), "123456789");
    writer.handle().resume();
    REQUIRE_FALSE(writer.done());

    REQUIRE(syncRun(shutdown(pair.client.get())).has_value());
    REQUIRE_FALSE(writer.done()); // nothing retries it until the reader makes room

    CHECK(syncRun(readAll(pair.server.get(), 4)) == "1234");
    REQUIRE(writer.done());
    auto const written = writer.result();
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == NetErrorCode::SystemError);

    // And the reader sees the end of what did leave, not the rest of the write.
    auto tail = std::array<std::byte, 8> {};
    CHECK(countOf(syncRun(readInto(pair.server.get(), tail))) == 0u);
}

TEST_CASE("close retires a parked write with Cancelled", "[net][inmemory]")
{
    auto pair = InMemorySocketPair::create(2);

    auto writer = writeText(pair.client.get(), "12345");
    writer.handle().resume();
    REQUIRE_FALSE(writer.done());

    pair.client->close();
    REQUIRE(writer.done());
    auto const written = writer.result();
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == NetErrorCode::Cancelled);
}

TEST_CASE("InMemoryListener: close retrieves an eagerly parked accept", "[net][inmemory][accept]")
{
    auto listener = InMemoryListener {};

    auto observed = AcceptOutcome {};
    observeAccept(&listener, &observed);
    REQUIRE_FALSE(observed.resolved); // parked: nothing queued and not closed

    listener.close();

    CHECK(observed.resolved);
    CHECK_FALSE(observed.hasValue);
    CHECK(observed.cancelled);
}

TEST_CASE("InMemoryListener: a connection completes an eagerly parked accept", "[net][inmemory][accept]")
{
    // The second site that completes a parked accept. Asserted separately because `close` and
    // `connectClient` are two call sites, and a fix applied to one would leave the other reading
    // freed storage.
    auto listener = InMemoryListener {};

    auto observed = AcceptOutcome {};
    observeAccept(&listener, &observed);
    REQUIRE_FALSE(observed.resolved);

    auto client = listener.connectClient();
    REQUIRE(client != nullptr);

    CHECK(observed.resolved);
    CHECK(observed.hasValue);
}

TEST_CASE("InMemoryListener: a queued connection is accepted without parking", "[net][inmemory][accept]")
{
    // The control: both cases above assert a parked accept is REACHED, and an implementation that
    // resolved every accept synchronously would satisfy them while never parking at all.
    auto listener = InMemoryListener {};

    auto client = listener.connectClient();
    REQUIRE(client != nullptr);

    auto observed = AcceptOutcome {};
    observeAccept(&listener, &observed);

    CHECK(observed.resolved);
    CHECK(observed.hasValue);
}
