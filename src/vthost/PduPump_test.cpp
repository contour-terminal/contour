// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vthost/PduPump.h>

using coro::Task;
using vthost::pumpPdus;
using vthost::PumpResult;
using vthost::PumpStop;
namespace proto = vthost::proto;

namespace
{

/// Feeds @p bytes to the pump's peer, then closes so the pump sees the stream end.
///
/// Takes a pointer, like the coroutines below: a reference parameter would outlive nothing in
/// particular across the first suspension (clang-tidy's avoid-reference-coroutine-parameters), and
/// the caller owns the buffer for the whole run anyway.
Task<void> feedThenClose(net::ISocket* peer, std::vector<std::byte> const* bytes)
{
    if (!bytes->empty())
        std::ignore = co_await peer->write(std::span<std::byte const> { bytes->data(), bytes->size() });
    peer->close();
}

/// Runs the pump and stores its outcome. A free function taking pointers, not a capturing
/// lambda: a lambda coroutine's closure dies at the end of the full-expression that created it,
/// while the coroutine frame lives on (clang-tidy's avoid-capturing-lambda-coroutines).
Task<void> runPump(net::ISocket* socket,
                   std::function<bool(proto::DecodedFrame const&)> const* handler,
                   PumpResult* out)
{
    *out = co_await pumpPdus(socket, *handler);
}

Task<void> pumpAndFeed(net::testing::SocketPair* pair,
                       std::vector<std::byte> const* bytes,
                       std::function<bool(proto::DecodedFrame const&)> const* handler,
                       PumpResult* out)
{
    co_await coro::whenAll(runPump(pair->first.get(), handler, out),
                           feedThenClose(pair->second.get(), bytes));
}

/// Runs the pump against a peer that writes @p bytes and hangs up.
/// @param bytes What the peer sends.
/// @param handler Consumes each decoded frame; defaults to accepting everything.
/// @return Why the pump stopped.
PumpResult pumpOver(
    std::vector<std::byte> const& bytes,
    std::function<bool(proto::DecodedFrame const&)> const& handler = [](auto const&) { return true; })
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto outcome = PumpResult {};
    loop.blockOn(pumpAndFeed(&*pair, &bytes, &handler, &outcome));
    return outcome;
}

/// @return One valid encoded frame carrying @p pdu.
std::vector<std::byte> encoded(proto::DecodedPdu const& pdu, std::uint64_t serial = 1)
{
    auto writer = proto::Writer {};
    proto::encodePdu(writer, serial, pdu);
    return { writer.view().begin(), writer.view().end() };
}

} // namespace

TEST_CASE("the pump reports a clean end of stream", "[vthost][pdupump]")
{
    auto const outcome = pumpOver({});
    CHECK(outcome.stop == PumpStop::PeerClosed);
    CHECK_FALSE(outcome.decodeError.has_value());
    CHECK_FALSE(outcome.transportError.has_value());
}

TEST_CASE("the pump reports a handler-requested stop", "[vthost][pdupump]")
{
    auto seen = 0;
    auto const outcome =
        pumpOver(encoded(proto::DecodedPdu { proto::CreateTab {} }), [&](proto::DecodedFrame const&) {
            ++seen;
            return false;
        });
    CHECK(outcome.stop == PumpStop::HandlerStopped);
    CHECK(seen == 1);
}

TEST_CASE("the pump surfaces the decoder's verdict on a malformed frame", "[vthost][pdupump]")
{
    // This is what the daemon used to throw away: before PumpResult, every one of these was an
    // unexplained disconnect.
    SECTION("a varint that runs past its maximum width")
    {
        // Ten continuation bytes: a varint can never be that long.
        auto const outcome = pumpOver(std::vector<std::byte>(10, std::byte { 0x80 }));
        CHECK(outcome.stop == PumpStop::ProtocolError);
        REQUIRE(outcome.decodeError.has_value());
        CHECK(*outcome.decodeError == proto::DecodeError::MalformedVarint);
    }

    SECTION("a header declaring a payload beyond the frame cap")
    {
        auto writer = proto::Writer {};
        writer.varint(std::numeric_limits<std::uint64_t>::max() & ~std::uint64_t { 1 });
        auto const outcome = pumpOver({ writer.view().begin(), writer.view().end() });
        CHECK(outcome.stop == PumpStop::ProtocolError);
        REQUIRE(outcome.decodeError.has_value());
        CHECK(*outcome.decodeError == proto::DecodeError::FrameTooLarge);
    }

    SECTION("the reserved compression bit")
    {
        auto writer = proto::Writer {};
        writer.varint(uint64_t { 4 } << 1 | 1); // payload length 4, compression bit set
        auto const outcome = pumpOver({ writer.view().begin(), writer.view().end() });
        CHECK(outcome.stop == PumpStop::ProtocolError);
        REQUIRE(outcome.decodeError.has_value());
        CHECK(*outcome.decodeError == proto::DecodeError::CompressedFrame);
    }
}

TEST_CASE("NeedMoreData is not a protocol error", "[vthost][pdupump]")
{
    // A frame arriving in pieces must keep the pump reading, not kill the connection. The peer
    // here hangs up mid-frame, so the pump ends on the stream, never on the decoder.
    auto const whole = encoded(proto::DecodedPdu { proto::ResizeRequest { .columns = 80, .lines = 24 } });
    REQUIRE(whole.size() > 2);
    auto partial = std::vector<std::byte> { whole.begin(), whole.end() - 1 };

    auto const outcome = pumpOver(partial);
    CHECK(outcome.stop == PumpStop::PeerClosed);
    CHECK_FALSE(outcome.decodeError.has_value());
}

TEST_CASE("the pump decodes several frames from one write", "[vthost][pdupump]")
{
    auto bytes = encoded(proto::DecodedPdu { proto::CreateTab {} }, 1);
    auto const second = encoded(proto::DecodedPdu { proto::NewWindow {} }, 2);
    bytes.insert(bytes.end(), second.begin(), second.end());

    auto seen = std::vector<proto::PduType> {};
    auto const outcome = pumpOver(bytes, [&](proto::DecodedFrame const& frame) {
        seen.push_back(proto::typeOf(frame.pdu));
        return true;
    });

    CHECK(outcome.stop == PumpStop::PeerClosed);
    CHECK(seen == std::vector { proto::PduType::CreateTab, proto::PduType::NewWindow });
}
