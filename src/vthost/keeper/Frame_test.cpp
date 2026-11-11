// SPDX-License-Identifier: Apache-2.0
#include <core/codec/Stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <vector>

#include <vthost/keeper/Codec.hpp>
#include <vthost/keeper/Frame.hpp>

using namespace vthost::keeper;

TEST_CASE("a frame split across reads is reassembled with its descriptors", "[keeper][frame]")
{
    auto const sent =
        Frame { .body = encode(Spawned { .request = 1, .session = 2, .pid = 3 }), .fds = { 17 } };
    auto const wire = encodeFrame(sent);

    auto assembler = FrameAssembler {};
    // The descriptor rides with the first byte; the rest of the bytes come in single-byte reads.
    assembler.feed(std::span { wire }.first(1), sent.fds);
    for (auto const index: std::views::iota(size_t { 1 }, wire.size()))
    {
        CHECK(!assembler.next().has_value());
        assembler.feed(std::span { wire }.subspan(index, 1), {});
    }
    auto const received = assembler.next();
    REQUIRE(received.has_value());
    CHECK(received->body == sent.body);
    CHECK(received->fds == sent.fds);
    CHECK(!assembler.error().has_value());
}

TEST_CASE("descriptors that do not match the header are a protocol error", "[keeper][frame]")
{
    auto const sent = Frame { .body = encode(Heartbeat {}), .fds = {} };
    auto assembler = FrameAssembler {};
    auto const stray = std::array { 17 };
    assembler.feed(encodeFrame(sent), stray);
    CHECK(!assembler.next().has_value());
    CHECK(assembler.error() == FrameError::DescriptorMismatch);
    CHECK(assembler.takePendingFds() == std::vector<int> { 17 });
}

TEST_CASE("a frame's descriptors come out with it", "[keeper][frame]")
{
    auto const sent = Frame { .body = encode(Structure {}), .fds = { 21 } };
    auto const wire = encodeFrame(sent);
    auto const half = wire.size() / 2;

    auto assembler = FrameAssembler {};
    assembler.feed(std::span { wire }.first(half), sent.fds);
    CHECK(!assembler.next().has_value());
    assembler.feed(std::span { wire }.subspan(half), {});
    auto const received = assembler.next();
    REQUIRE(received.has_value());
    CHECK(received->body == sent.body);
    CHECK(received->fds == sent.fds);
    CHECK(!assembler.error().has_value());
}

TEST_CASE("two frames in one read come out in order", "[keeper][frame]")
{
    auto const first = Frame { .body = encode(Heartbeat {}), .fds = {} };
    auto const second = Frame { .body = encode(Placed { .session = 9 }), .fds = {} };
    auto wire = encodeFrame(first);
    auto const more = encodeFrame(second);
    wire.insert(wire.end(), more.begin(), more.end());

    auto assembler = FrameAssembler {};
    assembler.feed(wire, {});
    auto const one = assembler.next();
    auto const two = assembler.next();
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(one->body == first.body);
    CHECK(two->body == second.body);
    CHECK(!assembler.next().has_value());
}

TEST_CASE("descriptors arriving with the second frame stay with it", "[keeper][frame]")
{
    auto const first = Frame { .body = encode(Heartbeat {}), .fds = {} };
    auto const second = Frame { .body = encode(Structure {}), .fds = { 30 } };
    auto assembler = FrameAssembler {};
    assembler.feed(encodeFrame(first), {});
    assembler.feed(encodeFrame(second), second.fds);

    auto const one = assembler.next();
    auto const two = assembler.next();
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(one->fds.empty());
    CHECK(two->fds == second.fds);
}

TEST_CASE("descriptors read together with a preceding plain frame stay with the frame they rode on",
          "[keeper][frame]")
{
    // A stream socket glues a plain frame to a following descriptor-carrying one in one read, and the
    // descriptors arrive with that read: they belong to the frame that announces them, not to the first.
    auto const plain = Frame { .body = encode(Heartbeat {}), .fds = {} };
    auto const carrying = Frame { .body = encode(Structure {}), .fds = { 30, 31 } };
    auto wire = encodeFrame(plain);
    auto const second = encodeFrame(carrying);
    auto const split = second.size() / 2;
    wire.insert(wire.end(), second.begin(), second.begin() + static_cast<std::ptrdiff_t>(split));
    auto assembler = FrameAssembler {};
    assembler.feed(wire, carrying.fds);
    assembler.feed(std::span { second }.subspan(split), {});

    auto const one = assembler.next();
    auto const two = assembler.next();
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(one->fds.empty());
    CHECK(two->fds == carrying.fds);
    CHECK(!assembler.error().has_value());
}

TEST_CASE("descriptors that arrived after a frame's first byte are a protocol error", "[keeper][frame]")
{
    auto const sent = Frame { .body = encode(Structure {}), .fds = { 30 } };
    auto const wire = encodeFrame(sent);
    auto assembler = FrameAssembler {};
    assembler.feed(std::span { wire }.first(1), {});
    assembler.feed(std::span { wire }.subspan(1), sent.fds);
    CHECK(!assembler.next().has_value());
    CHECK(assembler.error() == FrameError::DescriptorMismatch);
    CHECK(assembler.takePendingFds() == sent.fds);
}

TEST_CASE("a broken header ends the stream", "[keeper][frame]")
{
    SECTION("too many descriptors")
    {
        auto writer = core::codec::Writer {};
        writer.varint(1);
        writer.varint(MaxFdsPerFrame + 1);
        writer.u8(0);
        auto const wire = std::move(writer).take();
        auto assembler = FrameAssembler {};
        assembler.feed(wire, {});
        CHECK(!assembler.next().has_value());
        CHECK(assembler.error() == FrameError::TooManyDescriptors);
    }
    SECTION("an oversized body")
    {
        auto writer = core::codec::Writer {};
        writer.varint(MaxFrameBodySize + 1);
        writer.varint(0);
        auto const wire = std::move(writer).take();
        auto assembler = FrameAssembler {};
        assembler.feed(wire, {});
        CHECK(!assembler.next().has_value());
        CHECK(assembler.error() == FrameError::BodyTooLarge);
    }
    SECTION("an overlong varint")
    {
        auto const wire = std::vector<uint8_t>(11, 0xFF);
        auto assembler = FrameAssembler {};
        assembler.feed(wire, {});
        CHECK(!assembler.next().has_value());
        CHECK(assembler.error() == FrameError::MalformedHeader);
    }
}
