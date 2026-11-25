// SPDX-License-Identifier: Apache-2.0
//
// PDUs over a channel pair: whole frames arrive whole, and a peer that lies, floods or leaves ends the stream
// for the reason it gave.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

using namespace vtextension::wasm::remote;
using transport::makeChannelPair;
using transport::PduStream;
using transport::StreamError;

namespace
{

/// The control channel's two ends, as Contour and the extension host process hold them.
struct ControlPair
{
    PduStream parent;
    PduStream helper;
};

/// @return A connected control channel.
[[nodiscard]] ControlPair makeControlPair()
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    return ControlPair {
        .parent = PduStream { std::move(pair->parent),
                              proto::ChannelKind::Control,
                              proto::Direction::ToParent,
                              proto::PduLimits {} },
        .helper = PduStream { std::move(pair->helper),
                              proto::ChannelKind::Control,
                              proto::Direction::ToHelper,
                              proto::PduLimits {} },
    };
}

} // namespace

TEST_CASE("PduStream.roundTripsOverAPair", "[vtextension]")
{
    auto pair = makeControlPair();

    auto const hello = proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                                   .memoryMode = proto::MemoryMode::Copy,
                                                   .lockdown = proto::LockdownState::Unsandboxed } };
    REQUIRE(pair.helper.send(hello).has_value());
    CHECK(pair.parent.receive() == hello);

    // A module larger than a socket's buffer, followed at once by a second PDU: each arrives whole.
    auto const load = proto::Pdu { proto::Load {
        .runtimeId = 1,
        .moduleId = 2,
        .bytes = std::vector<std::byte>(std::size_t { 3 } * 1024 * 1024, std::byte { 7 }) } };
    auto const release = proto::Pdu { proto::Release { .target = proto::ReleaseTarget::Module, .id = 2 } };
    auto sent = std::async(std::launch::async, [&] {
        return pair.parent.send(load).and_then([&] { return pair.parent.send(release); });
    });
    CHECK(pair.helper.receive() == load);
    CHECK(pair.helper.receive() == release);
    CHECK(sent.get().has_value());
}

TEST_CASE("PduStream.overCapFailsWithoutWaitingForTheBody", "[vtextension]")
{
    auto channels = makeChannelPair();
    REQUIRE(channels.has_value());
    auto stream = PduStream { std::move(channels->parent),
                              proto::ChannelKind::Control,
                              proto::Direction::ToParent,
                              proto::PduLimits {} };

    // A Hello header claiming a megabyte, and nothing after it: the peer stays connected and silent.
    auto writer = proto::Writer {};
    writer.varint(1 + (std::size_t { 1024 } * 1024));
    writer.u8(std::to_underlying(proto::PduKind::Hello));
    REQUIRE(channels->helper.send(writer.take()).has_value());
    CHECK(stream.receive() == std::unexpected(StreamError::OverCap));
}

TEST_CASE("PduStream.overCapIsNotSent", "[vtextension]")
{
    auto pair = makeControlPair();

    // A runtime with more host functions than any reader accepts: refused here, so the helper never sees a
    // protocol violation, and the stream carries on. Small enough to fit the socket's buffer, so that a
    // sender that did send it fails the checks below rather than blocks.
    auto const tooMany = proto::Pdu { proto::CreateRuntime {
        .runtimeId = 1,
        .memoryBytes = 0,
        .stackBytes = 0,
        .functions = std::vector<proto::WireHostFunction>(proto::MaxHostFunctions + 1) } };
    CHECK(pair.parent.send(tooMany) == std::unexpected(StreamError::OverCap));

    auto const release = proto::Pdu { proto::Release { .target = proto::ReleaseTarget::Runtime, .id = 1 } };
    REQUIRE(pair.parent.send(release).has_value());
    CHECK(pair.helper.receive() == release);
}

TEST_CASE("PduStream.peerCloseIsClosed", "[vtextension]")
{
    auto pair = makeControlPair();
    {
        auto const helper = std::move(pair.helper);
    }
    CHECK(pair.parent.receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("PduStream.garbageIsMalformed", "[vtextension]")
{
    auto channels = makeChannelPair();
    REQUIRE(channels.has_value());
    auto stream = PduStream { std::move(channels->parent),
                              proto::ChannelKind::Control,
                              proto::Direction::ToParent,
                              proto::PduLimits {} };

    SECTION("a kind naming no PDU")
    {
        REQUIRE(channels->helper.send(std::vector<uint8_t> { 1, 0 }).has_value());
        CHECK(stream.receive() == std::unexpected(StreamError::Malformed));
    }
    SECTION("a PDU travelling the other way")
    {
        auto const frame =
            proto::encodeFrame(proto::Release { .target = proto::ReleaseTarget::Module, .id = 2 });
        REQUIRE(frame.has_value());
        REQUIRE(channels->helper.send(*frame).has_value());
        CHECK(stream.receive() == std::unexpected(StreamError::Malformed));
    }
}
