// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/state/StateFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{
/// A struct as an older release wrote it: two fields.
struct OldSample
{
    uint64_t header = 0;
    std::vector<int64_t> items;

    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.header);
        visitor(2, self.items);
    }
};

/// The same struct as a newer release writes it: one field more, of each kind, and one nested.
struct NewSample
{
    uint64_t header = 0;
    std::vector<int64_t> items;
    std::string note;
    int32_t level = 0;
    std::optional<OldSample> nested;

    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.header);
        visitor(2, self.items);
        visitor(3, self.note);
        visitor(4, self.level);
        visitor(5, self.nested);
    }
};

/// One enum field.
struct Holder
{
    CursorShape shape = CursorShape::Block;

    void visitFields(this auto& self, auto& visitor) { visitor(1, self.shape); }
};

/// Two char fields, as UserPreferredSupplementalSet has.
struct CharPair
{
    char first = char {};
    char second = char {};

    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.first);
        visitor(2, self.second);
    }
};

template <typename T>
std::vector<uint8_t> encodeSample(T const& sample)
{
    auto writer = StateWriter::payload();
    detail::encodeFields(writer, sample);
    return std::move(writer).take();
}
} // namespace

TEST_CASE("a stream from a newer writer decodes, its unknown fields skipped", "[StateCodec]")
{
    auto const bytes = encodeSample(NewSample { .header = 42,
                                                .items = { -5, 0, 9 },
                                                .note = "only the newer release knows me",
                                                .level = -3,
                                                .nested = OldSample { .header = 7, .items = { 1 } } });
    auto decoded = OldSample {};
    REQUIRE(detail::decodeFields(bytes, decoded).has_value());
    CHECK(decoded.header == 42);
    CHECK(decoded.items == std::vector<int64_t> { -5, 0, 9 });
}

TEST_CASE("a stream from an older writer decodes, its missing fields defaulted", "[StateCodec]")
{
    auto const bytes = encodeSample(OldSample { .header = 42, .items = { 1, 2 } });
    auto decoded = NewSample {};
    REQUIRE(detail::decodeFields(bytes, decoded).has_value());
    CHECK(decoded.header == 42);
    CHECK(decoded.items == std::vector<int64_t> { 1, 2 });
    CHECK(decoded.note.empty());
    CHECK(decoded.level == 0);
    CHECK_FALSE(decoded.nested.has_value());
}

TEST_CASE("a field written twice is MalformedRecord", "[StateCodec]")
{
    auto writer = StateWriter::payload();
    writer.varint(detail::fieldKey(1, detail::WireKind::Varint));
    writer.varint(1);
    writer.varint(detail::fieldKey(1, detail::WireKind::Varint));
    writer.varint(2);
    auto const bytes = std::move(writer).take();
    auto decoded = OldSample {};
    CHECK(detail::decodeFields(bytes, decoded) == std::unexpected { StateDecodeError::MalformedRecord });
}

TEST_CASE("a field of the wrong kind is MalformedRecord", "[StateCodec]")
{
    auto writer = StateWriter::payload();
    writer.varint(detail::fieldKey(1, detail::WireKind::Framed));
    writer.string("not a number");
    auto const bytes = std::move(writer).take();
    auto decoded = OldSample {};
    CHECK(detail::decodeFields(bytes, decoded) == std::unexpected { StateDecodeError::MalformedRecord });
}

TEST_CASE("an enumerator crosses as its wire index and an unknown index is refused", "[StateCodec]")
{
    auto const bytes = encodeSample(Holder { .shape = CursorShape::Bar });
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[1] == detail::wireIndexOf(CursorShape::Bar));

    auto tampered = bytes;
    tampered[1] = static_cast<uint8_t>(detail::StateEnumWire<CursorShape>::Values.size());
    auto decoded = Holder {};
    CHECK(detail::decodeFields(tampered, decoded) == std::unexpected { StateDecodeError::InvalidValue });
}

TEST_CASE("a frame longer than 127 bytes gets its length written in front of it", "[StateCodec]")
{
    auto writer = StateWriter::payload();
    auto const frame = writer.beginFrame();
    for ([[maybe_unused]] auto const index: std::views::iota(0, 300))
        writer.u8(0x5A);
    writer.endFrame(frame);
    writer.u8(0xEE);
    auto const bytes = std::move(writer).take();

    auto reader = StateReader::ofPayload(bytes);
    auto const payload = reader.bytes();
    REQUIRE(payload.has_value());
    CHECK(payload->size() == 300);
    CHECK(std::ranges::all_of(*payload, [](uint8_t byte) { return byte == 0x5A; }));
    CHECK(reader.u8().value() == 0xEE);
    CHECK(reader.atEnd());
}

TEST_CASE("varint round-trips 0, 127, 128, 2^63", "[StateCodec]")
{
    auto const values = std::array<uint64_t, 5> { 0, 127, 128, uint64_t { 1 } << 63U, UINT64_MAX };
    auto writer = StateWriter {};
    for (auto const value: values)
        writer.varint(value);
    auto const bytes = std::move(writer).take();

    auto reader = StateReader::open(bytes);
    REQUIRE(reader.has_value());
    for (auto const value: values)
    {
        auto const decoded = reader->varint();
        REQUIRE(decoded.has_value());
        CHECK(*decoded == value);
    }
    CHECK(reader->atEnd());
}

