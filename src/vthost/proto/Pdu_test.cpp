// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <limits>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/proto/Pdu.hpp>

using namespace vthost::proto;

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

TEST_CASE("every catalog PDU round-trips", "[vthost][proto]")
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

    auto const pdus =
        std::vector<DecodedPdu> {
            // Both ClientHello shapes: the settings block is optional, so the presence byte needs a
            // round trip in each direction, not just the interesting one.
            ClientHello { .codecVersion = CodecVersion, .token = "s3cr3t-token" },
            ClientHello {
                .codecVersion = CodecVersion,
                .token = "s3cr3t-token",
                .sessionSettings =
                    WireSessionSettings { .historyLineCount = -1, // unlimited
                                          .terminalId = 41,       // VT420
                                          .graphemeClustering = 0,
                                          .allowReflowOnResize = 0,
                                          .maxImageRegisterCount = 1024,
                                          .wordDelimiters = " /\\()\"'-.,:;<>~!@#$%^&*|+=[]{}~?",
                                          .frozenModes = { WireFrozenMode { .mode = 2027, .frozenAs = 1 },
                                                           WireFrozenMode { .mode = 12, .frozenAs = 0 } } } },
            ServerHello { .codecVersion = CodecVersion },
            Input { .session = 9, .data = { std::byte { 0x1B }, std::byte { '[' }, std::byte { 'A' } } },
            ResizeRequest { .columns = 120, .lines = 40 },
            ResizePane { .session = 9, .columns = 50, .lines = 30 },
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
                           .cwd = "file:///home/user/src",
                           .statusDisplayType = 1,
                           .activeStatusDisplay = 0,
                           .kittyKeyboardFlags = 5,
                           .modifyOtherKeys = 2,
                           .progressState = 1,
                           .progressPercentage = 42 },
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
                    .setAnsiModes = { 20 }, // LNM — its own number space, so its own field
                    .titleChanged = 1,
                    .title = "~/src/contour — vim",
                    .cursorShapeChanged = 1,
                    .cursorShape = 4,
                    .cwdChanged = 1,
                    .cwd = "file:///home/user",
                    .colorsChanged = 1,
                    .defaultForeground = 0xD0D0D0,
                    .defaultBackground = 0x1A1716,
                    .statusChanged = 1,
                    .statusDisplayType = 2,
                    .activeStatusDisplay = 1,
                    .statusLinesChanged = 1,
                    .statusLines = { line },
                    .kittyKeyboardChanged = 1,
                    .kittyKeyboardFlags = 11,
                    .modifyOtherKeysChanged = 1,
                    .modifyOtherKeys = 2,
                    .progressChanged = 1,
                    .progressState = 2,
                    .progressPercentage = 80 },
        SessionBell { .session = 4 },
        SessionNotify { .session = 4, .title = "Build finished", .body = "3 warnings, 0 errors" },
        SessionClipboard { .session = 4, .selection = "c", .data = "copied text" },
            LayoutState {
                .window = 1,
                .activeTab = 0,
                .tabs = { WireTab { .tabId = 5,
                                    .activePane = 12,
                                    .zoomedPane = 0,
                                    .title = "editor",
                                    .hasColor = 1,
                                    .color = 0x336699,
                                    .root =
                                        WirePane {
                                            .paneId = 10,
                                            .split = 2, // vertical split
                                            .session = 0,
                                            .ratio = 6000,
                                            .children = { WirePane { .paneId = 11, .session = 100 },
                                                          WirePane { .paneId = 12, .session = 101 } },
                                        } } },
            },
            CreateTab {},                  // no preference: the daemon's own first window
            CreateTab { .session = 4711 }, // named window, via a session it hosts
            SplitPane { .session = 5, .orientation = 2, .ratio = 6000 },
            ClosePane { .session = 100 },
            ResizeSplit { .firstSession = 5, .secondSession = 9, .ratio = 7250 },
            NewWindow {},
        };

    for (auto const& pdu: pdus)
        CHECK(roundTrip(pdu) == pdu);
}

