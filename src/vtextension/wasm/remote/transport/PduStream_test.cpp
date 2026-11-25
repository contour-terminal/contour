// SPDX-License-Identifier: Apache-2.0
//
// PDUs over a channel pair: whole frames arrive whole, and a peer that lies, floods or leaves ends the stream
// for the reason it gave.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <future>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#ifndef _WIN32
    #include <unistd.h>

    #include <vtextension/wasm/remote/transport/HandleGrant.hpp>
    #include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
    #include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>
#endif

using namespace vtextension::wasm::remote;
using transport::makeChannelPair;
using transport::PduStream;
using transport::StreamError;

namespace
{

/// What a large frame's buffer may take beyond the frame itself.
constexpr auto LargeFrameHeadroom = std::size_t { 64 };

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

TEST_CASE("PduStream.overByteCapIsNotSent", "[vtextension]")
{
    // A module larger than the stream's limits let a Load carry: its lists are within their caps, its bytes
    // are not. Refused here, as the helper's reader would refuse it, and the stream carries on.
    auto const limits = proto::PduLimits { .maxModuleBytes = 64, .maxMemoryBytes = 64 };
    auto channels = makeChannelPair();
    REQUIRE(channels.has_value());
    auto parent = PduStream {
        std::move(channels->parent), proto::ChannelKind::Control, proto::Direction::ToParent, limits
    };
    auto helper = PduStream {
        std::move(channels->helper), proto::ChannelKind::Control, proto::Direction::ToHelper, limits
    };

    auto const tooLarge =
        proto::Pdu { proto::Load { .runtimeId = 1, .moduleId = 2, .bytes = std::vector<std::byte>(256) } };
    CHECK(parent.send(tooLarge) == std::unexpected(StreamError::OverCap));

    auto const release = proto::Pdu { proto::Release { .target = proto::ReleaseTarget::Module, .id = 2 } };
    REQUIRE(parent.send(release).has_value());
    CHECK(helper.receive() == release);
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

TEST_CASE("PduStream.largeFrameAllocatesItsSizeOnce", "[vtextension]")
{
    // A module near the largest a Load carries: the buffer grows once, to the frame's size, without doubling
    // past it.
    auto pair = makeControlPair();
    auto const load = proto::Pdu { proto::Load {
        .runtimeId = 1,
        .moduleId = 2,
        .bytes = std::vector<std::byte>(proto::PduLimits {}.maxModuleBytes - 64, std::byte { 7 }) } };
    auto const frameBytes = proto::encodeFrame(load).value().size();
    auto sent = std::async(std::launch::async, [&] { return pair.parent.send(load); });
    CHECK(pair.helper.receive() == load);
    REQUIRE(sent.get().has_value());
    auto const stats = pair.helper.receiveBufferStats();
    CHECK(stats.peakBytes <= frameBytes + LargeFrameHeadroom);
    CHECK(stats.allocations <= 2); // the first chunk, then the frame
}

TEST_CASE("PduStream.keepsAModerateBufferBetweenFrames", "[vtextension]")
{
    // Frames of a few hundred KiB each, as memory reads of a busy instance are: the buffer grown for the
    // first serves the rest.
    auto pair = makeControlPair();
    auto const load = proto::Pdu { proto::Load {
        .runtimeId = 1, .moduleId = 2, .bytes = std::vector<std::byte>(std::size_t { 512 } * 1024) } };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        auto sent = std::async(std::launch::async, [&] { return pair.parent.send(load); });
        CHECK(pair.helper.receive() == load);
        REQUIRE(sent.get().has_value());
    }
    CHECK(pair.helper.receiveBufferStats().allocations <= 2);
}

TEST_CASE("PduStream.trySendKeepsAPartialFrame", "[vtextension]")
{
    using namespace std::chrono_literals;
    auto pair = makeControlPair();

    // Frames larger than half a socket's buffer, which the channel takes in part: nobody reads, so they go
    // in until one does not fit whole, and then until none fits at all.
    auto const loadNumbered = [](uint32_t moduleId) {
        return proto::Pdu { proto::Load {
            .runtimeId = 1,
            .moduleId = moduleId,
            .bytes = std::vector<std::byte>(std::size_t { 256 } * 1024, std::byte { 9 }) } };
    };
    constexpr auto MaxFrames = uint32_t { 1024 };
    auto sentFrames = uint32_t { 0 };
    auto begunTail = std::size_t { 0 };
    while (sentFrames < MaxFrames)
    {
        auto const outcome = pair.parent.trySend(loadNumbered(sentFrames + 1));
        REQUIRE(outcome.has_value());
        if (*outcome == transport::SendOutcome::WouldBlock)
            break;
        ++sentFrames;
        begunTail = pair.parent.unsentBytes();
    }
    REQUIRE(sentFrames < MaxFrames);
#ifndef _WIN32
    // A socket takes part of a frame; a pipe's write may be taken whole or not at all.
    CHECK(begunTail > 0);
#endif
    CHECK(pair.parent.unsentBytes() == begunTail);

    // A reader drains; the next send, a blocking one, finishes the frame begun before its own.
    auto reading = std::async(std::launch::async, [&] {
        auto moduleIds = std::vector<uint32_t> {};
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, sentFrames + 1))
        {
            auto const pdu = pair.helper.receive();
            if (!pdu || !std::holds_alternative<proto::Load>(*pdu))
            {
                // Ended here, so that the blocking send below fails rather than waits.
                pair.helper.shutdown();
                break;
            }
            moduleIds.push_back(std::get<proto::Load>(*pdu).moduleId);
        }
        return moduleIds;
    });
    auto const sent = pair.parent.send(loadNumbered(sentFrames + 1));
    if (reading.wait_for(5s) != std::future_status::ready)
    {
        FAIL_CHECK("the reader did not get every frame");
        std::fputs("PduStream.trySendKeepsAPartialFrame: the reader did not get every frame\n", stderr);
        std::abort();
    }
    CHECK(sent.has_value());
    CHECK(pair.parent.unsentBytes() == 0);
    auto const expected = std::views::iota(uint32_t { 1 }, sentFrames + 2);
    CHECK(reading.get() == std::vector<uint32_t>(expected.begin(), expected.end()));
}