TEST_CASE("varint uses the LEB128 byte layout", "[StateCodec]")
{
    auto writer = StateWriter {};
    writer.varint(128);
    auto const bytes = std::move(writer).take();
    // magic + version byte, then 128 = 0x80 0x01.
    REQUIRE(bytes.size() == 4 + 1 + 2);
    CHECK(bytes[5] == 0b1000'0000);
    CHECK(bytes[6] == 0b0000'0001);
}

TEST_CASE("svarint round-trips INT64_MIN and -1", "[StateCodec]")
{
    auto const values = std::array<int64_t, 5> {
        std::numeric_limits<int64_t>::min(), -1, 0, 1, std::numeric_limits<int64_t>::max()
    };
    auto writer = StateWriter {};
    for (auto const value: values)
        writer.svarint(value);
    auto const bytes = std::move(writer).take();

    auto reader = StateReader::open(bytes);
    REQUIRE(reader.has_value());
    for (auto const value: values)
    {
        auto const decoded = reader->svarint();
        REQUIRE(decoded.has_value());
        CHECK(*decoded == value);
    }
}

TEST_CASE("strings and byte runs round-trip", "[StateCodec]")
{
    auto writer = StateWriter {};
    writer.string("hello");
    writer.string("");
    writer.u8(0xAB);
    auto const bytes = std::move(writer).take();

    auto reader = StateReader::open(bytes);
    REQUIRE(reader.has_value());
    CHECK(reader->string().value() == "hello");
    CHECK(reader->string().value().empty());
    CHECK(reader->u8().value() == 0xAB);
    CHECK(reader->atEnd());
}

TEST_CASE("a truncated byte run is Truncated", "[StateCodec]")
{
    auto writer = StateWriter {};
    writer.string("a body of some length");
    auto bytes = std::move(writer).take();
    bytes.resize(bytes.size() - 3);

    auto reader = StateReader::open(bytes);
    REQUIRE(reader.has_value());
    auto const run = reader->bytes();
    REQUIRE_FALSE(run.has_value());
    CHECK(run.error() == StateDecodeError::Truncated);
}

TEST_CASE("a stream shorter than its header is Truncated", "[StateCodec]")
{
    auto const bytes = std::array<uint8_t, 2> { 'C', 'T' };
    auto const reader = StateReader::open(bytes);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error() == StateDecodeError::Truncated);
}

TEST_CASE("a stream without the magic is MalformedRecord", "[StateCodec]")
{
    auto const bytes = std::array<uint8_t, 5> { 'X', 'T', 'S', 'T', 1 };
    auto const reader = StateReader::open(bytes);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error() == StateDecodeError::MalformedRecord);
}

TEST_CASE("format version above TerminalStateFormatVersion is UnknownFormatVersion", "[StateCodec]")
{
    auto const bytes = std::move(StateWriter { TerminalStateFormatVersion + 1 }).take();
    auto const reader = StateReader::open(bytes);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error() == StateDecodeError::UnknownFormatVersion);
}

TEST_CASE("format version below the oldest readable is UnknownFormatVersion", "[StateCodec]")
{
    auto const bytes = std::move(StateWriter { OldestReadableTerminalStateFormat - 1 }).take();
    auto const reader = StateReader::open(bytes);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error() == StateDecodeError::UnknownFormatVersion);
}

TEST_CASE("an over-long varint is MalformedRecord", "[StateCodec]")
{
    auto bytes = std::move(StateWriter {}).take();
    bytes.insert(bytes.end(), 11, 0xFF);
    auto reader = StateReader::open(bytes);
    REQUIRE(reader.has_value());
    auto const value = reader->varint();
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error() == StateDecodeError::MalformedRecord);
}

TEST_CASE("a varint whose tenth byte carries more than bit 63 is MalformedRecord", "[StateCodec]")
{
    // Nine groups of seven bits, then a tenth whose second bit would be bit 64: dropped by a lax decoder,
    // which then decodes another value than the one written.
    // Listed rather than `std::vector(9, 0xFF)` plus `push_back`: GCC's link-time inlining reports a
    // -Wstringop-overflow for that form that cannot happen.
    auto bytes = std::vector<uint8_t> { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0b0000'0011 };
    auto reader = StateReader::ofPayload(bytes);
    auto const value = reader.varint();
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error() == StateDecodeError::MalformedRecord);

    bytes.back() = 0b0000'0001;
    auto valid = StateReader::ofPayload(bytes);
    CHECK(valid.varint().value() == UINT64_MAX);
}

TEST_CASE("a char crosses the wire as a signed byte whether or not char is signed", "[StateCodec]")
{
    // The v1 wire form is the one x86 wrote: a signed 8-bit value, zigzag coded. A char holding 0x25 is the
    // varint 74; one holding 0xE9 (-23 as a signed byte) is 45. AArch64 Linux has an unsigned char, and wrote
    // 37 and 233 there.
    auto const pair = CharPair { .first = '%', .second = static_cast<char>(0xE9) };
    auto const bytes = encodeSample(pair);
    auto expected = StateWriter::payload();
    expected.varint(detail::fieldKey(1, detail::WireKind::Varint));
    expected.varint(74);
    expected.varint(detail::fieldKey(2, detail::WireKind::Varint));
    expected.varint(45);
    auto const expectedBytes = std::move(expected).take();
    CHECK(bytes == expectedBytes);

    auto decoded = CharPair {};
    REQUIRE(detail::decodeFields(bytes, decoded).has_value());
    CHECK(decoded.first == '%');
    CHECK(decoded.second == static_cast<char>(0xE9));

    // A value outside a signed byte is not a char.
    auto writer = StateWriter::payload();
    writer.varint(detail::fieldKey(1, detail::WireKind::Varint));
    writer.varint(0x1FF);
    auto tooWide = CharPair {};
    auto const wide = std::move(writer).take();
    CHECK_FALSE(detail::decodeFields(wide, tooWide).has_value());
}