TEST_CASE("a layout split pane must carry exactly two children", "[vthost][proto]")
{
    // A split pane (split != None) with the wrong child count, or an out-of-range
    // split state, must be rejected at decode: otherwise the layout converters index
    // children[0]/[1] out of bounds on the reconstructed tree.
    auto layoutWith = [](uint8_t split, std::vector<WirePane> children) {
        return DecodedPdu { LayoutState {
            .window = 1,
            .activeTab = 0,
            .tabs = { WireTab {
                .tabId = 1,
                .root = WirePane {
                    .paneId = 1, .split = split, .session = 0, .children = std::move(children) } } } } };
    };
    auto encodeThenDecode = [](DecodedPdu const& pdu) {
        auto stream = Writer {};
        encodePdu(stream, 5, pdu);
        return decodePdu(stream.view());
    };
    auto const leafA = WirePane { .paneId = 2, .session = 100 };
    auto const leafB = WirePane { .paneId = 3, .session = 101 };

    SECTION("a split with no children is malformed")
    {
        CHECK(encodeThenDecode(layoutWith(2, {})).error() == DecodeError::MalformedPdu);
    }
    SECTION("a split with a single child is malformed")
    {
        CHECK(encodeThenDecode(layoutWith(2, { leafA })).error() == DecodeError::MalformedPdu);
    }
    SECTION("an out-of-range split state is malformed")
    {
        CHECK(encodeThenDecode(layoutWith(7, { leafA, leafB })).error() == DecodeError::MalformedPdu);
    }
    SECTION("a leaf carrying children is malformed")
    {
        CHECK(encodeThenDecode(layoutWith(0, { leafA, leafB })).error() == DecodeError::MalformedPdu);
    }
    SECTION("a well-formed binary split still decodes")
    {
        CHECK(encodeThenDecode(layoutWith(2, { leafA, leafB })).has_value());
    }
}

TEST_CASE("a SplitPane verb with an out-of-range orientation is malformed", "[vthost][proto]")
{
    // The verb's orientation is a vtworkspace::SplitState value: exactly 1 (Horizontal)
    // or 2 (Vertical), rejected at decode exactly like WirePane.split. An invalid
    // SplitState reaching the layout tree renders as a phantom leaf (orientation
    // 0 with children) or re-serializes as garbage every client rejects.
    auto encodeThenDecode = [](uint8_t orientation) {
        auto stream = Writer {};
        encodePdu(
            stream, 5, DecodedPdu { SplitPane { .session = 5, .orientation = orientation, .ratio = 5000 } });
        return decodePdu(stream.view());
    };

    SECTION("a None orientation is malformed")
    {
        CHECK(encodeThenDecode(0).error() == DecodeError::MalformedPdu);
    }
    SECTION("an out-of-range orientation is malformed")
    {
        CHECK(encodeThenDecode(3).error() == DecodeError::MalformedPdu);
        CHECK(encodeThenDecode(255).error() == DecodeError::MalformedPdu);
    }
    SECTION("Horizontal and Vertical still decode")
    {
        CHECK(encodeThenDecode(1).has_value());
        CHECK(encodeThenDecode(2).has_value());
    }
}

// A split ratio crosses the wire quantized to 1/10000, and a client decides whether to report a
// dragged divider by comparing its own ratio against the one the server last sent. That comparison is
// only sound if both ends quantize identically -- which is why there is ONE spelling of each
// direction, and why a value that survives a round trip must compare equal on the second one.
TEST_CASE("a split ratio survives the wire quantization", "[vthost][proto]")
{
    SECTION("every ratio a pane can hold round-trips back to itself")
    {
        // The whole band vtworkspace::Pane::setRatio clamps into, not a hand-picked sample: this is
        // the property the client's convergence rests on, so a gap in it is a divider two clients
        // would trade back and forth forever.
        auto stable = true;
        for (auto const wire: std::views::iota(uint16_t { 500 }, uint16_t { 9501 }))
            stable = stable && toWireRatio(fromWireRatio(wire)) == wire;
        CHECK(stable);
    }
    SECTION("an arbitrary drag settles after ONE quantization")
    {
        // A drag lands between two wire steps; the comparison must agree on the second pass rather
        // than differ forever.
        for (auto const ratio: { 0.05, 0.5, 0.61237, 0.95 })
        {
            auto const wire = toWireRatio(ratio);
            CHECK(toWireRatio(fromWireRatio(wire)) == wire);
        }
    }
    SECTION("a degenerate value decodes to an even split rather than a collapsed pane")
    {
        CHECK(fromWireRatio(0) == 0.5);
        CHECK(fromWireRatio(10000) == 0.5);
        CHECK(fromWireRatio(60000) == 0.5);
    }
    SECTION("a nonsense ratio saturates instead of wrapping the narrowing conversion")
    {
        CHECK(toWireRatio(-1.0) == 0);
        CHECK(toWireRatio(0.0) == 0);
        CHECK(toWireRatio(7.5) == 10000);
        CHECK(toWireRatio(std::numeric_limits<double>::quiet_NaN()) == 0);
    }
}

