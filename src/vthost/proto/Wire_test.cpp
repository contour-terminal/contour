// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

#include <vthost/proto/Wire.h>

using namespace vthost::proto;

TEST_CASE("varint round-trips boundary values", "[vthost][proto]")
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

TEST_CASE("svarint round-trips signed values via zigzag", "[vthost][proto]")
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

TEST_CASE("a truncated varint asks for more data, an overlong one is malformed", "[vthost][proto]")
{
    auto const partial = std::vector<std::byte> { std::byte { 0x80 }, std::byte { 0x80 } };
    auto reader = Reader { partial };
    CHECK(reader.varint().error() == DecodeError::NeedMoreData);

    auto const overlong = std::vector<std::byte>(11, std::byte { 0x80 });
    auto reader2 = Reader { overlong };
    CHECK(reader2.varint().error() == DecodeError::MalformedVarint);
}

TEST_CASE("a non-canonical ten-byte varint is malformed, not truncated", "[vthost][proto]")
{
    // Nine continuation bytes leave shift == 63; the tenth group may carry only
    // bit 63, so a value above 0x01 would overflow past uint64 and must be rejected
    // rather than silently losing its high bits to the `<< 63` shift.
    auto const overflowing = std::vector<std::byte>(9, std::byte { 0x80 });

    for (auto const terminator: { uint8_t { 0x02 }, uint8_t { 0x40 }, uint8_t { 0x7F } })
    {
        auto bytes = overflowing;
        bytes.push_back(std::byte { terminator });
        auto reader = Reader { bytes };
        CHECK(reader.varint().error() == DecodeError::MalformedVarint);
    }

    // The canonical maximum still decodes: nine 0xFF groups plus a final 0x01 is
    // exactly uint64::max, and the guard must not reject it.
    auto canonicalMax = std::vector<std::byte>(9, std::byte { 0xFF });
    canonicalMax.push_back(std::byte { 0x01 });
    auto reader = Reader { canonicalMax };
    auto const decoded = reader.varint();
    REQUIRE(decoded.has_value());
    CHECK(*decoded == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("scalars and strings round-trip", "[vthost][proto]")
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

TEST_CASE("a frame round-trips and reports its consumed size", "[vthost][proto]")
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

TEST_CASE("every strict prefix of a frame asks for more data", "[vthost][proto]")
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

TEST_CASE("the reserved compression bit is rejected, not misread", "[vthost][proto]")
{
    auto stream = Writer {};
    stream.varint(1); // tagged length with the compressed bit set (value 1 = len 0 | bit)
    CHECK(readFrame(stream.view()).error() == DecodeError::CompressedFrame);
}

TEST_CASE("an over-large declared frame length is rejected, not buffered toward", "[vthost][proto]")
{
    // A peer declares a payload beyond MaxFrameSize and sends only the header. Without the cap this
    // returns NeedMoreData and the read loop keeps buffering toward the (huge) declared length — an
    // unbounded-memory DoS. With the cap it is a hard FrameTooLarge, so the loop tears the peer down.
    auto stream = Writer {};
    stream.varint((MaxFrameSize + 1) << 1); // tagged length: payload = MaxFrameSize + 1, no compression

    auto const frame = readFrame(stream.view());
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == DecodeError::FrameTooLarge);

    // Even declaring the largest representable payload (the 2^63-class attack) is rejected outright
    // rather than mistaken for "read more".
    auto huge = Writer {};
    huge.varint(std::numeric_limits<uint64_t>::max() & ~uint64_t { 1 }); // max payload, compression bit clear
    CHECK(readFrame(huge.view()).error() == DecodeError::FrameTooLarge);
}

TEST_CASE("a frame at exactly the size cap still decodes", "[vthost][proto]")
{
    // The bound is inclusive: a payload of exactly MaxFrameSize is legal, so the cap never rejects a
    // frame a compliant peer could send. (Check the boundary arithmetic without materializing 64 MiB:
    // a header declaring MaxFrameSize with the body absent must ask for more data, not FrameTooLarge.)
    auto stream = Writer {};
    stream.varint(MaxFrameSize << 1);
    CHECK(readFrame(stream.view()).error() == DecodeError::NeedMoreData);
}
