// SPDX-License-Identifier: Apache-2.0
//
// The record pump, held against a peer that is not the code under test.
//
// Each case puts one `TlsSocket` on one end of a socket pair and a `StrictTlsPeer` -- OpenSSL driven
// by hand -- on the other. A TlsSocket checked only against another TlsSocket agrees with itself:
// a defect both ends share, in writing the alert or in reading it, would pass. The strict peer is
// OpenSSL alone, reading the way OpenSSL 3 does from a socket, and it is what tells them apart.
//
// Ported from fastcached's `Net/TlsSocket_test.cpp` (0708dd54): the waitReadable cases (#712) and
// the handshake over non-TLS input. Upstream's peer pumps two in-memory pipes by hand; core-cpp has
// no such pipe, so this one pumps the far end of `makeSocketPair` through the loop, which is also
// what makes every wait here bounded by the loop rather than by a step count.
#include <core/async/AsTask.hpp>
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

#include <algorithm>
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
using core::net::testing::StreamEnd;
using core::net::testing::StrictTlsPeer;
using namespace std::chrono_literals;

namespace
{

/// Every exchange here is bounded: a regression in a record pump does not fail, it HANGS -- a peer
/// waiting for an alert nobody sent -- and this turns that into a named failure.
constexpr auto ExchangeBound = std::chrono::milliseconds { 10'000 };

/// Long enough that a loaded runner has delivered what was sent, short enough to be cheap: what
/// the cases wait THIS long for is something that must NOT happen.
constexpr auto QuietPeriod = 30ms;

std::span<std::byte const> bytesOf(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// Writes @p text through @p socket.
/// @return Whether every byte was written.
Task<bool> send(ISocket* socket, std::string_view text)
{
    auto const written = co_await socket->write(bytesOf(text));
    co_return written.has_value() && *written == text.size();
}

/// Reads once from @p socket.
/// @return The bytes as text, or the error.
Task<std::expected<std::string, core::net::NetError>> receive(ISocket* socket)
{
    auto buffer = std::array<std::byte, 256> {};
    auto const n = co_await socket->read(buffer);
    if (!n)
        co_return std::unexpected(n.error());
    co_return std::string { reinterpret_cast<char const*>(buffer.data()), *n };
}

/// A `TlsSocket` over one end of a socket pair, the strict peer over the other, and the loop.
struct Conversation
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    EventLoop loop { *backend };
    std::unique_ptr<ISocket> wire; ///< The strict peer's plaintext end.
    std::unique_ptr<ISocket> tls;  ///< The code under test.
    std::unique_ptr<StrictTlsPeer> peer;

    /// Runs @p task on the loop, bounded.
    /// @return Its value, or nullopt if the bound elapsed first.
    template <typename T>
    std::optional<T> run(Task<T> task)
    {
        return loop.blockOn(core::net::withTimeout(&loop, std::move(task), ExchangeBound));
    }

    /// Runs @p task on the loop, bounded.
    /// @return Whether it finished before the bound.
    bool run(Task<void> task)
    {
        return loop.blockOn(core::net::withTimeout(&loop, std::move(task), ExchangeBound));
    }

    /// Pumps the loop until @p ready holds, bounded.
    template <typename Predicate>
    bool pumpUntil(Predicate ready)
    {
        return loop.blockOn(core::net::testing::waitUntil(&loop, std::move(ready), 5000));
    }
};

/// The TLS side is a SERVER, and the strict peer a client.
/// @return The conversation, TCP-connected and not yet handshaken.
std::unique_ptr<Conversation> serverConversation()
{
    auto conversation = std::make_unique<Conversation>();
    auto pair = core::net::testing::makeSocketPair(conversation->loop);
    REQUIRE(pair.has_value());
    auto context = core::net::makeSelfSignedServerContext();
    REQUIRE(context.has_value());
    conversation->tls = (*context)->wrap(std::move(pair->first), conversation->loop);
    REQUIRE(conversation->tls != nullptr);
    conversation->wire = std::move(pair->second);
    auto peer = StrictTlsPeer::client();
    REQUIRE(peer.has_value());
    conversation->peer = std::move(*peer);
    return conversation;
}

/// Handshakes through application data rather than `handshakeIfNeeded`, so the cases that are not
/// ABOUT the handshake do not depend on the verb that is.
Task<bool> handshakeByPing(Conversation* c)
{
    auto const received = std::make_shared<std::string>();
    auto const serverSide = [](ISocket* tls, std::shared_ptr<std::string> out) -> Task<void> {
        if (auto got = co_await receive(tls); got)
            *out = std::move(*got);
    };
    auto const peerSide = [](StrictTlsPeer* peer, ISocket* wire) -> Task<void> {
        if (co_await peer->handshake(wire))
            std::ignore = co_await peer->write(wire, "ping");
    };
    co_await core::net::testing::allOf(serverSide(c->tls.get(), received),
                                       peerSide(c->peer.get(), c->wire.get()));
    co_return *received == "ping";
}

/// What one `waitReadable` answered, and whether it has answered YET.
///
/// `resolved` is what separates "parked until the peer acted" from "answered on the spot"; a case
/// that does not look at it passes under a socket that never parks.
struct ReadableOutcome
{
    bool resolved = false;
    std::optional<core::net::IoResult> result;
};

/// Detached, so it catches its own cancellation: one escaping a `DetachedTask` terminates the
/// binary, which would turn a case that fails its assertions into one that takes every later case
/// down with it.
DetachedTask observeReadable(ISocket* socket, ReadableOutcome* out)
{
    try
    {
        out->result = co_await socket->waitReadable();
    }
    catch (core::async::OperationCancelled const&)
    {
        out->result = std::unexpected(core::net::makeNetError(core::net::NetErrorCode::Cancelled));
    }
    out->resolved = true;
}

} // namespace

TEST_CASE("shutdownWrite sends close_notify before the transport's FIN", "[net][tls][tlssocket]")
{
    // What a strict peer would catch: a half-close that forwards straight to the inner socket
    // delivers a FIN with no close_notify, which OpenSSL 3 reading from a socket reports as a
    // truncation, not an end. And a half-close is a statement about OUTPUT: this side must still
    // read the peer's answer afterwards, and must not be able to write.
    auto c = serverConversation();

    // No REQUIRE inside a flow: a throw there unwinds one side of the exchange and leaves the other
    // waiting, which turns a red into a hang. Each side records what it saw and stops.
    auto const serverSide =
        [](ISocket* tls, std::string* reply, bool* halfClosed, bool* writeRefused) -> Task<void> {
        if (!co_await send(tls, "last words"))
            co_return;
        *halfClosed = (co_await tls->shutdownWrite()).has_value();
        *writeRefused = !(co_await send(tls, "one more"));
        if (auto got = co_await receive(tls); got)
            *reply = std::move(*got);
    };
    auto const peerSide =
        [](StrictTlsPeer* peer, ISocket* wire, std::string* heard, StreamEnd* end) -> Task<void> {
        if (!co_await peer->handshake(wire))
            co_return;
        *end = co_await peer->readToEnd(wire, heard);
        if (*end == StreamEnd::CloseNotify)
            std::ignore = co_await peer->write(wire, "answer");
    };

    auto reply = std::string {};
    auto halfClosed = false;
    auto writeRefused = false;
    auto heard = std::string {};
    auto end = StreamEnd::Failed;
    auto const finished =
        c->run(core::net::testing::allOf(serverSide(c->tls.get(), &reply, &halfClosed, &writeRefused),
                                         peerSide(c->peer.get(), c->wire.get(), &heard, &end)));

    // Checked before `finished`, because a half-close with no alert does not only fail this: the
    // strict peer's session is dead once it has read a truncation, so it cannot answer, and the
    // exchange then runs into its bound. The reason is what these say; the bound only says when.
    CHECK(heard == "last words");
    CHECK(end == StreamEnd::CloseNotify);
    REQUIRE(finished); // otherwise the peer waited for an end that never came
    CHECK(halfClosed);
    CHECK(writeRefused);
    CHECK(reply == "answer");
}

TEST_CASE("handshakeIfNeeded completes the handshake with nothing else driving it", "[net][tls][tlssocket]")
{
    auto c = serverConversation();

    auto const serverSide = [](ISocket* tls, bool* ok) -> Task<void> {
        *ok = (co_await tls->handshakeIfNeeded()).has_value();
    };
    auto const peerSide = [](StrictTlsPeer* peer, ISocket* wire, bool* ok) -> Task<void> {
        *ok = (co_await peer->handshake(wire)).has_value();
    };
    auto serverOk = false;
    auto peerOk = false;
    auto const finished = c->run(core::net::testing::allOf(serverSide(c->tls.get(), &serverOk),
                                                           peerSide(c->peer.get(), c->wire.get(), &peerOk)));

    REQUIRE(finished); // otherwise the peer waited for a server flight never sent
    CHECK(serverOk);
    CHECK(peerOk);
    CHECK(c->peer->handshakeFinished());
}

TEST_CASE("handshakeIfNeeded over non-TLS input fails instead of hanging", "[net][tls][tlssocket]")
{
    auto c = serverConversation();

    auto const garbage = [](ISocket* wire) -> Task<void> {
        std::ignore = co_await send(wire, "GET / HTTP/1.1\r\nHost: not-tls\r\n\r\n");
    };
    auto const handshaken = [](ISocket* tls, std::optional<bool>* ok) -> Task<void> {
        *ok = (co_await tls->handshakeIfNeeded()).has_value();
    };
    auto ok = std::optional<bool> {};
    auto const finished =
        c->run(core::net::testing::allOf(garbage(c->wire.get()), handshaken(c->tls.get(), &ok)));

    REQUIRE(finished);
    REQUIRE(ok.has_value());
    CHECK_FALSE(*ok);
}

TEST_CASE("waitReadable parks until the peer acts, and tells data from close_notify", "[net][tls][tlssocket]")
{
    auto c = serverConversation();
    REQUIRE(c->run(handshakeByPing(c.get())) == std::optional<bool> { true });

    SECTION("data pending is reported, and none of it is consumed")
    {
        auto outcome = ReadableOutcome {};
        observeReadable(c->tls.get(), &outcome);
        c->run(core::net::testing::sleepFor(&c->loop, QuietPeriod));
        CHECK_FALSE(outcome.resolved); // nothing was sent: this must be a wait, not an answer

        REQUIRE(c->run(c->peer->write(c->wire.get(), "payload")) == std::optional<bool> { true });
        REQUIRE(c->pumpUntil([&] { return outcome.resolved; }));
        REQUIRE(outcome.result.has_value());
        REQUIRE(outcome.result->has_value());
        CHECK(**outcome.result > 0);

        auto const read = c->run(receive(c->tls.get()));
        REQUIRE(read.has_value());
        REQUIRE(read->has_value());
        CHECK(**read == "payload");
    }

    SECTION("a parked watch resolves EOF when the peer closes cleanly")
    {
        auto outcome = ReadableOutcome {};
        observeReadable(c->tls.get(), &outcome);
        c->run(core::net::testing::sleepFor(&c->loop, QuietPeriod));
        CHECK_FALSE(outcome.resolved);

        REQUIRE(c->run(c->peer->sendCloseNotify(c->wire.get())) == std::optional<bool> { true });
        REQUIRE(c->pumpUntil([&] { return outcome.resolved; }));
        REQUIRE(outcome.result.has_value());
        REQUIRE(outcome.result->has_value());
        CHECK(**outcome.result == 0);
    }

    SECTION("close_notify followed by FIN reads as EOF, not as data")
    {
        // The raw socket sees BYTES here -- the alert record -- so a transport-level peek answers
        // "data pending" for a peer that has finished sending (fastcached#712).
        REQUIRE(c->run(c->peer->sendCloseNotify(c->wire.get())) == std::optional<bool> { true });
        auto const halfClosed = c->run(core::async::asTask(c->wire->shutdownWrite()));
        REQUIRE(halfClosed.has_value());
        REQUIRE(halfClosed->has_value());

        auto outcome = ReadableOutcome {};
        observeReadable(c->tls.get(), &outcome);
        REQUIRE(c->pumpUntil([&] { return outcome.resolved; }));
        REQUIRE(outcome.result.has_value());
        REQUIRE(outcome.result->has_value());
        CHECK(**outcome.result == 0);
    }

    SECTION("a truncated stream is a reset, not an end")
    {
        // Upstream answered 0 here -- "the peer has finished sending" -- which is the claim a
        // truncation attack needs a reader to make. A FIN with no close_notify before it is not
        // an orderly end, so the watch reports the failure the next read would.
        auto const halfClosed = c->run(core::async::asTask(c->wire->shutdownWrite()));
        REQUIRE(halfClosed.has_value());
        REQUIRE(halfClosed->has_value());

        auto outcome = ReadableOutcome {};
        observeReadable(c->tls.get(), &outcome);
        REQUIRE(c->pumpUntil([&] { return outcome.resolved; }));
        REQUIRE(outcome.result.has_value());
        REQUIRE_FALSE(outcome.result->has_value());
        CHECK(outcome.result->error().code == core::net::NetErrorCode::ConnReset);
    }

    SECTION("a watch parked at close() is retrieved, not leaked")
    {
        auto outcome = ReadableOutcome {};
        observeReadable(c->tls.get(), &outcome);
        c->run(core::net::testing::sleepFor(&c->loop, QuietPeriod));
        REQUIRE_FALSE(outcome.resolved);

        c->tls->close();
        REQUIRE(c->pumpUntil([&] { return outcome.resolved; }));
        REQUIRE(outcome.result.has_value());
        CHECK_FALSE(outcome.result->has_value());
    }
}

TEST_CASE("A transport EOF before close_notify reads as a reset, and close_notify as the end",
          "[net][tls][tlssocket]")
{
    // A read's `0` means the peer has finished sending. Answering it for a transport that ended
    // with no close_notify lets a truncated stream pass as a complete one -- a response cut short
    // by an attacker, or by a crash, reads exactly like the whole of it. OpenSSL 3 reading from a
    // socket refuses it (SSL_R_UNEXPECTED_EOF_WHILE_READING), and so does this layer.
    auto c = serverConversation();
    REQUIRE(c->run(handshakeByPing(c.get())) == std::optional<bool> { true });

    SECTION("the peer's transport ends mid-stream with no alert")
    {
        REQUIRE(c->run(c->peer->write(c->wire.get(), "partial")) == std::optional<bool> { true });
        auto const halfClosed = c->run(core::async::asTask(c->wire->shutdownWrite()));
        REQUIRE(halfClosed.has_value());
        REQUIRE(halfClosed->has_value());

        auto const first = c->run(receive(c->tls.get()));
        REQUIRE(first.has_value());
        REQUIRE(first->has_value());
        CHECK(**first == "partial"); // what arrived intact is still delivered

        auto const second = c->run(receive(c->tls.get()));
        REQUIRE(second.has_value());
        REQUIRE_FALSE(second->has_value());
        CHECK(second->error().code == core::net::NetErrorCode::ConnReset);
        CHECK(c->tls->isClosed()); // the peer is gone either way
    }

    SECTION("the peer sends close_notify: a clean end of stream")
    {
        REQUIRE(c->run(c->peer->write(c->wire.get(), "whole")) == std::optional<bool> { true });
        REQUIRE(c->run(c->peer->sendCloseNotify(c->wire.get())) == std::optional<bool> { true });
        auto const halfClosed = c->run(core::async::asTask(c->wire->shutdownWrite()));
        REQUIRE(halfClosed.has_value());
        REQUIRE(halfClosed->has_value());

        auto const first = c->run(receive(c->tls.get()));
        REQUIRE(first.has_value());
        REQUIRE(first->has_value());
        CHECK(**first == "whole");

        auto const second = c->run(receive(c->tls.get()));
        REQUIRE(second.has_value());
        REQUIRE(second->has_value());
        CHECK((*second)->empty()); // `0`: ISocket's end of stream
    }
}

TEST_CASE("A stale OpenSSL error on the loop's thread does not fail a healthy read", "[net][tls][tlssocket]")
{
    // SSL_get_error reads the THREAD's error queue, and every connection on a loop shares that
    // thread. An entry left there by another connection turns this one's routine WANT_READ into
    // SSL_ERROR_SSL, and a healthy connection fails for a neighbour's error.
    auto c = serverConversation();
    REQUIRE(c->run(handshakeByPing(c.get())) == std::optional<bool> { true });

    auto const readAfterStaleError =
        [](ISocket* tls) -> Task<std::expected<std::string, core::net::NetError>> {
        core::net::testing::leaveStaleOpensslError();
        co_return co_await receive(tls); // parks on WANT_READ: nothing has been sent yet
    };
    auto const answerLater = [](EventLoop* loop, StrictTlsPeer* peer, ISocket* wire) -> Task<void> {
        co_await core::net::testing::sleepFor(loop, QuietPeriod);
        std::ignore = co_await peer->write(wire, "healthy");
    };
    auto read = std::optional<std::expected<std::string, core::net::NetError>> {};
    auto const collect =
        [](Task<std::expected<std::string, core::net::NetError>> task,
           std::optional<std::expected<std::string, core::net::NetError>>* out) -> Task<void> {
        *out = co_await std::move(task);
    };
    auto const finished =
        c->run(core::net::testing::allOf(collect(readAfterStaleError(c->tls.get()), &read),
                                         answerLater(&c->loop, c->peer.get(), c->wire.get())));

    REQUIRE(finished);
    REQUIRE(read.has_value());
    REQUIRE(read->has_value());
    CHECK(**read == "healthy");
}

namespace
{

/// An inner transport whose writes wait for the test to open a gate, counting how many it holds.
///
/// The TLS layer's inner socket has ONE write operation. A reader and a writer each reaching it
/// through the outbound flush would put a second write into that slot, which a real socket answers
/// by dropping the parked one (Debug: `contract::claimWriteSlot`) -- and whichever way it goes, two
/// flushes interleave ciphertext. Counting at the inner socket sees that on every build.
class GatedWrites final: public ISocket
{
  public:
    GatedWrites(EventLoop& loop, std::unique_ptr<ISocket> inner) noexcept:
        _loop(loop), _inner(std::move(inner))
    {
    }