#ifndef _WIN32
using transport::testing::inodeOf;

namespace
{

/// A shared memory object stand-in: one end of a fresh pipe, closed when it is destroyed.
[[nodiscard]] transport::OwnedHandle makeObject()
{
    auto ends = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(ends.data()) == 0);
    ::close(ends[1]);
    return transport::OwnedHandle { ends[0] };
}

/// @return An Instantiate naming @p grant's handle, as the parent sends it.
[[nodiscard]] proto::Pdu instantiateWith(transport::HandleGrant const& grant, uint32_t instanceId = 1)
{
    return proto::Instantiate { .moduleId = 1,
                                .instanceId = instanceId,
                                .interruptSlot = 0,
                                .memory = proto::MemoryGrant { .handle = grant.wireValue,
                                                               .objectBytes = proto::WasmPageBytes,
                                                               .initialBytes = proto::WasmPageBytes } };
}

/// @return A grant of @p object to this process.
[[nodiscard]] transport::HandleGrant grantOf(transport::OwnedHandle const& object)
{
    auto grant = transport::grantHandle(transport::HandleTarget::currentProcess(), object.get());
    REQUIRE(grant.has_value());
    return *grant;
}

} // namespace

TEST_CASE("PduStream.instantiateCarriesItsDescriptor", "[vtextension]")
{
    auto pair = makeControlPair();
    auto const object = makeObject();
    auto const grant = grantOf(object);
    CHECK(grant.wireValue == 0);
    REQUIRE(pair.parent.send(instantiateWith(grant), grant).has_value());

    auto const received = pair.helper.receive();
    REQUIRE(received.has_value());
    REQUIRE(std::holds_alternative<proto::Instantiate>(*received));
    auto const claimed =
        transport::claimGrantedHandle(pair.helper, std::get<proto::Instantiate>(*received).memory.handle);
    REQUIRE(claimed.has_value());
    CHECK(inodeOf(claimed->get()) == inodeOf(object.get()));
    // Claimed once: there is nothing left to claim.
    CHECK(transport::claimGrantedHandle(pair.helper, 0) == std::unexpected(StreamError::Malformed));
}

TEST_CASE("PduStream.secondUnclaimedDescriptorIsMalformed", "[vtextension]")
{
    auto pair = makeControlPair();
    auto const first = makeObject();
    auto const second = makeObject();
    // The first arrives with its descriptor, which nobody claims; the second's finds it still held. Sent one
    // at a time, as the parent sends them, so that no system reads both in one receive.
    REQUIRE(pair.parent.send(instantiateWith(grantOf(first), 1), grantOf(first)).has_value());
    auto const received = pair.helper.receive();
    REQUIRE(received.has_value());
    CHECK(std::holds_alternative<proto::Instantiate>(*received));
    REQUIRE(pair.parent.send(instantiateWith(grantOf(second), 2), grantOf(second)).has_value());
    auto const before = transport::testing::openDescriptorCount();
    CHECK(pair.helper.receive() == std::unexpected(StreamError::Malformed));
    // The second was closed at once; the first is still the stream's.
    CHECK(transport::testing::openDescriptorCount() == before);
    CHECK(pair.helper.takeAttachedHandle());
}

TEST_CASE("PduStream.lanesNeverInstallADescriptor", "[vtextension]")
{
    auto channels = makeChannelPair();
    REQUIRE(channels.has_value());
    auto lane = PduStream {
        std::move(channels->helper), proto::ChannelKind::Lane, proto::Direction::ToHelper, proto::PduLimits {}
    };
    auto const frame = proto::encodeFrame(proto::Call { .instanceId = 1, .exportOrdinal = 0, .argv = {} });
    REQUIRE(frame.has_value());
    auto const before = transport::testing::openDescriptorCount();
    {
        auto const object = makeObject();
        REQUIRE(channels->parent.sendWithHandle(*frame, object.get()).has_value());
    }
    auto const received = lane.receive();
    REQUIRE(received.has_value());
    CHECK(std::holds_alternative<proto::Call>(*received));
    CHECK_FALSE(lane.takeAttachedHandle());
    CHECK(transport::testing::openDescriptorCount() == before);
}

TEST_CASE("PduStream.claimWithoutADescriptorIsMalformed", "[vtextension]")
{
    auto pair = makeControlPair();
    auto const object = makeObject();
    // Sent without what the grant attaches: the PDU names a handle that never came.
    REQUIRE(pair.parent.send(instantiateWith(grantOf(object))).has_value());
    auto const received = pair.helper.receive();
    REQUIRE(received.has_value());
    CHECK(transport::claimGrantedHandle(pair.helper, 0) == std::unexpected(StreamError::Malformed));

    // A descriptor that came is not claimed by a value POSIX never sends.
    REQUIRE(pair.parent.send(instantiateWith(grantOf(object)), grantOf(object)).has_value());
    REQUIRE(pair.helper.receive().has_value());
    CHECK(transport::claimGrantedHandle(pair.helper, 0x1F4) == std::unexpected(StreamError::Malformed));
}
#endif
