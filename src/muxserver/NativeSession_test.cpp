// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <coro/WhenAll.hpp>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using coro::Task;
using muxserver::NativeSession;
using muxserver::SessionHost;
namespace proto = muxserver::proto;

namespace
{

/// Writes the pre-encoded request bytes onto the wire.
Task<void> feedBytes(net::ISocket* client, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// Decodes server PDUs until @p expected arrived (or the stream ended), then
/// closes the client end — the session's read loop sees EOF and finishes.
Task<void> collectPdus(net::ISocket* client, std::size_t expected, std::vector<proto::DecodedFrame>* out)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::array<std::byte, 4096> {};
    while (out->size() < expected)
    {
        auto decoded = proto::decodePdu(buffer);
        if (decoded)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(decoded->consumed));
            out->push_back(std::move(*decoded));
            continue;
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            break;
        auto const n = co_await client->read(scratch);
        if (!n.has_value() || *n == 0)
            break;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
    }
    client->close();
}

Task<void> driveExchange(NativeSession* session,
                         net::ISocket* client,
                         std::vector<std::byte> const* request,
                         std::size_t expected,
                         std::vector<proto::DecodedFrame>* out)
{
    co_await coro::whenAll(session->run(), feedBytes(client, request), collectPdus(client, expected, out));
}

/// Drives a NativeSession over an in-memory socket pair.
struct NativeHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> session =
        std::make_unique<NativeSession>(loop, host, std::move(pair.first));

    /// Encodes @p pdus, runs the exchange, returns the first @p expected server PDUs.
    std::vector<proto::DecodedFrame> exchange(std::vector<proto::DecodedPdu> const& pdus,
                                              std::size_t expected)
    {
        auto request = proto::Writer {};
        auto serial = uint64_t { 1 };
        for (auto const& pdu: pdus)
            proto::encodePdu(request, serial++, pdu);
        auto const bytes = std::vector<std::byte> { request.view().begin(), request.view().end() };

        auto received = std::vector<proto::DecodedFrame> {};
        loop.blockOn(driveExchange(session.get(), pair.second.get(), &bytes, expected, &received));
        return received;
    }
};

} // namespace

TEST_CASE("the native handshake answers ServerHello and a full snapshot", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("hello native");

    // Expect: ServerHello, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 3);
    REQUIRE(received.size() == 3);

    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->codecVersion == proto::CodecVersion);

    auto const* state = std::get_if<proto::SessionState>(&received[1].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->session == sessionId.value);
    CHECK(state->columns == 80);
    CHECK(state->lines == 25);

    auto const* delta = std::get_if<proto::Delta>(&received[2].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    REQUIRE(delta->lines.size() == 25); // the whole page ships on attach

    // The written text arrived cell by cell on the first row.
    auto text = std::string {};
    for (auto const& cell: delta->lines.front().cells)
        if (cell.codepoint != 0)
            text += static_cast<char>(cell.codepoint);
    CHECK(text == "hello native");
}

TEST_CASE("Input PDUs land in the target pane's PTY", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto input = proto::Input { .session = sessionId.value, .data = {} };
    for (auto const ch: std::string_view { "ls\r" })
        input.data.push_back(static_cast<std::byte>(ch));

    std::ignore = h.exchange({ proto::ClientHello {}, proto::DecodedPdu { input } }, 3);

    auto& tapped = dynamic_cast<muxserver::TappingPty&>(h.host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(mock.stdinBuffer() == "ls\r");
}

TEST_CASE("FetchImage for an unknown id answers ImageGone", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    auto const received =
        h.exchange({ proto::ClientHello {}, proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } }, 4);
    REQUIRE(received.size() == 4);
    auto const* gone = std::get_if<proto::ImageGone>(&received[3].pdu);
    REQUIRE(gone != nullptr);
    CHECK(gone->imageId == 4242);
    CHECK(received[3].serial == 2); // correlated to the request's serial
}

TEST_CASE("a version-mismatched hello is answered and the session ends", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    auto const received = h.exchange({ proto::ClientHello { .codecVersion = 9999 } }, 2);
    // Only the ServerHello arrives — no snapshot follows a failed handshake.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ServerHello>(received[0].pdu));
}