TEST_CASE("a blank line needs no cells on the wire", "[vthost][proto]")
{
    auto blank = WireLine {};
    blank.stableId = 12;
    blank.columns = 80;
    auto delta = Delta {};
    delta.lines = { blank };
    auto const pdu = DecodedPdu { delta };
    CHECK(roundTrip(pdu) == pdu);
}

TEST_CASE("an unknown ident decodes to Invalid and keeps the stream in sync", "[vthost][proto]")
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

TEST_CASE("trailing bytes after a known body are a protocol error", "[vthost][proto]")
{
    auto body = Writer {};
    body.varint(0); // FetchImage's body: session ...
    body.u32(1);    // ... then imageId ...
    body.u8(0);     // ... plus a stray byte
    auto stream = Writer {};
    writeFrame(stream, 1, std::to_underlying(PduType::FetchImage), body.view());

    CHECK(decodePdu(stream.view()).error() == DecodeError::TrailingBytes);
}

TEST_CASE("a complete frame whose body runs short is malformed, not incomplete", "[vthost][proto]")
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

TEST_CASE("an absurd element count fails cleanly without a huge allocation", "[vthost][proto]")
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

TEST_CASE("an announced grid beyond MaxGridExtent is malformed", "[vthost][proto]")
{
    // The SERVER's announced dimensions were the one size on this wire nobody bounded, while the
    // client's PROPOSED ones were checked at 10000. That asymmetry mattered: the client sizes a
    // repaint buffer as lines * columns * 25 and hands the pair to a real terminal to allocate a
    // grid, so a daemon naming four billion columns steered its own client into an allocation it
    // cannot make. `assign` narrows with a plain static_cast, so the value need not even be
    // representable to arrive.
    auto const sessionStateWith = [](uint64_t columns, uint64_t lines) {
        auto body = Writer {};
        body.varint(9);       // session
        body.varint(columns); // columns
        body.varint(lines);   // lines
        body.u8(0);           // screenType
        body.svarint(0);      // cursorLine
        body.svarint(0);      // cursorColumn
        body.u8(0);           // cursorShape
        body.u8(1);           // cursorVisible
        body.string("");      // title
        body.u32(0);          // defaultForeground
        body.u32(0);          // defaultBackground
        body.varint(0);       // palette size
        body.string("");      // cwd
        body.u8(0);           // statusDisplayType
        body.u8(0);           // activeStatusDisplay
        body.u8(0);           // kittyKeyboardFlags
        body.u8(0);           // modifyOtherKeys
        // Hand-built, so this list must stay in step with encodeBody(Writer&, SessionState const&) —
        // a missing field makes the decode fail here rather than anywhere informative.
        body.u16(0); // mouseProtocol
        body.u8(0);  // mouseTransport
        body.u8(0);  // mouseWheelMode
        body.u8(0);  // progressState
        body.u8(0);  // progressPercentage
        auto stream = Writer {};
        writeFrame(stream, 1, std::to_underlying(PduType::SessionState), body.view());
        return stream;
    };

    SECTION("the largest sane grid still decodes")
    {
        auto const stream = sessionStateWith(MaxGridExtent, MaxGridExtent);
        auto const decoded = decodePdu(stream.view());
        REQUIRE(decoded.has_value());
        auto const* const state = std::get_if<SessionState>(&decoded->pdu);
        REQUIRE(state != nullptr);
        CHECK(state->columns == MaxGridExtent);
    }

    SECTION("one column past it is refused")
    {
        auto const stream = sessionStateWith(MaxGridExtent + 1, 24);
        auto const decoded = decodePdu(stream.view());
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }

    SECTION("a value too large for the field at all is refused, not truncated")
    {
        // 2^32 + 80 would land as 80 through the static_cast.
        auto const stream = sessionStateWith((uint64_t { 1 } << 32U) + 80, 24);
        auto const decoded = decodePdu(stream.view());
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }
}

