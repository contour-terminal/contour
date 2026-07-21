// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

#include <muxserver/proto/Wire.h>

using namespace muxserver::proto;

TEST_CASE("varint round-trips boundary values", "[muxserver][proto]")
{
    for (auto const value: { uint64_t { 0 },
                             uint64_t { 1 },
                             uint64_t { 127 },
                             uint64_t { 128 },
                             uint64_t { 16383 },
                             uint64_t { 16384 },
                             std::numeric_limits<uint64_t>::max() })
    {
        auto writer = Writer {};
        writer.varint(value);
        auto reader = Reader { writer.view() };
        auto const back = reader.varint();
        REQUIRE(back.has_value());
        CHECK(*back == value);
        CHECK(reader.remaining() == 0);
    }
}

TEST_CASE("svarint round-trips signed values via zigzag", "[muxserver][proto]")
{
    for (auto const value: { int64_t { 0 },
                             int64_t { -1 },
                             int64_t { 1 },
                             int64_t { -4000 }, // a stable id pushed below the origin
                             std::numeric_limits<int64_t>::min(),
                             std::numeric_limits<int64_t>::max() })
    {
        auto writer = Writer {};
        writer.svarint(value);
        auto reader = Reader { writer.view() };
        auto const back = reader.svarint();
        REQUIRE(back.has_value());
        CHECK(*back == value);
    }
}

TEST_CASE("a truncated varint asks for more data, an overlong one is malformed", "[muxserver][proto]")
{
    auto const partial = std::vector<std::byte> { std::byte { 0x80 }, std::byte { 0x80 } };
    auto reader = Reader { partial };
    CHECK(reader.varint().error() == DecodeError::NeedMoreData);

    auto const overlong = std::vector<std::byte>(11, std::byte { 0x80 });
    auto reader2 = Reader { overlong };
    CHECK(reader2.varint().error() == DecodeError::MalformedVarint);
}

TEST_CASE("scalars and strings round-trip", "[muxserver][proto]")
{
    auto writer = Writer {};
    writer.u8(0xAB);
    writer.u16(0xCDEF);
    writer.u32(0xDEADBEEF);
    writer.string("caf\xc3\xa9");

    auto reader = Reader { writer.view() };
    CHECK(*reader.u8() == 0xAB);
    CHECK(*reader.u16() == 0xCDEF);
    CHECK(*reader.u32() == 0xDEADBEEF);
    CHECK(*reader.string() == "caf\xc3\xa9");
    CHECK(reader.remaining() == 0);
}

TEST_CASE("a frame round-trips and reports its consumed size", "[muxserver][proto]")
{
    auto body = Writer {};
    body.string("payload");

    auto stream = Writer {};
    writeFrame(stream, 42, 7, body.view());
    auto const trailerStart = stream.size();
    stream.u8(0xFF); // unrelated bytes after the frame must not be consumed

    auto const frame = readFrame(stream.view());
    REQUIRE(frame.has_value());
    CHECK(frame->serial == 42);
    CHECK(frame->ident == 7);
    CHECK(frame->consumed == trailerStart);

    auto bodyReader = Reader { frame->body };
    CHECK(*bodyReader.string() == "payload");
    CHECK(bodyReader.remaining() == 0);
}

TEST_CASE("every strict prefix of a frame asks for more data", "[muxserver][proto]")
{
    auto body = Writer {};
    body.u32(0x12345678);
    auto stream = Writer {};
    writeFrame(stream, 1, 3, body.view());

    auto const bytes = stream.view();
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
    {
        auto const frame = readFrame(bytes.subspan(0, cut));
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error() == DecodeError::NeedMoreData);
    }
    CHECK(readFrame(bytes).has_value());
}

TEST_CASE("the reserved compression bit is rejected, not misread", "[muxserver][proto]")
{
    auto stream = Writer {};
    stream.varint(1); // tagged length with the compressed bit set (value 1 = len 0 | bit)
    CHECK(readFrame(stream.view()).error() == DecodeError::CompressedFrame);
}
