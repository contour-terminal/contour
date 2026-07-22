// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

#include <muxserver/proto/Pdu.h>

using namespace muxserver::proto;

namespace
{

/// Encodes @p pdu and decodes it back, checking framing invariants on the way.
DecodedPdu roundTrip(DecodedPdu const& pdu, uint64_t serial = 5)
{
    auto stream = Writer {};
    encodePdu(stream, serial, pdu);
    auto const decoded = decodePdu(stream.view());
    REQUIRE(decoded.has_value());
    CHECK(decoded->serial == serial);
    CHECK(decoded->consumed == stream.size());
    return decoded->pdu;
}

/// A Delta body up to (but excluding) its lines-count varint: the eight fixed
/// header fields every Delta opens with. Tests append a (possibly lying) count
/// to probe the truncation guards without hand-rolling the whole prefix twice.
Writer deltaHeaderBody()
{
    auto body = Writer {};
    body.varint(9);   // session
    body.varint(2);   // generation
    body.varint(1);   // seqno
    body.u8(1);       // snapshot
    body.svarint(0);  // stableViewportBase
    body.svarint(0);  // stableFloor
    body.svarint(5);  // cursorLine
    body.svarint(10); // cursorColumn
    return body;
}

} // namespace

TEST_CASE("every catalog PDU round-trips", "[muxserver][proto]")
{
    auto const cell = WireCell {
        .codepoint = U'a',
        .clusterExtras = { 0x1F3FB }, // a skin-tone modifier: the cluster pool path
        .width = 2,
        .scale = 2,
        .textScaleExtras = 0x1234,
        .hyperlink = 7,
        .foreground = 0x11223344,
        .background = 0x55667788,
        .underlineColor = 0x99AABBCC,
        .flags = 0xFFFFF, // all 20 CellFlags bits
    };
    auto const line = WireLine {
        .stableId = -3, // signed: SD/unscroll push ids below the origin
        .flags = 0x01FF,
        .columns = 80,
        .cells = { cell },
        .fillForeground = 1,
        .fillBackground = 2,
    };

    auto const pdus = std::vector<DecodedPdu> {
        ClientHello { .codecVersion = CodecVersion },
        ServerHello { .codecVersion = CodecVersion },
        Input { .session = 9, .data = { std::byte { 0x1B }, std::byte { '[' }, std::byte { 'A' } } },
        ResizeRequest { .columns = 120, .lines = 40 },
        FetchImage { .session = 9, .imageId = 77 },
        ImageData { .imageId = 77,
                    .format = 1,
                    .width = 2,
                    .height = 1,
                    .data = std::vector<std::byte>(8, std::byte { 0xAB }) },
        ImageGone { .imageId = 78 },
        SessionState { .session = 9,
                       .columns = 120,
                       .lines = 40,
                       .screenType = 1,
                       .cursorLine = 39,
                       .cursorColumn = 0,
                       .cursorShape = 2,
                       .cursorVisible = 1,
                       .title = "vim",
                       .defaultForeground = 0xDDDDDDFF,
                       .defaultBackground = 0x000000FF,
                       .palette = { 0x000000FF, 0xFF0000FF },
                       .cwd = "file:///home/user/src" },
        Delta { .session = 9,
                .generation = 2,
                .seqno = 1234,
                .snapshot = 1,
                .stableFloor = -5,
                .cursorLine = 5,
                .cursorColumn = 10,
                .lines = { line },
                .hyperlinks = { HyperlinkEntry { .id = 7, .uri = "https://example.com" } },
                .imageCells = { ImageCellEntry { .stableId = -3,
                                                 .column = 4,
                                                 .imageId = 77,
                                                 .offsetLine = 0,
                                                 .offsetColumn = 1,
                                                 .layer = 2 } },
                .setModes = { 1, 25, 1006, 2004 },
                .titleChanged = 1,
                .title = "~/src/contour — vim",
                .cursorShapeChanged = 1,
                .cursorShape = 4,
                .cwdChanged = 1,
                .cwd = "file:///home/user" },
        SessionEvent { .session = 4, .kind = 1, .a = "Build finished", .b = "3 warnings, 0 errors" },
    };

    for (auto const& pdu: pdus)
        CHECK(roundTrip(pdu) == pdu);
}