TEST_CASE("serial zero marks an unsolicited push", "[vthost][proto]")
{
    auto stream = Writer {};
    encodePdu(stream, 0, DecodedPdu { ImageGone { .imageId = 5 } });
    auto const decoded = decodePdu(stream.view());
    REQUIRE(decoded.has_value());
    CHECK(decoded->serial == 0);
}

TEST_CASE("asSessionEvent projects the catalog onto the transient events", "[vthost][proto]")
{
    // The question a consumer asks once instead of testing each event tag itself.
    auto const bell = asSessionEvent(DecodedPdu { SessionBell { .session = 3 } });
    REQUIRE(bell.has_value());
    CHECK(std::holds_alternative<SessionBell>(*bell));
    CHECK(sessionOf(*bell) == 3);

    auto const notify =
        asSessionEvent(DecodedPdu { SessionNotify { .session = 4, .title = "t", .body = "b" } });
    REQUIRE(notify.has_value());
    CHECK(std::holds_alternative<SessionNotify>(*notify));
    CHECK(sessionOf(*notify) == 4);

    auto const clipboard =
        asSessionEvent(DecodedPdu { SessionClipboard { .session = 5, .selection = "c", .data = "x" } });
    REQUIRE(clipboard.has_value());
    CHECK(std::holds_alternative<SessionClipboard>(*clipboard));
    CHECK(sessionOf(*clipboard) == 5);
}

TEST_CASE("asSessionEvent rejects everything that is not a session event", "[vthost][proto]")
{
    // Including Invalid, whose sole field happens to be a uint64_t like SessionBell's: the
    // projection is by TYPE, so a shape coincidence cannot make one pass for the other.
    CHECK_FALSE(asSessionEvent(DecodedPdu { Invalid { .ident = 3 } }).has_value());
    CHECK_FALSE(asSessionEvent(DecodedPdu { ClientHello {} }).has_value());
    CHECK_FALSE(asSessionEvent(DecodedPdu { Delta {} }).has_value());
    CHECK_FALSE(asSessionEvent(DecodedPdu { CreateTab {} }).has_value());
    CHECK_FALSE(asSessionEvent(DecodedPdu { ClosePane { .session = 3 } }).has_value());
}

TEST_CASE("the bounded reserve caps MEMORY, not the element count", "[vthost][proto]")
{
    // Bounding the element count by the byte count only limits the allocation for ONE-byte
    // elements. A single frame at MaxFrameSize claiming 2^26 lines used to reserve
    // 2^26 * sizeof(WireLine) — some three gigabytes — from any peer that can reach the socket,
    // which either throws bad_alloc out of the std::expected path or gets the daemon OOM-killed,
    // taking every hosted shell with it.
    auto constexpr Frame = static_cast<std::size_t>(MaxFrameSize);
    auto constexpr Claimed = std::size_t { 1 } << 26;

    CHECK(boundedReserveCount<WireLine>(Claimed, Frame) * sizeof(WireLine) <= Frame);
    CHECK(boundedReserveCount<WireCell>(Claimed, Frame) * sizeof(WireCell) <= Frame);
    CHECK(boundedReserveCount<uint32_t>(Claimed, Frame) * sizeof(uint32_t) <= Frame);
    CHECK(boundedReserveCount<HyperlinkEntry>(Claimed, Frame) * sizeof(HyperlinkEntry) <= Frame);
    CHECK(boundedReserveCount<ImageCellEntry>(Claimed, Frame) * sizeof(ImageCellEntry) <= Frame);

    // An honest count below the bound is reserved in full — the cap must not throttle real traffic.
    CHECK(boundedReserveCount<WireLine>(3, Frame) == 3);
    // ...and a body that cannot back a single element reserves nothing rather than underflowing.
    CHECK(boundedReserveCount<WireLine>(Claimed, 0) == 0);
    CHECK(boundedReserveCount<WireLine>(Claimed, sizeof(WireLine) - 1) == 0);
}

