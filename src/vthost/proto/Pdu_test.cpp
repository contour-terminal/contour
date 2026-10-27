// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/InputEchoWire.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/testing/PduExamples.hpp>
#include <vthost/testing/PreNegotiationCodec.hpp>
#include <vthost/testing/ShippedCodec.hpp>

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

/// @return The bytes of @p text.
std::vector<std::byte> bytes(std::string_view text)
{
    auto const view = text | std::views::transform([](char c) { return static_cast<std::byte>(c); });
    return { view.begin(), view.end() };
}

/// @return The body of the one frame @p pdu encodes to.
std::vector<std::byte> bodyOf(DecodedPdu const& pdu)
{
    auto stream = Writer {};
    encodePdu(stream, 1, pdu);
    auto const frame = readFrame(stream.view());
    REQUIRE(frame.has_value());
    return { frame->body.begin(), frame->body.end() };
}

/// A complete frame around a hand-made body.
/// @param type The PDU type the frame carries.
/// @param body The body bytes, as a test made them.
/// @param serial The frame's serial.
/// @return The frame's bytes.
std::vector<std::byte> frameOf(PduType type, std::span<std::byte const> body, uint64_t serial = 5)
{
    auto stream = Writer {};
    writeFrame(stream, serial, std::to_underlying(type), body);
    return stream.take();
}

/// Decodes @p frame and takes the PDU of type @p T out of it, failing the test if it is anything else.
/// @param frame One complete frame.
/// @return The decoded PDU.
template <typename T>
T decodeAs(std::span<std::byte const> frame)
{
    auto const decoded = decodePdu(frame);
    REQUIRE(decoded.has_value());
    REQUIRE(std::holds_alternative<T>(decoded->pdu));
    return std::get<T>(decoded->pdu);
}

/// Encodes @p pdu with its fields rewritten, as a peer of another revision would send them. A negotiating
/// hello keeps its fields in a length-prefixed blob: the blob is rewritten and its length prefix with it.
/// Any other PDU's fields are its whole body.
/// @param pdu The PDU, as this version encodes it.
/// @param edit Given the fields, the fields that peer sends instead.
/// @return The frame's bytes.
template <typename EditFn>
std::vector<std::byte> encodeWithFieldsEdited(DecodedPdu const& pdu, EditFn edit)
{
    auto stream = Writer {};
    encodePdu(stream, 1, pdu);
    auto const frame = readFrame(stream.view());
    REQUIRE(frame.has_value());
    auto const type = static_cast<PduType>(frame->ident);

    auto const* server = std::get_if<ServerHello>(&pdu);
    auto const inBlob = std::holds_alternative<ClientHello>(pdu)
                        || (server != nullptr && server->layout == HelloLayout::Negotiating);
    if (!inBlob)
        return frameOf(type, edit(frame->body));
    auto in = Reader { frame->body };
    auto const marker = in.u32();
    auto const fields = in.blob();
    REQUIRE(marker.has_value());
    REQUIRE(fields.has_value());
    auto const rest = in.bytes(in.remaining());
    REQUIRE(rest.has_value());

    auto out = Writer {};
    out.u32(*marker);
    out.blob(edit(*fields));
    out.bytes(*rest);
    return frameOf(type, out.view());
}

/// Encodes @p pdu with the tail of its fields cut off, as a peer from before they were appended would.
/// @param pdu The PDU, as this version encodes it.
/// @param bytesToCut Given the fields, how many bytes from their end the older peer did not send.
/// @return The frame's bytes.
template <typename CutFn>
std::vector<std::byte> encodeWithFieldsCut(DecodedPdu const& pdu, CutFn bytesToCut)
{
    return encodeWithFieldsEdited(pdu, [&](std::span<std::byte const> fields) {
        auto const cut = bytesToCut(fields);
        REQUIRE(cut <= fields.size());
        auto const kept = fields.first(fields.size() - cut);
        return std::vector<std::byte>(kept.begin(), kept.end());
    });
}

/// Encodes @p pdu with one byte appended to its fields, as a peer that appended a field would.
/// @param pdu The PDU, as this version encodes it.
/// @return The frame's bytes.
std::vector<std::byte> encodeWithFieldAppended(DecodedPdu const& pdu)
{
    return encodeWithFieldsEdited(pdu, [](std::span<std::byte const> fields) {
        auto grown = std::vector<std::byte>(fields.begin(), fields.end());
        grown.push_back(std::byte { 0x2A });
        return grown;
    });
}

/// The frame as a peer from before the last appended field would send it.
/// @param pdu The PDU, as this version encodes it.
/// @param bytesToDrop How many bytes from the end of its fields (a hello's blob, else the body) to drop.
/// @return The frame's bytes.
std::vector<std::byte> encodeWithoutTrailingField(DecodedPdu const& pdu, std::size_t bytesToDrop)
{
    return encodeWithFieldsCut(pdu, [&](std::span<std::byte const>) { return bytesToDrop; });
}

/// The frame as a peer from before the last appended `string` field would send it: that string's varint
/// length and its bytes dropped. Exact, not guessed: the caller names the string, and the test fails
/// loudly unless the PDU's fields end in precisely that string.
/// @param pdu The PDU, as this version encodes it.
/// @param last The value of the PDU's last field, a string.
/// @return The frame's bytes.
std::vector<std::byte> encodeWithoutTrailingString(DecodedPdu const& pdu, std::string_view last)
{
    return encodeWithFieldsCut(pdu, [last](std::span<std::byte const> fields) {
        auto encoded = Writer {};
        encoded.string(last);
        auto const view = encoded.view();
        REQUIRE(fields.size() >= view.size());
        REQUIRE(std::ranges::equal(fields.last(view.size()), view));
        return view.size();
    });
}

/// A Delta body up to (but excluding) its lines-count varint: the ten fixed
/// header fields every Delta opens with. Tests append a (possibly lying) count
/// to probe the truncation guards without hand-rolling the whole prefix twice.
Writer deltaHeaderBody()
{
    auto body = Writer {};
    body.varint(9);   // session
    body.varint(2);   // generation
    body.varint(1);   // seqno
    body.u8(1);       // snapshot
    body.u8(0);       // snapshotPart
    body.svarint(0);  // stableViewportBase
    body.svarint(0);  // stableFloor
    body.varint(0);   // evictedRowCount
    body.svarint(5);  // cursorLine
    body.svarint(10); // cursorColumn
    return body;
}

/// A command-block record with every optional field present and every scalar off its default, so a
/// round trip proves each one is written AND read back.
WireCommandBlock sampleBlock()
{
    auto block = WireCommandBlock {};
    block.id = 0xFFFFFFF0;
    block.state = 2;             // Finished
    block.end = 1;               // Implicit
    block.commandLineSource = 1; // Reported
    block.locality = 2;          // Foreign
    block.present = CommandBlockPresentMask;
    block.commandLine = "make -j8 test";
    block.workingDirectory = "/home/user/src/contour";
    block.endLocality = 1; // Local
    block.endWorkingDirectory = "/home/user/src/contour/build";
    block.foreignDirectory = 1; // Reported
    block.outcomeExit = 3;      // Crash
    block.outcomeSignal = 11;
    block.outcomeStatus = 139;
    block.promptStartedAt = 1'700'000'000'123'456'000;
    block.exitCode = -1; // signed on the wire: the receiver judges the range, not the codec
    block.commandStartedAt = 1'700'000'002'000'000'000;
    block.duration = 3'000'000'000;
    return block;
}

