// SPDX-License-Identifier: Apache-2.0
//
// `ISocket::waitReadable`'s COUNT semantics, over every backend this platform builds.
//
// The count was once documented as advisory, and it was: the reactor sockets answered `1` whatever
// their `recv(MSG_PEEK)` had just measured, and the completion-based one answered with the byte
// count of a zero-byte receive, which is `0` however much data is waiting. So the same call
// reported opposite numbers on Windows and Linux for the same event
// ([fastcached#677](https://github.com/LASTRADA-Software/fastcached/issues/677)). It now means:
//
//   - `0`  the peer has closed its write side; a `read` here returns EOF;
//   - `>0` bytes are pending; a `read` here returns some of them.
//
// Both the SYNCHRONOUS arm (the answer is already available, so nothing parks) and the PARKED arm
// (the answer arrives later, from the readiness callback) have to give the same answer, or a
// caller's behaviour depends on how busy its peer happened to be. They are different code, so each
// gets its own cases — and the parked ones assert that something actually parked, because a case
// that silently degenerates into the synchronous arm proves nothing about the path it names.
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::testing::BackendMatrix;

namespace
{

/// What one `waitReadable` answered, and what a read after it found.
struct Observation
{
    bool resolved = false;    ///< Whether the operation finished at all.
    bool hasValue = false;    ///< Whether it answered with a count rather than an error.
    std::size_t count = 0;    ///< The count it answered with.
    std::size_t readBack = 0; ///< How many bytes a following `read` got, where one was made.
    bool parkedFirst = false; ///< Whether the operation genuinely parked before it was answered.
};

/// What the peer does once the observer has parked.
enum class PeerAction : std::uint8_t
{
    Close,         ///< A full, graceful close: FIN, not RST.
    Write,         ///< One byte, which the observer must NOT consume.
    ShutdownWrite, ///< A half-close: "I have finished sending", not "I have gone".
};

/// Awaits `waitReadable` on @p sock and records what it answered.
/// @param sock The socket to watch.
/// @param out Where to record the observation.
/// @param readAfter Whether to follow the watch with a real `read`, which is what proves the
///        watch consumed nothing.
Task<void> observe(ISocket* sock, Observation* out, bool readAfter)
{
    auto const watched = co_await sock->waitReadable();
    out->resolved = true;
    out->hasValue = watched.has_value();
    if (watched.has_value())
        out->count = *watched;
    if (!readAfter || !watched.has_value())
        co_return;

    auto buffer = std::array<std::byte, 8> {};
    auto const got = co_await sock->read(buffer);
    if (got.has_value())
        out->readBack = *got;
}

/// Acts on @p peer once the observer has parked, and records that it really had.
///
/// **The park is ASSERTED, not assumed.** `whenAll` starts the observer first, so by the time this
/// runs the watch has either parked or answered synchronously — and if it answered synchronously
/// the case is testing the other arm entirely and would pass having parked nothing. The loop's own
/// count of readiness parks is what tells them apart.
/// @param loop The loop both sockets belong to.
/// @param peer The other end.
/// @param action What to do to it.
/// @param out Where to record that the observer had parked.
Task<void> actOnPeer(EventLoop* loop, ISocket* peer, PeerAction action, Observation* out)
{
    out->parkedFirst = loop->parkedWaiterCount() > 0;

    switch (action)
    {
        case PeerAction::Close: peer->close(); break;
        case PeerAction::ShutdownWrite: std::ignore = co_await peer->shutdownWrite(); break;
        case PeerAction::Write: {
            auto const payload = std::array<std::byte, 1> { std::byte { 0x7A } };
            std::ignore = co_await peer->write(std::span<std::byte const> { payload });
            break;
        }
    }
}

/// Runs the observer and the peer concurrently on one loop.
Task<void> watchWhilePeerActs(
    EventLoop* loop, ISocket* sock, ISocket* peer, PeerAction action, Observation* out, bool readAfter)
{
    co_await core::async::whenAll(observe(sock, out, readAfter), actOnPeer(loop, peer, action, out));
}

/// Writes @p text to @p sock, asserting it all went.
Task<void> writeText(ISocket* sock, std::string_view text, bool* ok)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(text.data()), text.size() };
    auto const written = co_await sock->write(bytes);
    *ok = written.has_value() && *written == text.size();
}

/// Reads whatever is there once, into @p out.
Task<void> readOnce(ISocket* sock, std::size_t* out, bool* hasValue)
{
    auto buffer = std::array<std::byte, 16> {};
    auto const got = co_await sock->read(buffer);
    *hasValue = got.has_value();
    if (got.has_value())
        *out = *got;
}

