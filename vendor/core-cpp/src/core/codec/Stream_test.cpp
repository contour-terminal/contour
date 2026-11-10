// SPDX-License-Identifier: Apache-2.0
#include <core/codec/Stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <vector>

using core::codec::DecodeError;
using core::codec::Reader;
using core::codec::Writer;

TEST_CASE("codec.Stream: varints round-trip at their edges", "[codec]")
{
    constexpr auto Values =
        std::array<uint64_t, 7> { 0, 1, 127, 128, 16383, 16384, std::numeric_limits<uint64_t>::max() };
    auto writer = Writer {};
    for (auto const value: Values)
        writer.varint(value);
    auto const bytes = std::move(writer).take();
    auto reader = Reader { bytes };
    for (auto const value: Values)
    {
        auto const read = reader.varint();
        REQUIRE(read.has_value());
        CHECK(*read == value);
    }
    CHECK(reader.atEnd());
}

TEST_CASE("codec.Stream: signed varints are zigzag", "[codec]")
{
    auto writer = Writer {};
    writer.svarint(-1);
    writer.svarint(std::numeric_limits<int64_t>::min());
    writer.svarint(63);
    auto const bytes = std::move(writer).take();
    CHECK(bytes[0] == 1); // -1 -> 1
    auto reader = Reader { bytes };
    CHECK(*reader.svarint() == -1);
    CHECK(*reader.svarint() == std::numeric_limits<int64_t>::min());
    CHECK(*reader.svarint() == 63);
}

TEST_CASE("codec.Stream: a varint that does not fit 64 bits is refused", "[codec]")
{
    auto const tooLong = std::vector<uint8_t> { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 };
    auto reader = Reader { tooLong };
    auto const read = reader.varint();
    REQUIRE_FALSE(read.has_value());
    CHECK(read == std::unexpected(DecodeError::MalformedRecord));

    auto const endless = std::vector<uint8_t>(11, 0x80);
    auto endlessReader = Reader { endless };
    CHECK(endlessReader.varint() == std::unexpected(DecodeError::MalformedRecord));
}

TEST_CASE("codec.Stream: a read past the end is Truncated", "[codec]")
{
    auto const bytes = std::vector<uint8_t> { 0x80 };
    auto reader = Reader { bytes };
    CHECK(reader.varint() == std::unexpected(DecodeError::Truncated));
    auto empty = Reader { std::span<uint8_t const> {} };
    CHECK(empty.u8() == std::unexpected(DecodeError::Truncated));
    CHECK(empty.raw(1) == std::unexpected(DecodeError::Truncated));
}

TEST_CASE("codec.Stream: strings and bytes are length-prefixed", "[codec]")
{
    auto writer = Writer {};
    writer.string("hello");
    writer.bytes(std::array<uint8_t, 2> { 7, 8 });
    auto const bytes = std::move(writer).take();
    auto reader = Reader { bytes };
    CHECK(*reader.string() == "hello");
    auto const data = reader.bytes();
    REQUIRE(data.has_value());
    CHECK(data->size() == 2);
    CHECK(reader.atEnd());

    auto const lying = std::vector<uint8_t> { 5, 'a' };
    auto lyingReader = Reader { lying };
    CHECK(lyingReader.string() == std::unexpected(DecodeError::Truncated));
}

TEST_CASE("codec.Stream: frames close with the one byte reserved, or move up for a long payload", "[codec]")
{
    auto writer = Writer {};
    auto const shortFrame = writer.beginFrame();
    writer.raw(std::vector<uint8_t>(10, 1));
    writer.endFrame(shortFrame);
    CHECK(writer.size() == 11);

    auto const longFrame = writer.beginFrame();
    writer.raw(std::vector<uint8_t>(300, 2));
    writer.endFrame(longFrame);
    CHECK(writer.size() == 11 + 2 + 300);

    auto const bytes = std::move(writer).take();
    auto reader = Reader { bytes };
    CHECK(reader.bytes()->size() == 10);
    CHECK(reader.bytes()->size() == 300);
    CHECK(reader.atEnd());
}
