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
        FetchImage { .imageId = 77 },
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
                       .palette = { 0x000000FF, 0xFF0000FF } },
        Delta { .session = 9,
                .generation = 2,
                .seqno = 1234,
                .snapshot = 1,
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
                .setModes = { 1, 25, 1006, 2004 } },
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
    body.u32(1); // FetchImage's body ...
    body.u8(0);  // ... plus a stray byte
    auto stream = Writer {};
    writeFrame(stream, 1, std::to_underlying(PduType::FetchImage), body.view());

    CHECK(decodePdu(stream.view()).error() == DecodeError::TrailingBytes);
}

TEST_CASE("serial zero marks an unsolicited push", "[muxserver][proto]")
{
    auto stream = Writer {};
    encodePdu(stream, 0, DecodedPdu { ImageGone { .imageId = 5 } });
    auto const decoded = decodePdu(stream.view());
    REQUIRE(decoded.has_value());
    CHECK(decoded->serial == 0);
}