    void open() noexcept { _open = true; }
    void shut() noexcept { _open = false; }
    [[nodiscard]] int inFlight() const noexcept { return _inFlight; }
    [[nodiscard]] int maxInFlight() const noexcept { return _maxInFlight; }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return _inner->read(buffer);
    }
    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return core::net::IoAwaitable { gatedWrite(buffer) };
    }
    void cancelRead() noexcept override { _inner->cancelRead(); }
    void close() noexcept override { _inner->close(); }
    [[nodiscard]] bool isClosed() const noexcept override { return _inner->isClosed(); }

  private:
    Task<core::net::IoResult> gatedWrite(std::span<std::byte const> buffer)
    {
        ++_inFlight;
        _maxInFlight = std::max(_maxInFlight, _inFlight);
        co_await core::net::pollUntil(&_loop, [this] { return _open || _inner->isClosed(); });
        auto result = co_await _inner->write(buffer);
        --_inFlight;
        co_return result;
    }

    EventLoop& _loop;
    std::unique_ptr<ISocket> _inner;
    bool _open = true;
    int _inFlight = 0;
    int _maxInFlight = 0;
};

/// Detached; catches its own cancellation, as @c observeReadable does.
DetachedTask writeInBackground(ISocket* socket, std::string const* payload, std::optional<bool>* ok)
{
    try
    {
        *ok = co_await send(socket, *payload);
    }
    catch (core::async::OperationCancelled const&)
    {
        *ok = false;
    }
}