/// The half-close scenario: the peer says it has finished sending, and this end must still be able
/// to answer what it already owes.
Task<void> answerAHalfClosedPeer(EventLoop* loop,
                                 ISocket* server,
                                 ISocket* client,
                                 Observation* out,
                                 std::size_t* clientGot,
                                 bool* clientRead,
                                 bool* wrote)
{
    co_await watchWhilePeerActs(loop, server, client, PeerAction::ShutdownWrite, out, /*readAfter*/ false);
    // The wire's rule, executable: EOF means "this peer has finished sending", so the reply it is
    // already owed still arrives. A `shutdownWrite` that behaved as a close would lose it.
    co_await core::async::whenAll(writeText(server, "owed", wrote), readOnce(client, clientGot, clientRead));
}

} // namespace

TEST_CASE("waitReadable answers 0 for an EOF that is already pending", "[net][socket][waitreadable]")
{
    // The synchronous arm: the peer closed before anything asked, so the `MSG_PEEK` probe answers
    // without the operation ever parking.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());
            pair->second->close();

            auto observed = Observation {};
            loop.blockOn(observe(pair->first.get(), &observed, /*readAfter*/ false));

            REQUIRE(observed.resolved);
            REQUIRE(observed.hasValue); // EOF is not an error
            CHECK(observed.count == 0);
        }
    }
}

TEST_CASE("waitReadable answers >0 for pending data and consumes none of it", "[net][socket][waitreadable]")
{
    // The other half, and it is what stops the fix being "answer 0 always": a caller acting on `0`
    // must be able to trust that `>0` means there is something to read. The follow-up `read` is the
    // part that matters — a probe that CONSUMED the byte to learn about it would satisfy the count
    // and break every caller.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto wrote = false;
            loop.blockOn(writeText(pair->second.get(), "z", &wrote));
            REQUIRE(wrote);

            auto observed = Observation {};
            loop.blockOn(observe(pair->first.get(), &observed, /*readAfter*/ true));

            REQUIRE(observed.resolved);
            REQUIRE(observed.hasValue);
            CHECK(observed.count > 0);
            CHECK(observed.readBack == 1); // the byte was still there for the real read
        }
    }
}

TEST_CASE("A PARKED waitReadable answers 0 when the peer closes gracefully", "[net][socket][waitreadable]")
{
    // **The case the count exists for.** A peer that goes away the ordinary way sends FIN, which is
    // readability rather than an error. A caller told only "readable" cannot tell that from a
    // pipelined request, so it either abandons a live peer or waits forever on a dead one.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto observed = Observation {};
            loop.blockOn(watchWhilePeerActs(&loop,
                                            pair->first.get(),
                                            pair->second.get(),
                                            PeerAction::Close,
                                            &observed,
                                            /*readAfter*/ false));

            CHECK(observed.parkedFirst); // or this case is the synchronous arm wearing a disguise
            REQUIRE(observed.resolved);
            REQUIRE(observed.hasValue);
            CHECK(observed.count == 0);
        }
    }
}

TEST_CASE("A PARKED waitReadable answers >0 and still consumes nothing", "[net][socket][waitreadable]")
{
    // The parked arm of the previous pair: the answer arrives from the readiness callback rather
    // than from the probe in the verb, and it has to be the same answer — including the part where
    // the byte survives for the read that follows.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto observed = Observation {};
            loop.blockOn(watchWhilePeerActs(&loop,
                                            pair->first.get(),
                                            pair->second.get(),
                                            PeerAction::Write,
                                            &observed,
                                            /*readAfter*/ true));

            CHECK(observed.parkedFirst);
            REQUIRE(observed.resolved);
            REQUIRE(observed.hasValue);
            CHECK(observed.count > 0);
            CHECK(observed.readBack == 1);
        }
    }
}

TEST_CASE("A half-closed peer still receives what it is owed", "[net][socket][waitreadable][halfclose]")
{
    // `shutdownWrite` exists so a peer can say "I have finished sending" without saying "I have
    // gone", and the rule is that a server answers what it already owes such a peer
    // ([fastcached#671](https://github.com/LASTRADA-Software/fastcached/issues/671)). This is that
    // sentence as a test: the watch sees EOF — count 0 — and the reply still arrives.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto observed = Observation {};
            auto clientGot = std::size_t { 0 };
            auto clientRead = false;
            auto wrote = false;
            loop.blockOn(answerAHalfClosedPeer(
                &loop, pair->first.get(), pair->second.get(), &observed, &clientGot, &clientRead, &wrote));

            CHECK(observed.parkedFirst);
            REQUIRE(observed.resolved);
            REQUIRE(observed.hasValue);
            CHECK(observed.count == 0); // the peer has finished SENDING
            CHECK(wrote);               // and this end can still write
            REQUIRE(clientRead);
            CHECK(clientGot == 4); // and the half-closed peer receives it
        }
    }
}