TEST_CASE("a blank line needs no cells on the wire", "[muxserver][proto]")
{
    auto blank = WireLine {};
    blank.stableId = 12;
    blank.columns = 80;
    auto delta = Delta {};
    delta.lines = { blank };
    auto const pdu = DecodedPdu { delta };
    CHECK(roundTrip(pdu) == pdu);
}

TEST_CASE("an unknown ident decodes to Invalid and keeps the stream in sync", "[muxserver][proto]")
{
    // A future PDU with a body this decoder has never heard of.
    auto body = Writer {};
    body.string("from the future");
    auto stream = Writer {};
    writeFrame(stream, 3, 999, body.view());
    encodePdu(stream, 4, DecodedPdu { FetchImage { .imageId = 1 } }); // a known one behind it

    auto const first = decodePdu(stream.view());
    REQUIRE(first.has_value());
    CHECK(first->pdu == DecodedPdu { Invalid { .ident = 999 } });

    // The unknown frame's size was still consumed exactly, so the next decode works.
    auto const second = decodePdu(stream.view().subspan(first->consumed));
    REQUIRE(second.has_value());
    CHECK(second->pdu == DecodedPdu { FetchImage { .imageId = 1 } });
}

TEST_CASE("trailing bytes after a known body are a protocol error", "[muxserver][proto]")
{
    auto body = Writer {};
    body.varint(0); // FetchImage's body: session ...
    body.u32(1);    // ... then imageId ...
    body.u8(0);     // ... plus a stray byte
    auto stream = Writer {};
    writeFrame(stream, 1, std::to_underlying(PduType::FetchImage), body.view());

    CHECK(decodePdu(stream.view()).error() == DecodeError::TrailingBytes);
}

TEST_CASE("a complete frame whose body runs short is malformed, not incomplete", "[muxserver][proto]")
{
    // readFrame confirms the whole frame is buffered; a body shorter than the
    // structure it declares must therefore be fatal, NOT NeedMoreData — else
    // the pump co_awaits socket data that can never complete an already-whole
    // frame and the connection wedges forever.
    SECTION("a lying element count")
    {
        auto body = deltaHeaderBody();
        body.varint(3); // declares three lines ...
        // ... but no line bytes follow.
        auto stream = Writer {};
        writeFrame(stream, 1, std::to_underlying(PduType::Delta), body.view());

        auto const decoded = decodePdu(stream.view());
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }

    SECTION("a truncated fixed field")
    {
        // FetchImage's body is a session varint then a u32 imageId; give the
        // u32 only two bytes so the scalar read hits end-of-body mid-value.
        auto body = Writer {};
        body.varint(0);
        body.u16(0x1234);
        auto stream = Writer {};
        writeFrame(stream, 1, std::to_underlying(PduType::FetchImage), body.view());

        auto const decoded = decodePdu(stream.view());
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }
}

TEST_CASE("an absurd element count fails cleanly without a huge allocation", "[muxserver][proto]")
{
    // A malicious peer declares four billion lines in a byte-sized body. The
    // decoder must not reserve() four billion elements (bad_alloc thrown out
    // of the std::expected path kills the coroutine silently); the bounded
    // reserve caps at the remaining bytes and the loop trips MalformedPdu.
    auto body = deltaHeaderBody();
    body.varint(0xFFFFFFFFU);
    auto stream = Writer {};
    writeFrame(stream, 1, std::to_underlying(PduType::Delta), body.view());

    auto const decoded = decodePdu(stream.view());
    REQUIRE(!decoded.has_value());
    CHECK(decoded.error() == DecodeError::MalformedPdu);
}

TEST_CASE("serial zero marks an unsolicited push", "[muxserver][proto]")
{
    auto stream = Writer {};
    encodePdu(stream, 0, DecodedPdu { ImageGone { .imageId = 5 } });
    auto const decoded = decodePdu(stream.view());
    REQUIRE(decoded.has_value());
    CHECK(decoded->serial == 0);
}