namespace
{

/// A Delta body carrying exactly one line of one cell, whose codepoint is written as the raw varint
/// @p codepoint — which a `char32_t` field cannot otherwise be made to hold.
/// @param codepoint The value to put on the wire.
/// @return The framed PDU bytes.
[[nodiscard]] Writer deltaWithCodepoint(uint64_t codepoint)
{
    auto body = deltaHeaderBody();
    body.varint(1);  // one line
    body.svarint(7); // stableId
    body.u16(0);     // flags
    body.svarint(0); // promptEndOffset
    body.svarint(0); // commandEndOffset
    body.varint(1);  // columns
    body.varint(1);  // one cell
    body.varint(codepoint);
    body.varint(0); // no cluster extras
    body.u8(1);     // width
    body.u8(1);     // scale
    body.u16(0);    // textScaleExtras
    body.u16(0);    // hyperlink
    body.u32(0);    // foreground
    body.u32(0);    // background
    body.u32(0);    // underlineColor
    body.u32(0);    // flags

    auto stream = Writer {};
    writeFrame(stream, 1, std::to_underlying(PduType::Delta), body.view());
    return stream;
}

} // namespace

TEST_CASE("a codepoint that is not a Unicode scalar value is malformed", "[vthost][proto]")
{
    // `assign`'s range check only fires for StandardInteger types, and char32_t is excluded from
    // that concept — so the codepoint fell into the unchecked branch and was static_cast from a
    // uint64. A peer naming 0x1'0000'0041 had it truncate to 'A' (the client renders the wrong
    // glyph, silently), and anything in (0x10FFFF, 0xFFFFFFFF] survived INTACT into the mirror's
    // SoA, then into libunicode's width()/grapheme segmenter and the font shaper as a value that is
    // not a scalar at all.
    SECTION("a value that would truncate into a plausible one")
    {
        auto const decoded = decodePdu(deltaWithCodepoint(0x1'0000'0041ULL).view());
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }

    SECTION("a value that fits char32_t but names no scalar")
    {
        auto const decoded = decodePdu(deltaWithCodepoint(MaxCodepoint + 1).view());
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }

    SECTION("the last scalar value itself still round-trips")
    {
        auto cell = WireCell {};
        cell.codepoint = static_cast<char32_t>(MaxCodepoint);
        auto line = WireLine {};
        line.columns = 1;
        line.cells = { cell };
        auto delta = Delta {};
        delta.lines = { line };
        auto const pdu = DecodedPdu { delta };
        CHECK(roundTrip(pdu) == pdu);
    }
}

TEST_CASE("a cluster continuation codepoint is bounded like the base one", "[vthost][proto]")
{
    // The extras reach the grapheme segmenter and the shaper through exactly the same path, so a
    // range check on the base codepoint alone would leave the door open.
    auto cell = WireCell {};
    cell.codepoint = U'e';
    cell.clusterExtras = { static_cast<char32_t>(0x110000) };
    auto line = WireLine {};
    line.columns = 1;
    line.cells = { cell };
    auto delta = Delta {};
    delta.lines = { line };

    auto stream = Writer {};
    encodePdu(stream, 1, DecodedPdu { delta });
    auto const decoded = decodePdu(stream.view());
    REQUIRE(!decoded.has_value());
    CHECK(decoded.error() == DecodeError::MalformedPdu);
}

TEST_CASE("Delta::hasChanges answers for every gated field", "[vthost][proto]")
{
    // The send site used to spell this out as a 13-term condition, so every `…Changed` gate added
    // to the struct had to be remembered over there too — and forgetting one meant the field was
    // populated, the delta was dropped whenever nothing else moved, and the client never learned of
    // the change, with no error anywhere.
    CHECK_FALSE(Delta {}.hasChanges());

    auto const gates = std::vector<uint8_t Delta::*> {
        &Delta::snapshot,           &Delta::titleChanged,         &Delta::cursorShapeChanged,
        &Delta::cwdChanged,         &Delta::colorsChanged,        &Delta::statusChanged,
        &Delta::statusLinesChanged, &Delta::kittyKeyboardChanged, &Delta::modifyOtherKeysChanged,
        &Delta::mouseChanged,       &Delta::progressChanged,
    };
    for (auto const gate: gates)
    {
        auto delta = Delta {};
        delta.*gate = 1;
        CHECK(delta.hasChanges());
    }

    // Changed rows are the non-byte gate, and the one that carries the payload.
    auto withLines = Delta {};
    withLines.lines = { WireLine {} };
    CHECK(withLines.hasChanges());

    // The two peer-relative facts are deliberately NOT the delta's business: it cannot know what
    // this peer was last told, so the sender adds them.
    auto moved = Delta {};
    moved.cursorLine = 4;
    moved.setModes = { 1049 };
    CHECK_FALSE(moved.hasChanges());
}
