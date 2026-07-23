// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native protocol's typed PDU catalog.
///
/// Tags are EXPLICIT stable integers: retired tags leave gaps, and an unknown
/// ident decodes to Invalid{ident} — data, not an error — so old servers and
/// new clients keep talking. Adding a PDU is: a struct, a tag in PduType, one
/// row in each of the encode/decode tables in Pdu.cpp.
///
/// The structs are deliberately wire-level (raw u32 colors, raw flag words,
/// std-only types): the codec is shared by the server, the attach client, and
/// tests without dragging vtbackend into every consumer.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <vthost/proto/Wire.h>

namespace vthost::proto
{

/// The stable wire tag of each PDU (the wire carries it as a varint; the enum's
/// base type only bounds the catalog, not the protocol).
enum class PduType : uint8_t
{
    Invalid = 0,
    ClientHello = 1,
    ServerHello = 2,
    Input = 3,
    ResizeRequest = 4,
    FetchImage = 5,
    ImageData = 6,
    ImageGone = 7,
    SessionState = 8,
    Delta = 9,
    SessionEvent = 10,
    LayoutState = 11,
    CreateTab = 12,
    SplitPane = 13,
    ClosePane = 14,
    NewWindow = 15,
};

/// The kind of a SessionEvent — adding a transient session-app event is adding a
/// row here plus a case in the client's re-emit. `a`/`b` are the kind's payload.
enum class SessionEventKind : uint8_t
{
    Bell = 0,         ///< The bell rang (BEL). `a`/`b` unused.
    Notify = 1,       ///< A desktop notification: `a` = title, `b` = body.
    ClipboardSet = 2, ///< An OSC 52 clipboard write: `a` = selection, `b` = raw data.
};

/// An ident this decoder does not know (yet). Carried, never fatal.
struct Invalid
{
    uint64_t ident = 0;
    bool operator==(Invalid const&) const = default;
};

/// Client's first PDU: its codec revision and (for TCP) a preshared auth token.
/// Anything before it is a protocol error.
struct ClientHello
{
    uint32_t codecVersion = CodecVersion;
    /// Preshared token authenticating the client on the opt-in TCP transport.
    /// Empty over AF_UNIX, where the hardened socket's permissions are the gate.
    std::string token = {};
    bool operator==(ClientHello const&) const = default;
};

/// Server's answer to ClientHello.
struct ServerHello
{
    uint32_t codecVersion = CodecVersion;
    bool operator==(ServerHello const&) const = default;
};

/// Client keyboard/paste bytes for one session's PTY.
struct Input
{
    uint64_t session = 0;
    std::vector<std::byte> data;
    bool operator==(Input const&) const = default;
};

/// The client's size proposal; the server answers with authoritative state.
struct ResizeRequest
{
    uint32_t columns = 0;
    uint32_t lines = 0;
    bool operator==(ResizeRequest const&) const = default;
};

/// On-demand image pixel fetch by stable image id.
struct FetchImage
{
    uint64_t session = 0; ///< Image ids are per-session pools; scope the lookup.
    uint32_t imageId = 0;
    bool operator==(FetchImage const&) const = default;
};

/// The image bytes for a FetchImage answer. Clients cache by id.
struct ImageData
{
    uint32_t imageId = 0;
    uint8_t format = 0;  ///< vtbackend::ImageFormat's underlying value.
    uint32_t width = 0;  ///< Pixels.
    uint32_t height = 0; ///< Pixels.
    std::vector<std::byte> data;
    bool operator==(ImageData const&) const = default;
};

/// FetchImage answer when the refcount already dropped the image: the client
/// clears the cells referencing it.
struct ImageGone
{
    uint32_t imageId = 0;
    bool operator==(ImageGone const&) const = default;
};

/// Everything renditional a session carries OUTSIDE its grid cells — replayed
/// on attach and after every ResyncRequired.
struct SessionState
{
    uint64_t session = 0;
    uint32_t columns = 0;
    uint32_t lines = 0;
    uint8_t screenType = 0; ///< 0 = primary, 1 = alternate.
    int32_t cursorLine = 0;
    int32_t cursorColumn = 0;
    uint8_t cursorShape = 0;
    uint8_t cursorVisible = 1;
    std::string title;
    uint32_t defaultForeground = 0; ///< Raw RGBA.
    uint32_t defaultBackground = 0; ///< Raw RGBA.
    std::vector<uint32_t> palette;  ///< Indexed colors, raw RGBA.
    std::string cwd;                ///< The OSC 7 working-directory URL, if known.
    /// The status-display state (multi-page support): which status line is shown
    /// (DECSSDT) and which display the app writes to (DECSASD).
    uint8_t statusDisplayType = 0;   ///< StatusDisplayType: 0 none, 1 indicator, 2 host-writable.
    uint8_t activeStatusDisplay = 0; ///< ActiveStatusDisplay: 0 main, 1 status-line, 2 indicator.
    /// The Kitty keyboard protocol flags currently active (top of the app's flag
    /// stack), re-emitted so the client encodes keys the way the app negotiated.
    uint8_t kittyKeyboardFlags = 0;
    bool operator==(SessionState const&) const = default;
};

/// One cell's full renditional state on the wire.
struct WireCell
{
    char32_t codepoint = 0;
    std::vector<char32_t> clusterExtras; ///< Extra codepoints of the grapheme cluster.
    uint8_t width = 1;
    uint8_t scale = 1;            ///< OSC 66 block height.
    uint16_t textScaleExtras = 0; ///< Packed fraction/alignment (TextScale.h).
    uint16_t hyperlink = 0;       ///< Server-side HyperlinkId; URI via the side table.
    uint32_t foreground = 0;      ///< Raw vtbackend::Color bits.
    uint32_t background = 0;
    uint32_t underlineColor = 0;
    uint32_t flags = 0; ///< Raw CellFlags bits.
    bool operator==(WireCell const&) const = default;
};

/// One grid row, addressed by its stable id.
struct WireLine
{
    int64_t stableId = 0;
    uint16_t flags = 0; ///< Raw LineFlags bits.
    uint32_t columns = 0;
    /// Empty for a blank line (uniformly filled with the fill attributes below).
    std::vector<WireCell> cells;
    uint32_t fillForeground = 0;
    uint32_t fillBackground = 0;
    bool operator==(WireLine const&) const = default;
};

/// id → URI, sent once per connection on first reference (immune to the
/// server-side hyperlink LRU evicting the id later).
struct HyperlinkEntry
{
    uint16_t id = 0;
    std::string uri;
    bool operator==(HyperlinkEntry const&) const = default;
};

/// One image-covered cell: which row/column shows which part of which image.
struct ImageCellEntry
{
    int64_t stableId = 0; ///< Row.
    uint16_t column = 0;
    uint32_t imageId = 0;
    uint16_t offsetLine = 0; ///< Fragment offset within the image, in cells.
    uint16_t offsetColumn = 0;
    uint8_t layer = 0;
    bool operator==(ImageCellEntry const&) const = default;
};

/// A batch of changed rows plus the side tables they reference. `snapshot`
/// marks a full resync (attach or generation change) rather than an increment.
struct Delta
{
    uint64_t session = 0;
    uint64_t generation = 0;
    uint64_t seqno = 0;
    uint8_t snapshot = 0;
    /// The stable id of page row 0 at delta time — what lets the client map
    /// stable-id-addressed rows onto its viewport.
    int64_t stableViewportBase = 0;
    /// The oldest stable id the server still holds (its scrollback floor). Rows
    /// below it were evicted server-side — a `clear`/CSI 3 J jumps this up with
    /// no line changes, so the client MUST drop history below it or keep showing
    /// scrollback the real terminal already threw away.
    int64_t stableFloor = 0;
    int32_t cursorLine = 0;
    int32_t cursorColumn = 0;
    std::vector<WireLine> lines;
    std::vector<HyperlinkEntry> hyperlinks;
    std::vector<ImageCellEntry> imageCells;
    /// The DEC private modes (by DECSET number) currently SET on the hosted
    /// terminal, restricted to the mirrored-mode table — sent complete with
    /// every delta so clients replay input-relevant state (cursor keys,
    /// mouse, bracketed paste, focus, cursor visibility) by diffing.
    std::vector<uint32_t> setModes;
    /// Set (1) when the window title changed in this batch; `title` then holds
    /// the new OSC 0/2 title. Gated so an unchanged title costs one byte, not
    /// the whole string on every delta.
    uint8_t titleChanged = 0;
    std::string title;
    /// Set (1) when the cursor shape changed in this batch; `cursorShape` then
    /// holds the DECSCUSR Ps value (1 blink block … 6 steady bar) to re-emit.
    uint8_t cursorShapeChanged = 0;
    uint8_t cursorShape = 0;
    /// Set (1) when the working directory changed in this batch; `cwd` then holds
    /// the new OSC 7 URL to re-emit.
    uint8_t cwdChanged = 0;
    std::string cwd;
    /// Set (1) when the default fg/bg changed in this batch (OSC 10/11);
    /// `defaultForeground`/`defaultBackground` then hold the new 0xRRGGBB colors.
    uint8_t colorsChanged = 0;
    uint32_t defaultForeground = 0;
    uint32_t defaultBackground = 0;
    /// Set (1) when the status-display state changed in this batch (DECSSDT /
    /// DECSASD); the two bytes below then hold the new state to re-emit.
    uint8_t statusChanged = 0;
    uint8_t statusDisplayType = 0;
    uint8_t activeStatusDisplay = 0;
    /// Set (1) when the host-writable status line's content changed this batch;
    /// `statusLines` then holds its whole (tiny) grid, painted onto the mirror's
    /// status page. A separate page from the main grid — the multi-page carrier.
    uint8_t statusLinesChanged = 0;
    std::vector<WireLine> statusLines;
    /// Set (1) when the Kitty keyboard flags changed in this batch;
    /// `kittyKeyboardFlags` then holds the new flag set to re-emit (CSI = flags ; 1 u).
    uint8_t kittyKeyboardChanged = 0;
    uint8_t kittyKeyboardFlags = 0;
    bool operator==(Delta const&) const = default;
};

/// A transient session-app event carrying no screen state (bell, desktop
/// notification, OSC 52 clipboard write) — pushed unsolicited (serial 0). The
/// client re-emits it as the matching VT into its mirror terminal, so the
/// frontend's own bell/notify/clipboard handling (and permissions) apply.
struct SessionEvent
{
    uint64_t session = 0;
    uint8_t kind = 0; ///< A SessionEventKind value.
    std::string a;    ///< Notify title / clipboard selection; kind-specific.
    std::string b;    ///< Notify body / clipboard data; kind-specific.
    bool operator==(SessionEvent const&) const = default;
};

/// One node of a tab's split tree on the wire (pre-order): a leaf carries a
/// session and no children; a split carries a ratio and exactly two children.
struct WirePane
{
    uint64_t paneId = 0;
    uint8_t split = 0;                   ///< vtworkspace::SplitState: 0 leaf, 1 horizontal, 2 vertical.
    uint64_t session = 0;                ///< Leaf only: the SessionId.
    uint16_t ratio = 5000;               ///< Split only: first child's share × 10000 (0.05..0.95).
    std::vector<WirePane> children = {}; ///< 0 for a leaf, 2 for a split.
    bool operator==(WirePane const&) const = default;
};

/// One tab's layout: its id, active/zoomed pane, optional rename + color, and
/// its split tree — everything a client needs to reproduce the tab.
struct WireTab
{
    uint64_t tabId = 0;
    uint64_t activePane = 0;
    uint64_t zoomedPane = 0; ///< 0 = not zoomed.
    std::string title = {};  ///< The runtime-title override; empty if none.
    uint8_t hasColor = 0;
    uint32_t color = 0; ///< 0xRRGGBB tab color when hasColor.
    WirePane root;
    bool operator==(WireTab const&) const = default;
};

/// The window's whole tab/pane layout — replayed on attach and re-pushed on
/// every model change, so a client reproduces the daemon's tabs and split trees
/// instead of flattening to one tab per session.
struct LayoutState
{
    uint64_t window = 0;
    uint32_t activeTab = 0; ///< The active tab's INDEX within `tabs`.
    std::vector<WireTab> tabs;
    bool operator==(LayoutState const&) const = default;
};

/// Client→server: create a new tab (with a fresh session) in the daemon window.
/// The resulting model change re-pushes LayoutState to every attached client.
struct CreateTab
{
    bool operator==(CreateTab const&) const = default;
};

/// Client→server: create a new daemon window (with a first tab + session). The
/// daemon then pushes that window's LayoutState; the client opens a GUI window for
/// it (B4).
struct NewWindow
{
    bool operator==(NewWindow const&) const = default;
};

/// Client→server: split the pane hosting @p session (the daemon activates it
/// first), backing the new leaf with a fresh session.
struct SplitPane
{
    uint64_t session = 0;    ///< The pane to split, by the session it hosts.
    uint8_t orientation = 1; ///< vtworkspace::SplitState: 1 horizontal, 2 vertical.
    uint16_t ratio = 5000;   ///< First child's share × 10000.
    bool operator==(SplitPane const&) const = default;
};

/// Client→server: close the pane hosting @p session (and destroy that session).
struct ClosePane
{
    uint64_t session = 0;
    bool operator==(ClosePane const&) const = default;
};

using DecodedPdu = std::variant<Invalid,
                                ClientHello,
                                ServerHello,
                                Input,
                                ResizeRequest,
                                FetchImage,
                                ImageData,
                                ImageGone,
                                SessionState,
                                Delta,
                                SessionEvent,
                                LayoutState,
                                CreateTab,
                                SplitPane,
                                ClosePane,
                                NewWindow>;

/// Encodes @p pdu (body + frame) into @p sink.
/// @param sink The output writer.
/// @param serial Request correlation; 0 = unsolicited push.
/// @param pdu Any catalog PDU.
void encodePdu(Writer& sink, uint64_t serial, DecodedPdu const& pdu);

/// The result of decoding one frame's worth of input.
struct DecodedFrame
{
    uint64_t serial = 0;
    DecodedPdu pdu;
    std::size_t consumed = 0; ///< Input bytes to drop from the stream.
};

/// Decodes the next PDU from @p data; NeedMoreData while the frame is incomplete.
[[nodiscard]] std::expected<DecodedFrame, DecodeError> decodePdu(std::span<std::byte const> data);

} // namespace vthost::proto