/// Detached; catches its own cancellation, as @c observeReadable does.
DetachedTask readInBackground(ISocket* socket, std::optional<std::string>* out)
{
    try
    {
        auto got = co_await receive(socket);
        *out = got ? std::move(*got) : std::string { "<error>" };
    }
    catch (core::async::OperationCancelled const&)
    {
        *out = std::string { "<cancelled>" };
    }
}

} // namespace

TEST_CASE("A read beside a parked write never puts a second write into the inner socket",
          "[net][tls][tlssocket]")
{
    // A 20000-byte write is two records, more than one flush chunk: the first chunk parks at the
    // inner socket and the rest stays queued in OpenSSL's outgoing BIO. A read issued then finds
    // bytes pending when it flushes on its way to WANT_READ -- and must leave them to the flush
    // already in progress instead of starting a second one.
    auto c = std::make_unique<Conversation>();
    auto pair = core::net::testing::makeSocketPair(c->loop);
    REQUIRE(pair.has_value());
    auto material = core::net::generateSelfSignedCertificate();
    REQUIRE(material.has_value());
    auto clientContext = core::net::makeTlsClientContext();
    REQUIRE(clientContext.has_value());
    auto gatedOwner = std::make_unique<GatedWrites>(c->loop, std::move(pair->first));
    auto* const gated = gatedOwner.get();
    c->tls = (*clientContext)->wrap(std::move(gatedOwner), c->loop);
    c->wire = std::move(pair->second);
    auto peer = StrictTlsPeer::server(*material);
    REQUIRE(peer.has_value());
    c->peer = std::move(*peer);

    // Handshake, through application data.
    auto const clientHello = [](ISocket* tls) -> Task<void> {
        std::ignore = co_await send(tls, "hello");
    };
    auto const peerHello = [](StrictTlsPeer* p, ISocket* wire, std::string* heard) -> Task<void> {
        if (!co_await p->handshake(wire))
            co_return;
        if (auto got = co_await p->readAtLeast(wire, 5); got)
            *heard = std::move(*got);
    };
    auto heard = std::string {};
    REQUIRE(c->run(core::net::testing::allOf(clientHello(c->tls.get()),
                                             peerHello(c->peer.get(), c->wire.get(), &heard))));
    REQUIRE(heard == "hello");

    gated->shut();
    auto const payload = std::string(20'000, 'x');
    auto written = std::optional<bool> {};
    auto read = std::optional<std::string> {};
    writeInBackground(c->tls.get(), &payload, &written);
    REQUIRE(c->pumpUntil([&] { return gated->inFlight() > 0; }));
    readInBackground(c->tls.get(), &read);
    c->run(core::net::testing::sleepFor(&c->loop, QuietPeriod));

    CHECK(gated->maxInFlight() == 1);

    // Let everything go, and check the peer received the payload whole and in order: two flushes
    // interleaving ciphertext would fail its record layer here.
    gated->open();
    auto const received = c->run(c->peer->readAtLeast(c->wire.get(), payload.size()));
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    CHECK(**received == payload);
    REQUIRE(c->pumpUntil([&] { return written.has_value(); }));
    CHECK(*written);

    REQUIRE(c->run(c->peer->write(c->wire.get(), "pong")) == std::optional<bool> { true });
    REQUIRE(c->pumpUntil([&] { return read.has_value(); }));
    CHECK(*read == "pong");
}