/// @return How many bytes a Delta carrying @p blocks, and nothing else, encodes to.
std::size_t encodedDeltaSize(std::vector<WireCommandBlock> blocks)
{
    auto delta = Delta {};
    delta.commandBlocks = std::move(blocks);
    auto sink = Writer {};
    encodePdu(sink, 1, DecodedPdu { delta });
    return sink.size();
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
    // Every field the mask can name, so the round trip proves each is written AND read back -- a
    // record populated only in part would let a missing decoder read pass unnoticed.
    auto const context = WireContext {
        .id = 3,
        .parent = 1,
        .identifier = "bed86fab93af4328bbed0a1224af6d40",
        .type = 6, // Container
        .present = ContextPresentMask,
        .user = "lennart",
        .hostname = "zeta",
        .machineId = "3deb5353d3ba43d08201c136a47ead7b",
        .bootId = "d4a3d0fdf2e24fdea6d971ce73f4fbf2",
        .comm = "systemd-nspawn",
        .workingDirectory = "/app",
        .commandLine = "ls -l",
        .vm = "myvm",
        .container = "foobar",
        .targetUser = "root",
        .targetHost = "example.com",
        .sessionId = "42",
        .pid = 1062862,
        .pidFdId = 1063162,
        .exitKind = 2, // Failure
        .signal = 11,  // SIGSEGV
        .status = 139,
    };
    auto const line = WireLine {
        .stableId = -3, // signed: SD/unscroll push ids below the origin
        .flags = 0x01FF,
        .contextId = 3,
        .blockId = 0xFFFFFFFF, // the whole 32 bits: host ids are never narrowed
        .bornAt = 0x00FFFFFF,  // vtbackend::Line::MaxBornAt, the saturated stamp
        .columns = 80,
        .cells = { cell },
        .fillForeground = 1,
        .fillBackground = 2,
    };

    auto const pdus =
        std::vector<DecodedPdu> {
            // Both ClientHello shapes: the settings block is optional, so the presence byte needs a
            // round trip in each direction, not just the interesting one.
            ClientHello { .token = "s3cr3t-token" },
            ClientHello {
                .versions = { 4, 7 },
                .capabilities = CapabilitySet::of({ Capability::ExtensionPermissions }),
                .token = "s3cr3t-token",
                .sessionSettings =
                    WireSessionSettings { .historyLineCount = -1, // unlimited
                                          .terminalId = 41,       // VT420
                                          .graphemeClustering = 0,
                                          .allowReflowOnResize = 0,
                                          .maxImageRegisterCount = 1024,
                                          .sixelScrolling = 0,
                                          .wordDelimiters = " /\\()\"'-.,:;<>~!@#$%^&*|+=[]{}~?",
                                          .frozenModes = { WireFrozenMode { .mode = 2027, .frozenAs = 1 },
                                                           WireFrozenMode { .mode = 12, .frozenAs = 0 } } } },
            ServerHello { .agreedVersion = 4,
                          .capabilities = CapabilitySet::of({ Capability::ExtensionPermissions }),
                          .machineId = "3deb5353d3ba43d08201c136a47ead7b",
                          .hostname = "zeta" },
            ServerHello {}, // a negotiating refusal: agreed version 0, and no machine
            // A refusal to a pre-negotiation client: the version alone, read back as a range of it.
            ServerHello { .layout = HelloLayout::VersionAlone, .versions = { NegotiatingHelloMarker, NegotiatingHelloMarker } },
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
                           .progressPercentage = 42,
                           .contexts = { context },
                           .contextChain = { 1, 3 },
                           .sessionEpoch = 1'700'000'000'123'456'000,
                           .hostTime = 1'700'000'040'000'000'000,
                           .commandBlockIdEpoch = 3,
                           .commandBlocksFloor = 0xFFFFFFFF,
                           .commandBlocks = { sampleBlock() },
                           .interruptedCommand = "make test" },
            Delta { .session = 9,
                    .generation = 2,
                    .seqno = 1234,
                    .snapshot = 1,
                    .stableFloor = -5,
                    .evictedRowCount = 0x1'0000'0001,
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
                    .statusLinesDisplay = 1,
                    .statusLines = { line },
                    .kittyKeyboardChanged = 1,
                    .kittyKeyboardFlags = 11,
                    .modifyOtherKeysChanged = 1,
                    .modifyOtherKeys = 2,
                    .progressChanged = 1,
                    .progressState = 2,
                    .progressPercentage = 80,
                    .contextChanged = 1,
                    .activeContext = 3,
                    .contexts = { context },
                    .commandBlockIdEpoch = 4,
                    .stableIdGeneration = 0x1'0000'0002,
                    .commandBlocks = { sampleBlock() },
                    .retiredCommandBlocks = { 3, 0xFFFFFFFF } },
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
            PerformOperation { .session = 5, .operation = EngineOperation::ClearToPrompt, .text = {} },
            PerformOperation {
                .session = 6, .operation = EngineOperation::WriteScreen, .text = "\033[2J\033[3J" },
            // A row's id is signed: rows the host pushed above its origin have negative ones.
            PerformOperation { .session = 7,
                               .operation = EngineOperation::SetUserMark,
                               .row = RowAddress { .stableId = -12, .generation = 0x1'0000'0003 } },
            NewWindow {},
            PermissionRequest { .session = 4,
                                .hostId = "git@/usr/share/contour/extensions/git.wasm",
                                .name = "git",
                                .version = 1,
                                .digest = "5f3c",
                                .commands = { WireDeclaredCommand { .name = "status",
                                                                    .argv = { "git", "status" } } } },
            PermissionReply { .hostId = "git@/usr/share/contour/extensions/git.wasm",
                              .digest = "5f3c",
                              .grantState = 1 },
            Unsupported { .refusedSerial = 9, .ident = 200 },
            DaemonStatusRequest {},
            DaemonStatusReply { .releaseVersion = "0.8.1",
                                .processId = 4242,
                                .uptimeSeconds = 7385,
                                .socketPath = "/run/user/1000/contour/default",
                                .executablePath = "/s/bin/0.8.1-1a2b3c4d/bin/contour",
                                .logFile = "/run/user/1000/contour/default.log",
                                .sessions = 3,
                                .clients = 1,
                                .lifecycle = 2 },
            StopDaemon { .reason = 1 },
            SessionSettingsChanged {}, // back to the daemon's own
            SessionSettingsChanged { .sessionSettings = WireSessionSettings {
                                         .historyLineCount = -1,
                                         .terminalId = 1,
                                         .wordDelimiters = " ./",
                                         .frozenModes = { WireFrozenMode { .mode = 2026, .frozenAs = 0 } } } },
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

TEST_CASE("an engine operation this build does not know is carried, not refused", "[vthost][proto]")
{
    // What an operation means is the host's to judge, as an unknown PDU is: refusing the frame here would end
    // the connection over one request a newer client made.
    auto stream = Writer {};
    encodePdu(stream,
              5,
              DecodedPdu { PerformOperation {
                  .session = 3, .operation = static_cast<EngineOperation>(200), .text = {} } });
    auto const decoded = decodePdu(stream.view());
    REQUIRE(decoded.has_value());
    auto const* request = std::get_if<PerformOperation>(&decoded->pdu);
    REQUIRE(request != nullptr);
    CHECK(std::to_underlying(request->operation) == 200);
    CHECK(request->session == 3);
}

TEST_CASE("a notification's occasion survives the wire", "[vthost][proto]")
{
    for (auto const occasion: { NotifyOccasion::Unstated,
                                NotifyOccasion::Always,
                                NotifyOccasion::Unfocused,
                                NotifyOccasion::Invisible })
    {
        INFO("occasion " << int { std::to_underlying(occasion) });
        auto const pdu =
            DecodedPdu { SessionNotify { .session = 4, .title = "t", .body = "b", .occasion = occasion } };
        CHECK(roundTrip(pdu) == pdu);
    }
}

TEST_CASE("a notification occasion this build does not know is carried, not refused", "[vthost][proto]")
{
    // What it means is the client's to judge: refusing the frame here would end the connection over one
    // notification a newer daemon sent.
    auto stream = Writer {};
    encodePdu(stream,
              0,
              DecodedPdu { SessionNotify {
                  .session = 3, .title = "t", .body = "b", .occasion = static_cast<NotifyOccasion>(200) } });
    auto const decoded = decodePdu(stream.view());
    REQUIRE(decoded.has_value());
    auto const* notify = std::get_if<SessionNotify>(&decoded->pdu);
    REQUIRE(notify != nullptr);
    CHECK(std::to_underlying(notify->occasion) == 200);
    CHECK(notify->body == "b");
}

TEST_CASE("Proto.permissionPdu.roundtrip", "[vthost][proto][extensions]")
{
    SECTION("a request carries who asks, the digest the answer binds to, and every argv template whole")
    {
        // Elements the prompt must still tell apart: an empty one, one holding a space, a placeholder.
        auto const request = DecodedPdu { PermissionRequest {
            .session = 7,
            .hostId = "git@/usr/share/contour/extensions/git.wasm",
            .name = "git",
            .version = 1,
            .digest = "9b74c9897bac770ffc029102a200c5de",
            .commands = {
                WireDeclaredCommand { .name = "status",
                                      .argv = { "git", "-C", "{dir}", "status", "--porcelain=v2" } },
                WireDeclaredCommand { .name = "odd", .argv = { "", "a b" } },
                WireDeclaredCommand { .name = "none", .argv = {} } } } };
        CHECK(roundTrip(request) == request);
    }

    SECTION("a request carries the control permissions it asks for")
    {
        auto const request = DecodedPdu { PermissionRequest { .session = 7,
                                                              .hostId = "agent@/x.wasm",
                                                              .name = "agent",
                                                              .version = 1,
                                                              .digest = "d1",
                                                              .commands = {},
                                                              .control = { "read", "input" } } };
        CHECK(roundTrip(request) == request);
    }

    SECTION("an older sender's request, which ends before the control permissions, asks for none")
    {
        auto const request =
            PermissionRequest { .session = 7,
                                .hostId = "git@/x.wasm",
                                .name = "git",
                                .version = 1,
                                .digest = "d1",
                                .commands = { WireDeclaredCommand { .name = "status", .argv = { "git" } } },
                                .control = {} };
        // The empty list is one byte, its count.
        CHECK(decodeAs<PermissionRequest>(encodeWithoutTrailingField(request, 1)) == request);
    }

    SECTION("a reply carries each answer")
    {
        for (auto const state:
             { vthost::GrantState::Pending, vthost::GrantState::Granted, vthost::GrantState::Denied })
        {
            auto const reply = DecodedPdu { PermissionReply {
                .hostId = "git@/x.wasm", .digest = "d1", .grantState = std::to_underlying(state) } };
            auto const decoded = roundTrip(reply);
            CHECK(decoded == reply);
            CHECK(vthost::grantStateOf(std::get<PermissionReply>(decoded).grantState) == state);
        }
    }

    SECTION("an answer byte no GrantState has decodes to none: total, as the status-display bytes are")
    {
        // Carried rather than refused, so that a newer client's answer does not end the connection; the
        // host then records nothing for it.
        for (auto const wire: { uint8_t { 3 }, uint8_t { 0x80 }, uint8_t { 0xFF } })
        {
            auto const decoded = roundTrip(
                DecodedPdu { PermissionReply { .hostId = "h", .digest = "d", .grantState = wire } });
            auto const* const reply = std::get_if<PermissionReply>(&decoded);
            REQUIRE(reply != nullptr);
            CHECK(reply->grantState == wire);
            CHECK_FALSE(vthost::grantStateOf(reply->grantState).has_value());
        }
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

TEST_CASE("an Unsupported names the refused frame and its tag", "[vthost][proto][negotiation]")
{
    auto const unsupported = Unsupported { .refusedSerial = 9, .ident = 200 };
    auto body = Writer {};
    body.varint(9);
    body.varint(200);
    CHECK(bodyOf(DecodedPdu { unsupported })
          == std::vector<std::byte>(body.view().begin(), body.view().end()));
    CHECK(decodeAs<Unsupported>(frameOf(PduType::Unsupported, body.view())) == unsupported);
    CHECK(std::to_underlying(PduType::Unsupported) == 24);
}

TEST_CASE("a Control-role ClientHello round-trips and an absent role reads as Attach", "[vthost][proto]")
{
    auto const hello = ClientHello { .role = ConnectionRole::Control };
    CHECK(roundTrip(DecodedPdu { hello }) == DecodedPdu { hello });
    // A hello encoded before the field existed: everything up to the role, nothing after -- neither
    // the role, the absent client area's presence byte, the intent (kind, count) nor the empty profile.
    auto const legacy = encodeWithoutTrailingField(
        DecodedPdu { ClientHello { .role = ConnectionRole::Control } }, /*bytesToDrop=*/5);
    CHECK(decodeAs<ClientHello>(legacy).role == ConnectionRole::Attach);
}

TEST_CASE("a ClientHello's client area round-trips and an absent one decodes as none", "[vthost][proto]")
{
    auto const hello = ClientHello { .clientArea = WireClientArea { .columns = 132, .lines = 43 } };
    CHECK(roundTrip(DecodedPdu { hello }) == DecodedPdu { hello });
    // A phase-2 hello: everything up to the role, and no presence byte after it (nor the intent and
    // profile that follow it now).
    auto const legacy = encodeWithoutTrailingField(
        DecodedPdu { ClientHello { .role = ConnectionRole::Control } }, /*bytesToDrop=*/4);
    auto const decoded = decodeAs<ClientHello>(legacy);
    CHECK(decoded.role == ConnectionRole::Control);
    CHECK(decoded.clientArea == std::nullopt);
    // The area is a presence byte, then two varints.
    auto const withArea = bodyOf(DecodedPdu { hello });
    auto const withoutArea = bodyOf(DecodedPdu { ClientHello {} });
    CHECK(withArea.size() == withoutArea.size() + 3); // varint 132 is two bytes, varint 43 one
}

TEST_CASE("a ClientHello whose client area is cut short is malformed", "[vthost][proto]")
{
    auto const cut = encodeWithoutTrailingField(
        DecodedPdu { ClientHello { .clientArea = WireClientArea { .columns = 80, .lines = 25 } } },
        /*bytesToDrop=*/4); // the intent and the profile, and the area's lines
    CHECK_FALSE(decodePdu(cut).has_value());
}

TEST_CASE("an unknown ConnectionRole byte reads as Control, never as Attach", "[vthost][proto]")
{
    auto const future = encodeWithFieldsEdited(DecodedPdu { ClientHello { .role = ConnectionRole::Control } },
                                               [](std::span<std::byte const> fields) {
                                                   auto edited =
                                                       std::vector<std::byte>(fields.begin(), fields.end());
                                                   // The role, just before the area's presence byte, the
                                                   // intent's kind and count, and the empty profile.
                                                   edited.rbegin()[4] = std::byte { 7 };
                                                   return edited;
                                               });
    CHECK(decodeAs<ClientHello>(future).role == ConnectionRole::Control);
}

TEST_CASE("ServerHello carries the daemon's release version, defaulting to empty", "[vthost][proto]")
{
    auto const hello = ServerHello { .releaseVersion = "0.8.1" };
    CHECK(std::get<ServerHello>(roundTrip(DecodedPdu { hello })).releaseVersion == "0.8.1");
    // The release string (length byte and five characters), and the followed-window count, the instance and
    // the two restored counts behind it.
    auto const legacy = encodeWithoutTrailingField(DecodedPdu { hello }, /*bytesToDrop=*/10);
    CHECK(decodeAs<ServerHello>(legacy).releaseVersion.empty());
}

TEST_CASE("the daemon-control PDUs round-trip", "[vthost][proto]")
{
    auto const request = DecodedPdu { DaemonStatusRequest {} };
    CHECK(roundTrip(request) == request);
    auto const reply = DecodedPdu { DaemonStatusReply { .releaseVersion = "0.8.1",
                                                        .processId = 4242,
                                                        .uptimeSeconds = 7385,
                                                        .socketPath = "/run/user/1000/contour/default",
                                                        .executablePath = "/s/bin/0.8.1-1a2b3c4d/bin/contour",
                                                        .logFile = "/run/user/1000/contour/default.log",
                                                        .sessions = 3,
                                                        .clients = 1,
                                                        .lifecycle = 2 } };
    CHECK(roundTrip(reply) == reply);
    auto const stop = DecodedPdu { StopDaemon { .reason = 1 } };
    CHECK(roundTrip(stop) == stop);
}

TEST_CASE("the daemon-control tags are pinned", "[vthost][proto]")
{
    STATIC_CHECK(std::to_underlying(PduType::DaemonStatusRequest) == 25);
    STATIC_CHECK(std::to_underlying(PduType::DaemonStatusReply) == 26);
    STATIC_CHECK(std::to_underlying(PduType::StopDaemon) == 27);
}

TEST_CASE("a known PDU with bytes appended by a newer peer decodes as the PDU",
          "[vthost][proto][negotiation]")
{
    for (auto const& [tag, pdu]: vthost::testing::everyAlternative())
    {
        INFO("PDU " << std::to_underlying(tag));
        auto const frame = encodeWithFieldAppended(pdu);
        auto const decoded = decodePdu(frame);
        REQUIRE(decoded);
        CHECK(decoded->pdu == pdu);
        CHECK(decoded->consumed == frame.size());
    }
}

TEST_CASE("the frame helpers build what they say", "[vthost][proto][negotiation]")
{
    auto const input = Input { .session = 7, .data = bytes("ls\r") };
    auto const body = bodyOf(input);

    SECTION("frameOf and decodeAs")
    {
        auto const frame = frameOf(PduType::Input, body, 9);
        CHECK(decodeAs<Input>(frame) == input);
        CHECK(decodePdu(frame)->serial == 9);
        CHECK(decodePdu(frameOf(PduType::Input, body))->serial == 5);
    }
    SECTION("encodeWithoutTrailingField cuts a plain body at its end")
    {
        auto const frame = encodeWithoutTrailingField(input, 2);
        auto const shortened = readFrame(frame);
        REQUIRE(shortened);
        CHECK(shortened->body.size() == body.size() - 2);
        CHECK(std::ranges::equal(shortened->body, std::span { body }.first(body.size() - 2)));
    }
    SECTION("encodeWithoutTrailingField cuts a hello's blob and rewrites its length")
    {
        auto const hello = ServerHello { .agreedVersion = 4,
                                         .versions = { 4, 4 },
                                         .machineId = "m",
                                         .hostname = "zeta",
                                         .releaseVersion = "r" };
        // "r", "zeta" and their lengths, the followed-window count, the instance and the two restored counts
        auto const frame = encodeWithoutTrailingField(hello, 11);
        CHECK(readFrame(frame)->body.size() == bodyOf(hello).size() - 11);
        auto expected = hello;
        expected.hostname.clear();
        expected.releaseVersion.clear();
        CHECK(decodeAs<ServerHello>(frame) == expected);
    }
    SECTION("encodeWithoutTrailingField on a ClientHello keeps its closing byte")
    {
        auto const hello =
            ClientHello { .versions = { 4, 6 },
                          .capabilities = { .idents = { 1, 77 } },
                          .token = "tok",
                          .sessionSettings = WireSessionSettings { .historyLineCount = 5000 } };
        auto const settingsSize =
            bodyOf(
                ClientHello { .versions = { 4, 6 }, .capabilities = { .idents = { 1, 77 } }, .token = "tok" })
                .size();
        auto const dropped =
            bodyOf(hello).size() - settingsSize + 6; // the settings, less the absent byte, the role, the
                                                     // absent client area, the intent and the empty profile
        auto expected = hello;
        expected.sessionSettings.reset();
        CHECK(decodeAs<ClientHello>(encodeWithoutTrailingField(hello, dropped)) == expected);

        auto const noToken = encodeWithoutTrailingField(hello, dropped + 4); // and the token "tok"
        expected.token.clear();
        CHECK(decodeAs<ClientHello>(noToken) == expected);
    }
    SECTION("encodeWithoutTrailingString drops exactly the named last string")
    {
        auto const hello = ClientHello { .token = "tok", .profileName = "work" };
        auto const frame = encodeWithoutTrailingString(hello, "work");
        CHECK(readFrame(frame)->body.size() == bodyOf(hello).size() - 5);
        auto expected = hello;
        expected.profileName.clear();
        CHECK(decodeAs<ClientHello>(frame) == expected);
    }
    SECTION("encodeWithFieldAppended grows a plain body at its end, and a hello's blob")
    {
        auto const frame = encodeWithFieldAppended(input);
        auto const grown = readFrame(frame);
        REQUIRE(grown);
        CHECK(grown->body.size() == body.size() + 1);
        CHECK(grown->body.back() == std::byte { 0x2A });

        auto const hello = ServerHello { .agreedVersion = 4, .versions = { 4, 4 }, .hostname = "zeta" };
        auto const helloBody = bodyOf(hello);
        auto const helloFrame = encodeWithFieldAppended(hello);
        auto const grownHello = readFrame(helloFrame);
        REQUIRE(grownHello);
        CHECK(grownHello->body.size() == helloBody.size() + 1);
        auto in = Reader { grownHello->body };
        CHECK(in.u32() == NegotiatingHelloMarker);
        auto const fields = in.blob();
        REQUIRE(fields);
        CHECK(fields->back() == std::byte { 0x2A });
    }
    SECTION("the surviving fields of a shortened plain body still decode")
    {
        auto const frame = encodeWithoutTrailingField(input, 0);
        CHECK(decodeAs<Input>(frame) == input);
    }
}

// ---------------------------------------------------------------------------------------------
// The handshake across protocol versions. Codecs 1 (contour 0.7.0), 2 (contour 0.8.0) and 3 predate
// negotiation, and a daemon of any of them keeps running across an upgrade: each must read a negotiating
// hello far enough to refuse it, and a client of each must read the refusal it is sent.
// ---------------------------------------------------------------------------------------------

namespace
{
/// @return The frame @p pdu, a negotiating hello, encodes to, with a field a newer peer appended at the
///         end of its field blob.
std::vector<std::byte> withAppendedField(PduType tag, DecodedPdu const& pdu)
{
    auto const body = bodyOf(pdu);
    auto in = Reader { body };
    auto const marker = in.u32();
    auto const fields = in.blob();
    REQUIRE(marker.has_value());
    REQUIRE(fields.has_value());
    auto const rest = in.bytes(in.remaining());
    REQUIRE(rest.has_value());

    auto grown = Writer {};
    grown.bytes(*fields);
    grown.string("a field this version has never heard of");
    grown.varint(4711);
    auto out = Writer {};
    out.u32(*marker);
    out.blob(grown.view());
    out.bytes(*rest);
    return frameOf(tag, out.view());
}
} // namespace

TEST_CASE("a negotiating ClientHello round-trips with ranges and capabilities",
          "[vthost][proto][negotiation]")
{
    auto const hello =
        ClientHello { .versions = { 4, 6 }, .capabilities = { .idents = { 1, 4711 } }, .token = "t" };
    CHECK(roundTrip(DecodedPdu { hello }) == DecodedPdu { hello });
}

TEST_CASE("a negotiating ServerHello round-trips with ranges and capabilities",
          "[vthost][proto][negotiation]")
{
    auto const hello = ServerHello { .agreedVersion = 4,
                                     .versions = { 4, 6 },
                                     .capabilities = { .idents = { 1, 4711 } },
                                     .machineId = "3deb5353d3ba43d08201c136a47ead7b",
                                     .hostname = "zeta" };
    CHECK(roundTrip(DecodedPdu { hello }) == DecodedPdu { hello });
}

TEST_CASE("every pre-negotiation daemon reads our ClientHello far enough to refuse it",
          "[vthost][proto][negotiation]")
{
    // Each reads a hello whole before it checks the version, 0.7.0 refusing a byte too many: only a hello
    // it reads gets its version back, and with it the client's "daemon speaks codec vN".
    auto const body = bodyOf(ClientHello {});
    CHECK(vthost::testing::shippedClientHelloVersion(body) == NegotiatingHelloMarker); // 0.7.0
    CHECK(vthost::testing::codec2ClientHelloVersion(body) == NegotiatingHelloMarker);
    CHECK(vthost::testing::codec3ClientHelloVersion(body) == NegotiatingHelloMarker);

    // Session settings and capabilities change nothing about that.
    auto const full = bodyOf(ClientHello {
        .capabilities = CapabilitySet::of({ Capability::ExtensionPermissions }),
        .token = "s3cr3t",
        .sessionSettings = WireSessionSettings { .historyLineCount = 5000, .sixelScrolling = 0 } });
    CHECK(vthost::testing::shippedClientHelloVersion(full) == NegotiatingHelloMarker);
    CHECK(vthost::testing::codec2ClientHelloVersion(full) == NegotiatingHelloMarker);
    CHECK(vthost::testing::codec3ClientHelloVersion(full) == NegotiatingHelloMarker);
}

TEST_CASE("a pre-negotiation client reads a VersionAlone ServerHello", "[vthost][proto][negotiation]")
{
    auto const body = bodyOf(ServerHello { .layout = HelloLayout::VersionAlone });
    CHECK(vthost::testing::shippedServerHelloVersion(body) == NegotiatingHelloMarker);
    CHECK(vthost::testing::codec2ServerHelloVersion(body) == NegotiatingHelloMarker);
    CHECK(vthost::testing::codec3ServerHelloVersion(body) == NegotiatingHelloMarker);
}

TEST_CASE("a VersionAlone ServerHello is its version and nothing else", "[vthost][proto][negotiation]")
{
    // Whatever else the struct holds stays off the wire: a refusal names no machine.
    auto const body =
        bodyOf(ServerHello { .layout = HelloLayout::VersionAlone,
                             .agreedVersion = 4,
                             .capabilities = CapabilitySet::of({ Capability::ExtensionPermissions }),
                             .machineId = "3deb5353d3ba43d08201c136a47ead7b",
                             .hostname = "zeta" });
    CHECK(body.size() == sizeof(uint32_t));
}

TEST_CASE("an old daemon's answer decodes as VersionAlone with its version", "[vthost][proto][negotiation]")
{
    auto const answerOf = [](std::vector<std::byte> const& frame) {
        auto const decoded = decodePdu(frame);
        REQUIRE(decoded);
        return std::get<ServerHello>(decoded->pdu);
    };
    SECTION("codec 3")
    {
        auto const hello = answerOf(vthost::testing::codec3ServerHello(1));
        CHECK(hello.layout == HelloLayout::VersionAlone);
        CHECK(hello.codecVersion == 3u);
        CHECK(hello.versions == VersionRange { 3, 3 });
        CHECK(hello.agreedVersion == 0u);
    }
    SECTION("codec 2")
    {
        auto const hello = answerOf(vthost::testing::codec2ServerHello(1));
        CHECK(hello.layout == HelloLayout::VersionAlone);
        CHECK(hello.codecVersion == 2u);
    }
    SECTION("contour 0.7.0")
    {
        auto const hello = answerOf(vthost::testing::shippedServerHello(1));
        CHECK(hello.layout == HelloLayout::VersionAlone);
        CHECK(hello.codecVersion == vthost::testing::ShippedProtocolVersion);
    }
    SECTION("a codec 3 daemon that names its machine")
    {
        // What follows another codec's version is that codec's own, consumed unread.
        auto body = Writer {};
        body.u32(3);
        body.string("3deb5353d3ba43d08201c136a47ead7b");
        body.string("zeta");
        auto const hello = answerOf(frameOf(PduType::ServerHello, body.view()));
        CHECK(hello.layout == HelloLayout::VersionAlone);
        CHECK(hello.codecVersion == 3u);
        CHECK(hello.machineId.empty());
    }
    SECTION("our marker with nothing after it")
    {
        auto body = Writer {};
        body.u32(NegotiatingHelloMarker);
        auto const hello = answerOf(frameOf(PduType::ServerHello, body.view()));
        CHECK(hello.layout == HelloLayout::VersionAlone);
        CHECK(hello.codecVersion == NegotiatingHelloMarker);
        CHECK(hello.agreedVersion == 0u);
    }
}

TEST_CASE("an old client's hello decodes with a single-version range", "[vthost][proto][negotiation]")
{
    auto const versionsOf = [](std::vector<std::byte> const& frame) {
        auto const decoded = decodePdu(frame);
        REQUIRE(decoded);
        auto const& hello = std::get<ClientHello>(decoded->pdu);
        // Nothing past the version is read: a token or settings of another codec are not ours to judge.
        CHECK(hello.token.empty());
        CHECK_FALSE(hello.sessionSettings.has_value());
        CHECK(hello.capabilities.idents.empty());
        return hello.versions;
    };
    CHECK(versionsOf(vthost::testing::codec3ClientHello(1)) == VersionRange { 3, 3 });
    CHECK(versionsOf(vthost::testing::codec2ClientHello(1)) == VersionRange { 2, 2 });
    CHECK(versionsOf(vthost::testing::shippedClientHello(1)) == VersionRange { 1, 1 });
}

TEST_CASE("a ClientHello leading with any value but the marker decodes as that version alone",
          "[vthost][proto][negotiation]")
{
    // Not only the smaller values of codecs 1 to 3: the marker is the one value that announces the
    // negotiating layout, and anything else is read no further than its version.
    auto body = Writer {};
    body.u32(NegotiatingHelloMarker + 1);
    body.string("a layout this version has never heard of");
    auto const decoded = decodePdu(frameOf(PduType::ClientHello, body.view()));
    REQUIRE(decoded.has_value());
    auto const& hello = std::get<ClientHello>(decoded->pdu);
    CHECK(hello.codecVersion == 5u);
    CHECK(hello.versions == VersionRange { 5, 5 });
    CHECK(hello.token.empty());
}

TEST_CASE("a hello with fields appended by a newer peer still decodes", "[vthost][proto][negotiation]")
{
    SECTION("ClientHello")
    {
        auto const hello =
            ClientHello { .versions = { 4, 9 },
                          .capabilities = { .idents = { 1, 77 } },
                          .token = "s3cr3t",
                          .sessionSettings = WireSessionSettings { .historyLineCount = 5000 } };
        auto const decoded = decodePdu(withAppendedField(PduType::ClientHello, hello));
        REQUIRE(decoded.has_value());
        CHECK(decoded->pdu == DecodedPdu { hello });
    }
    SECTION("ServerHello")
    {
        auto const hello = ServerHello { .agreedVersion = 4,
                                         .versions = { 4, 9 },
                                         .capabilities = { .idents = { 1, 77 } },
                                         .machineId = "3deb5353d3ba43d08201c136a47ead7b",
                                         .hostname = "zeta" };
        auto const decoded = decodePdu(withAppendedField(PduType::ServerHello, hello));
        REQUIRE(decoded.has_value());
        CHECK(decoded->pdu == DecodedPdu { hello });
    }
}

TEST_CASE("a negotiating hello is still held to its frame", "[vthost][proto][negotiation]")
{
    // What the frame says around the blob is checked; only bytes AFTER the fields it knows are skipped.
    auto fields = Writer {};
    fields.varint(4); // min
    fields.varint(4); // max
    fields.varint(0); // no capabilities
    fields.string("s3cr3t");
    fields.u8(0); // no session settings
    auto body = Writer {};
    auto tag = PduType::ClientHello;

    SECTION("a ClientHello whose closing byte is not zero")
    {
        body.u32(NegotiatingHelloMarker);
        body.blob(fields.view());
        body.u8(1);
    }
    SECTION("a ClientHello claiming more capabilities than its blob holds")
    {
        auto lying = Writer {};
        lying.varint(4);
        lying.varint(4);
        lying.varint(1000);
        body.u32(NegotiatingHelloMarker);
        body.blob(lying.view());
        body.u8(0);
    }
    SECTION("a ClientHello whose version does not fit 32 bits")
    {
        auto wide = Writer {};
        wide.varint(4);
        wide.varint(uint64_t { 1 } << 40);
        wide.varint(0);
        wide.string("");
        wide.u8(0);
        body.u32(NegotiatingHelloMarker);
        body.blob(wide.view());
        body.u8(0);
    }
    auto const decoded = decodePdu(frameOf(tag, body.view()));
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() != DecodeError::NeedMoreData);
}

TEST_CASE("bytes after a hello's known fields are a newer peer's", "[vthost][proto][negotiation]")
{
    SECTION("a ClientHello with bytes after its closing byte")
    {
        auto body = bodyOf(ClientHello { .token = "tok" });
        body.push_back(std::byte { 0x2A });
        CHECK(decodeAs<ClientHello>(frameOf(PduType::ClientHello, body)).token == "tok");
    }
    SECTION("a ServerHello with bytes after its blob")
    {
        auto const hello = ServerHello { .agreedVersion = 4, .versions = { 4, 4 }, .machineId = "m" };
        auto body = bodyOf(hello);
        body.push_back(std::byte { 0x2A });
        CHECK(decodeAs<ServerHello>(frameOf(PduType::ServerHello, body)) == hello);
    }
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
        body.u16(0);     // mouseProtocol
        body.u8(0);      // mouseTransport
        body.u8(0);      // mouseWheelMode
        body.u8(0);      // progressState
        body.u8(0);      // progressPercentage
        body.varint(0);  // contexts
        body.varint(0);  // contextChain
        body.svarint(0); // sessionEpoch
        body.svarint(0); // hostTime
        body.varint(0);  // commandBlockIdEpoch
        body.varint(0);  // commandBlocksFloor
        body.varint(0);  // commandBlocks
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

    SECTION("a body that ends before the interrupted command decodes with it empty")
    {
        // sessionStateWith() stops after commandBlocks, as an older sender's body does.
        auto const stream = sessionStateWith(80, 24);
        auto const decoded = decodePdu(stream.view());
        REQUIRE(decoded.has_value());
        auto const* const state = std::get_if<SessionState>(&decoded->pdu);
        REQUIRE(state != nullptr);
        CHECK(state->interruptedCommand.empty());
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

    // State rather than an event, but pushed per session the same way: a mirror applies it as it arrives.
    auto const echo = asSessionEvent(DecodedPdu { SessionInputEcho { .session = 6, .echo = 3 } });
    REQUIRE(echo.has_value());
    CHECK(std::holds_alternative<SessionInputEcho>(*echo));
    CHECK(sessionOf(*echo) == 6);
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

TEST_CASE("a row's size estimate tracks what the encoder actually writes", "[vthost][proto]")
{
    // The estimate exists so a splitter need not encode a row twice. That only holds while it
    // stays in the encoder's neighbourhood — a field added to encodeCell and forgotten here turns
    // a byte budget into a wish. Pinned as a FACTOR, not a number: the estimate is deliberately
    // approximate (it rounds, and it does not model varint widths), so an exact assertion would
    // fail on every harmless encoding change while missing the drift that matters.
    auto const rowOf = [](std::size_t cells) {
        auto line = WireLine {};
        line.stableId = 7;
        line.columns = static_cast<uint32_t>(cells);
        for (auto const column: std::views::iota(std::size_t { 0 }, cells))
        {
            auto cell = WireCell {};
            cell.codepoint = U'x';
            cell.hyperlink = static_cast<uint16_t>(column);
            line.cells.push_back(cell);
        }
        return line;
    };

    for (auto const cells: { std::size_t { 0 }, std::size_t { 1 }, std::size_t { 80 }, std::size_t { 400 } })
    {
        auto sink = Writer {};
        auto const line = rowOf(cells);
        auto delta = Delta {};
        delta.lines.push_back(line);
        encodePdu(sink, 1, DecodedPdu { delta });
        auto const actual = sink.view().size();
        auto const estimate = estimatedEncodedSize(line);
        // Never wildly under (a budget that under-counts overshoots the frame it was sizing) and
        // never wildly over (one that over-counts splits more than it needs to).
        CHECK(estimate * 4 >= actual);
        CHECK(actual * 4 >= estimate);
    }
}

TEST_CASE("partitionSnapshotRows covers every row exactly once, in order", "[vthost][proto]")
{
    auto lines = std::vector<WireLine> {};
    for (auto const row: std::views::iota(0, 20))
    {
        auto line = WireLine {};
        line.stableId = row;
        line.cells.resize(10);
        lines.push_back(line);
    }
    auto const perRow = estimatedEncodedSize(lines.front());

    SECTION("a budget holding three rows yields pieces of three")
    {
        auto const spans = partitionSnapshotRows(lines, perRow * 3);
        REQUIRE(spans.size() == 7); // 6 full pieces plus the remaining 2 rows
        CHECK(spans.front().count == 3);
        CHECK(spans.back().count == 2);

        // The spans tile the input: consecutive, gapless, no row named twice.
        auto next = std::size_t { 0 };
        for (auto const& span: spans)
        {
            CHECK(span.begin == next);
            next += span.count;
        }
        CHECK(next == lines.size());
    }

    SECTION("each piece is marked with its place in the run")
    {
        auto const spans = partitionSnapshotRows(lines, perRow * 3);
        REQUIRE(spans.size() > 2);
        CHECK(spans.front().part == SnapshotPart::First);
        CHECK(spans.back().part == SnapshotPart::Last);
        for (auto const& middle: spans | std::views::drop(1) | std::views::take(spans.size() - 2))
            CHECK(middle.part == SnapshotPart::Middle);
    }

    SECTION("a run of one is Whole, never a First whose Last never comes")
    {
        // The marker a receiver acts on: First tells it to clear and wait, and a lone piece that
        // said First would leave it waiting for a terminator that is never sent.
        CHECK(partitionSnapshotRows(lines, perRow * lines.size()).front().part == SnapshotPart::Whole);
        CHECK(partitionSnapshotRows({}, 1024).front().part == SnapshotPart::Whole);
    }

    SECTION("a row larger than the budget still travels, alone")
    {
        // A row is the smallest thing the wire can address, so the budget has to yield to it.
        // Refusing instead would make a wide enough grid unsendable at any budget.
        auto const spans = partitionSnapshotRows(lines, 1);
        REQUIRE(spans.size() == lines.size());
        CHECK(std::ranges::all_of(spans, [](auto const& span) { return span.count == 1; }));
    }

    SECTION("one budget-sized piece is not split")
    {
        auto const spans = partitionSnapshotRows(lines, perRow * lines.size());
        REQUIRE(spans.size() == 1);
        CHECK(spans.front().count == lines.size());
    }

    SECTION("no rows still yields one piece")
    {
        // A payload with no rows is still a payload: its sender has state to deliver, and a
        // caller that emits one PDU per span must emit exactly one.
        auto const spans = partitionSnapshotRows({}, 1024);
        REQUIRE(spans.size() == 1);
        CHECK(spans.front().count == 0);
    }
}

TEST_CASE("a row's command-block id and birth stamp survive the wire", "[vthost][proto][blocks]")
{
    // Unstamped, the smallest stamp, and the extremes: neither field may be narrowed or dropped, and
    // zero must stay zero (it means "no block" / "never stamped", not a value).
    auto const cases = std::vector<std::pair<uint32_t, uint32_t>> {
        { 0, 0 },
        { 1, 1 },
        { std::numeric_limits<uint32_t>::max(), 0x00FFFFFF },
    };
    for (auto const& [blockId, bornAt]: cases)
    {
        auto line = WireLine {};
        line.stableId = 4;
        line.columns = 80;
        line.blockId = blockId;
        line.bornAt = bornAt;
        auto delta = Delta {};
        delta.lines = { line };

        auto const decoded = roundTrip(DecodedPdu { delta });
        auto const* const back = std::get_if<Delta>(&decoded);
        REQUIRE(back != nullptr);
        REQUIRE(back->lines.size() == 1);
        CHECK(back->lines.front().blockId == blockId);
        CHECK(back->lines.front().bornAt == bornAt);
    }
}

TEST_CASE("a command-block record's optional fields travel only when present", "[vthost][proto][blocks]")
{
    SECTION("an absent field costs nothing on the wire")
    {
        // All four optionals zero either way; only the presence bits differ, and each present one
        // costs exactly its one-byte svarint(0). The evicted-head bit carries no field at all.
        auto absent = sampleBlock();
        absent.present = 0;
        absent.promptStartedAt = 0;
        absent.exitCode = 0;
        absent.commandStartedAt = 0;
        absent.duration = 0;
        auto present = absent;
        present.present = CommandBlockPresentMask;
        CHECK(encodedDeltaSize({ present }) == encodedDeltaSize({ absent }) + 4);
        // Built by assignment: Delta has members without default initializers, so a designated
        // initializer naming one field would trip -Wmissing-designated-field-initializers.
        auto delta = Delta {};
        delta.commandBlocks = { absent };
        CHECK(roundTrip(DecodedPdu { delta }) == DecodedPdu { delta });
    }
}

TEST_CASE("a record naming a presence bit this build does not assign is malformed", "[vthost][proto][blocks]")
{
    // The handshake admits only a peer of a version this build speaks, which assigns no such bit: one comes
    // from a broken or hostile peer, and whatever it wrote for that bit would put every later read out of
    // step. The encoder writes the mask as given, and fields only for the bits it knows.
    auto const decodeOf = [](DecodedPdu const& pdu) {
        auto stream = Writer {};
        encodePdu(stream, 1, pdu);
        return decodePdu(stream.view());
    };

    SECTION("a command-block record")
    {
        auto const bit = GENERATE(uint8_t { 1U << 5 }, uint8_t { 1U << 7 });
        auto unknown = sampleBlock();
        unknown.present = static_cast<uint8_t>(CommandBlockPresentMask | bit);
        auto delta = Delta {};
        delta.commandBlocks = { unknown, sampleBlock() };
        auto const decoded = decodeOf(DecodedPdu { delta });
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }

    SECTION("a context record")
    {
        auto context = WireContext {};
        context.id = 1;
        context.present = static_cast<uint16_t>(ContextPresentMask | (1U << 15));
        auto delta = Delta {};
        delta.contexts = { context };
        auto const decoded = decodeOf(DecodedPdu { delta });
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == DecodeError::MalformedPdu);
    }

    SECTION("every bit this build assigns still decodes")
    {
        auto context = WireContext {};
        context.id = 1;
        context.present = ContextPresentMask;
        auto delta = Delta {};
        delta.contexts = { context };
        delta.commandBlocks = { sampleBlock() };
        CHECK(roundTrip(DecodedPdu { delta }) == DecodedPdu { delta });
    }
}

TEST_CASE("Delta::hasChanges answers for command-block records and retirements", "[vthost][proto][blocks]")
{
    auto withBlock = Delta {};
    withBlock.commandBlocks = { WireCommandBlock {} };
    CHECK(withBlock.hasChanges());

    auto withRetired = Delta {};
    withRetired.retiredCommandBlocks = { 7 };
    CHECK(withRetired.hasChanges());
}

TEST_CASE("a command-block record's size estimate never under-counts", "[vthost][proto][blocks]")
{
    // The estimate budgets a delta's records (NativeSession::CommandBlockDeltaBytes) and caps a
    // snapshot's; an under-count would let a "bounded" frame exceed what it was sized against.
    auto const empty = encodedDeltaSize({});
    // Every scalar at its widest encoding as well, so the bound is held at its edge rather than with
    // the slack the sample's small values leave.
    auto widest = sampleBlock();
    widest.id = std::numeric_limits<uint32_t>::max();
    widest.outcomeStatus = std::numeric_limits<uint64_t>::max();
    widest.promptStartedAt = std::numeric_limits<int64_t>::min();
    widest.exitCode = std::numeric_limits<int64_t>::min();
    widest.commandStartedAt = std::numeric_limits<int64_t>::min();
    widest.duration = std::numeric_limits<int64_t>::min();
    for (auto const& base: { sampleBlock(), widest })
        for (auto const length: { std::size_t { 0 }, std::size_t { 40 }, std::size_t { 4096 } })
        {
            auto block = base;
            block.commandLine = std::string(length, 'x');
            block.endWorkingDirectory = std::string(length, 'y');
            auto const actual = encodedDeltaSize({ block }) - empty;
            auto const estimate = estimatedEncodedSize(block);
            CHECK(estimate >= actual);
            CHECK(actual * 2 >= estimate); // and not so generous that a budget fills with air
        }
}

TEST_CASE("ClientHello carries an attach intent and a profile name", "[vthost][pdu]")
{
    auto const hello =
        ClientHello { .attachIntent = { .kind = AttachIntentKind::Resume, .windows = { 1, 7 } },
                      .profileName = "work" };
    CHECK(roundTrip(DecodedPdu { hello }) == DecodedPdu { hello });
}

TEST_CASE("a ClientHello truncated before the appended fields decodes to AllWindows and no profile",
          "[vthost][pdu]")
{
    auto const hello = ClientHello { .token = "tok",
                                     .sessionSettings = WireSessionSettings { .historyLineCount = 5000 },
                                     .role = ConnectionRole::Control,
                                     .clientArea = WireClientArea { .columns = 132, .lines = 43 },
                                     .attachIntent = { .kind = AttachIntentKind::Launch, .windows = { 3 } },
                                     .profileName = "work" };
    auto expected = hello;
    expected.attachIntent = {};
    expected.profileName.clear();

    SECTION("after the client area")
    {
        // The intent (kind, count, one id) and the profile (length byte, four characters).
        CHECK(decodeAs<ClientHello>(encodeWithoutTrailingField(hello, 3 + 5)) == expected);
    }
    SECTION("after the role")
    {
        // The area (presence, two varints) goes as well.
        expected.clientArea.reset();
        CHECK(decodeAs<ClientHello>(encodeWithoutTrailingField(hello, 3 + 5 + 4)) == expected);
    }
    SECTION("after the session settings")
    {
        expected.clientArea.reset();
        expected.role = ConnectionRole::Attach;
        auto const withoutSettings = ClientHello { .token = "tok" };
        auto const settingsOnly =
            bodyOf(DecodedPdu { hello }).size() - bodyOf(DecodedPdu { withoutSettings }).size();
        CHECK(settingsOnly > 0);
        // Everything after the settings block: the role as well.
        auto const tail = std::size_t { 1 + 4 + 3 + 5 };
        CHECK(decodeAs<ClientHello>(encodeWithoutTrailingField(hello, tail)) == expected);
    }
    SECTION("the profile alone")
    {
        expected.attachIntent = hello.attachIntent;
        CHECK(decodeAs<ClientHello>(encodeWithoutTrailingField(hello, 5)) == expected);
    }
}

TEST_CASE("an attach intent kind no enumerator names decodes as AllWindows", "[vthost][pdu]")
{
    auto const hello =
        ClientHello { .attachIntent = { .kind = AttachIntentKind::Windows, .windows = { 4 } } };
    auto const future = encodeWithFieldsEdited(DecodedPdu { hello }, [](std::span<std::byte const> fields) {
        auto edited = std::vector<std::byte>(fields.begin(), fields.end());
        // The kind precedes the count, the one id, and the empty profile's length byte.
        edited.rbegin()[3] = std::byte { 0xEE };
        return edited;
    });
    auto const decoded = decodeAs<ClientHello>(future);
    CHECK(decoded.attachIntent.kind == AttachIntentKind::AllWindows);
    CHECK(decoded.attachIntent.windows == std::vector<uint64_t> { 4 });
}

TEST_CASE("ServerHello carries the followed windows; a refusal carries none", "[vthost][pdu]")
{
    auto const hello = ServerHello { .agreedVersion = 4, .followedWindows = { 1, 3 } };
    CHECK(std::get<ServerHello>(roundTrip(DecodedPdu { hello })).followedWindows
          == std::vector<uint64_t> { 1, 3 });
    CHECK(std::get<ServerHello>(roundTrip(DecodedPdu { ServerHello { .agreedVersion = 0 } }))
              .followedWindows.empty());
    // A daemon from before the field: the count, its ids, the instance and the restored counts after them are
    // missing.
    auto const legacy = encodeWithoutTrailingField(DecodedPdu { hello }, 6);
    CHECK(decodeAs<ServerHello>(legacy).followedWindows.empty());
}

TEST_CASE("ServerHello carries the daemon's instance; an older daemon's reads as unknown", "[vthost][pdu]")
{
    auto const hello =
        ServerHello { .agreedVersion = 4, .followedWindows = { 1 }, .daemonInstance = 0xC0FFEE'0000'0001 };
    CHECK(std::get<ServerHello>(roundTrip(DecodedPdu { hello })).daemonInstance == 0xC0FFEE'0000'0001);
    // A daemon from before the field: the followed windows end the blob.
    auto const legacy = encodeWithoutTrailingField(DecodedPdu { ServerHello { .agreedVersion = 4 } }, 3);
    auto const decoded = decodeAs<ServerHello>(legacy);
    CHECK(decoded.daemonInstance == 0);
    CHECK(decoded.agreedVersion == 4);
}

TEST_CASE("ServerHello carries what a restore brought back; an older daemon's reads as nothing",
          "[vthost][pdu]")
{
    auto const hello =
        ServerHello { .agreedVersion = 4, .daemonInstance = 7, .restoredWindows = 2, .restoredTabs = 7 };
    auto const decoded = std::get<ServerHello>(roundTrip(DecodedPdu { hello }));
    CHECK(decoded.restoredWindows == 2);
    CHECK(decoded.restoredTabs == 7);
    CHECK(decoded.daemonInstance == 7);
    // A daemon from before the fields: the two varints at the blob's end are missing.
    auto const legacy = decodeAs<ServerHello>(encodeWithoutTrailingField(DecodedPdu { hello }, 2));
    CHECK(legacy.restoredWindows == 0);
    CHECK(legacy.restoredTabs == 0);
    CHECK(legacy.daemonInstance == 7);
}

TEST_CASE("EditTab and Heartbeat round-trip", "[vthost][pdu]")
{
    auto const edit = EditTab { .session = 9, .kind = TabEditKind::SetColor, .color = 0x336699 };
    CHECK(roundTrip(DecodedPdu { edit }) == DecodedPdu { edit });
    auto const titled = EditTab { .session = 2, .kind = TabEditKind::SetTitle, .title = "build" };
    CHECK(roundTrip(DecodedPdu { titled }) == DecodedPdu { titled });
    CHECK(roundTrip(DecodedPdu { Heartbeat { .nonce = 42 } }) == DecodedPdu { Heartbeat { .nonce = 42 } });
}

TEST_CASE("an EditTab kind no enumerator names still decodes", "[vthost][pdu]")
{
    auto const decoded =
        std::get<EditTab>(roundTrip(DecodedPdu { EditTab { .kind = static_cast<TabEditKind>(0xEE) } }));
    CHECK(std::to_underlying(decoded.kind) == 0xEE);
}

TEST_CASE("the tab-edit and heartbeat tags are pinned", "[vthost][pdu]")
{
    STATIC_CHECK(std::to_underlying(PduType::EditTab) == 28);
    STATIC_CHECK(std::to_underlying(PduType::Heartbeat) == 29);
}

TEST_CASE("MergeTabIntoSplit round-trips and carries a side no enumerator names", "[vthost][pdu][merge]")
{
    auto const merge = MergeTabIntoSplit { .tabSession = 4, .targetSession = 9, .side = 3 };
    CHECK(roundTrip(DecodedPdu { merge }) == DecodedPdu { merge });
    auto const unknown = MergeTabIntoSplit { .tabSession = 1, .targetSession = 2, .side = 0xEE };
    CHECK(std::get<MergeTabIntoSplit>(roundTrip(DecodedPdu { unknown })).side == 0xEE);
    STATIC_CHECK(std::to_underlying(PduType::MergeTabIntoSplit) == 35);
}

TEST_CASE("spawning verbs carry a profile; one without it decodes to empty", "[vthost][pdu]")
{
    CHECK(std::get<CreateTab>(roundTrip(DecodedPdu { CreateTab { .session = 3, .profileName = "ssh" } }))
              .profileName
          == "ssh");
    CHECK(std::get<SplitPane>(roundTrip(DecodedPdu { SplitPane {
                                  .session = 3, .orientation = 2, .ratio = 5000, .profileName = "x" } }))
              .profileName
          == "x");
    CHECK(std::get<NewWindow>(roundTrip(DecodedPdu { NewWindow { .profileName = "y" } })).profileName == "y");

    // Bodies as a peer from before the field sends them.
    CHECK(decodeAs<NewWindow>(frameOf(PduType::NewWindow, {})).profileName.empty());
    auto createTab = Writer {};
    createTab.varint(3);
    CHECK(decodeAs<CreateTab>(frameOf(PduType::CreateTab, createTab.view())).profileName.empty());
    auto splitPane = Writer {};
    splitPane.varint(3);
    splitPane.u8(2);
    splitPane.u16(5000);
    CHECK(decodeAs<SplitPane>(frameOf(PduType::SplitPane, splitPane.view())).profileName.empty());
}

TEST_CASE("control PDUs round-trip", "[vthost][proto][control]")
{
    auto const request = ControlRequest {
        .requestId = 7, .method = "pane.split", .params = R"({"pane":"x"})", .context = "pane:p"
    };
    CHECK(roundTrip(DecodedPdu { request }) == DecodedPdu { request });
    auto const reply = ControlReply { .requestId = 7, .outcome = ControlOutcome::Error, .body = "{}" };
    CHECK(roundTrip(DecodedPdu { reply }) == DecodedPdu { reply });
    auto const event = ControlEvent { .subscription = 3, .event = R"({"type":"pane.bell"})" };
    CHECK(roundTrip(DecodedPdu { event }) == DecodedPdu { event });

    // A request that carries nothing optional still round-trips.
    CHECK(roundTrip(DecodedPdu { ControlRequest { .method = "daemon.info" } })
          == DecodedPdu { ControlRequest { .method = "daemon.info" } });

    // Cut short, a PDU is a decode error, not a crash.
    CHECK_FALSE(decodePdu(encodeWithoutTrailingField(DecodedPdu { reply }, 1)).has_value());
    CHECK_FALSE(decodePdu(encodeWithoutTrailingField(DecodedPdu { request }, 1)).has_value());
    CHECK_FALSE(decodePdu(encodeWithoutTrailingField(DecodedPdu { event }, 1)).has_value());
}

TEST_CASE("ProgramStatus PDU round-trips", "[vthost][proto][programstatus]")
{
    auto const pdu = ProgramStatus { .session = 4,
                                     .records = { WireProgramStatusRecord { .id = "",
                                                                            .state = 1,
                                                                            .kind = 0,
                                                                            .progress = 40,
                                                                            .app = "cargo",
                                                                            .title = "Build",
                                                                            .message = "" },
                                                  WireProgramStatusRecord { .id = "build/test",
                                                                            .state = 3,
                                                                            .kind = 2,
                                                                            .progress = 255,
                                                                            .app = "",
                                                                            .title = "",
                                                                            .message = "Continue?" } } };
    CHECK(roundTrip(DecodedPdu { pdu }) == DecodedPdu { pdu });
    // The empty set is how a viewer hears every record is gone.
    CHECK(roundTrip(DecodedPdu { ProgramStatus { .session = 4 } })
          == DecodedPdu { ProgramStatus { .session = 4 } });
    CHECK_FALSE(decodePdu(encodeWithoutTrailingField(DecodedPdu { pdu }, 1)).has_value());
}

TEST_CASE("the ProgramStatus tag and capability are pinned", "[vthost][proto][programstatus]")
{
    STATIC_CHECK(std::to_underlying(PduType::ProgramStatus) == 33);
    STATIC_CHECK(std::to_underlying(Capability::ProgramStatus) == 10);
    CHECK(std::ranges::any_of(CapabilityTable, [](CapabilityRow const& row) {
        return row.capability == Capability::ProgramStatus && row.name == "program-status";
    }));
    // The rows stay in value order.
    CHECK(
        std::ranges::is_sorted(CapabilityTable, {}, [](CapabilityRow const& row) { return row.capability; }));
}

TEST_CASE("an unknown ControlOutcome reads as Error, never as Ok", "[vthost][proto][control]")
{
    auto const future = encodeWithFieldsEdited(
        DecodedPdu { ControlReply { .requestId = 1, .outcome = ControlOutcome::Ok, .body = "{}" } },
        [](std::span<std::byte const> fields) {
            auto edited = std::vector<std::byte>(fields.begin(), fields.end());
            edited[1] = std::byte { 9 }; // the outcome, right after the one-byte request id
            return edited;
        });
    CHECK(decodeAs<ControlReply>(future).outcome == ControlOutcome::Error);
}

TEST_CASE("the control tags and capability are pinned", "[vthost][proto][control]")
{
    STATIC_CHECK(std::to_underlying(PduType::ControlRequest) == 30);
    STATIC_CHECK(std::to_underlying(PduType::ControlReply) == 31);
    STATIC_CHECK(std::to_underlying(PduType::ControlEvent) == 32);
    STATIC_CHECK(std::to_underlying(Capability::ControlApi) == 9);
    CHECK(std::ranges::any_of(CapabilityTable, [](CapabilityRow const& row) {
        return row.capability == Capability::ControlApi && row.name == "control-api";
    }));
}

TEST_CASE("the connection role byte: absent is Attach, 1 is Control, unknown is Control",
          "[vthost][proto][control]")
{
    // The unknown-role rule is phase 2's; it is pinned here under the ControlApi capability, whose
    // connections are the ones an unknown role must never be mistaken for a viewer on.
    auto const hello = ClientHello { .capabilities = CapabilitySet::of({ Capability::ControlApi }),
                                     .role = ConnectionRole::Control };
    CHECK(decodeAs<ClientHello>(encodeWithFieldsEdited(DecodedPdu { hello },
                                                       [](std::span<std::byte const> fields) {
                                                           return std::vector<std::byte>(fields.begin(),
                                                                                         fields.end());
                                                       }))
              .role
          == ConnectionRole::Control);
    auto const unknown = encodeWithFieldsEdited(DecodedPdu { hello }, [](std::span<std::byte const> fields) {
        auto edited = std::vector<std::byte>(fields.begin(), fields.end());
        edited.rbegin()[4] = std::byte { 9 };
        return edited;
    });
    CHECK(decodeAs<ClientHello>(unknown).role == ConnectionRole::Control);
    CHECK(decodeAs<ClientHello>(
              encodeWithoutTrailingField(DecodedPdu { ClientHello { .role = ConnectionRole::Control } }, 5))
              .role
          == ConnectionRole::Attach);
}

TEST_CASE("inputEchoFromWire reads every known state back and anything else as Unknown",
          "[vthost][proto][inputecho]")
{
    // Total: a newer peer's state must neither lock a pane nor turn secure input on here.
    for (auto const echo: { vtpty::InputEcho::Unknown,
                            vtpty::InputEcho::Echoed,
                            vtpty::InputEcho::Raw,
                            vtpty::InputEcho::Hidden })
        CHECK(vthost::inputEchoFromWire(vthost::inputEchoToWire(echo)) == echo);
    CHECK(vthost::inputEchoFromWire(4) == vtpty::InputEcho::Unknown);
    CHECK(vthost::inputEchoFromWire(255) == vtpty::InputEcho::Unknown);
}
