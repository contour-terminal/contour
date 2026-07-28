// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native protocol's typed PDU catalog.
///
/// Tags are EXPLICIT integers, and an unknown ident decodes to Invalid{ident} — data, not an error —
/// so a newer peer's PDU is carried rather than fatal. Adding a PDU is: a struct, a tag in PduType,
/// one row in each of the encode/decode tables in Pdu.cpp.
///
/// While the protocol is unreleased the catalog is kept CONTIGUOUS and renumbered freely: a retired
/// tag leaves no gap, because a gap only earns its keep against a deployed peer and there is none
/// (@see CodecVersion, which is pinned for the same reason). Once the protocol ships, tags become
/// stable and a retirement must leave its number behind.
///
/// The structs are deliberately wire-level (raw u32 colors, raw flag words,
/// std-only types): the codec is shared by the server, the attach client, and
/// tests without dragging vtbackend into every consumer.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <vthost/proto/Wire.h>

namespace vthost::proto
{

/// The wire tag of each PDU (the wire carries it as a varint; the enum's base type only bounds the
/// catalog, not the protocol). Contiguous, and not yet stable — @see the file comment.
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
    SessionBell = 10,
    SessionNotify = 11,
    SessionClipboard = 12,
    LayoutState = 13,
    CreateTab = 14,
    SplitPane = 15,
    ClosePane = 16,
    NewWindow = 17,
    ResizePane = 18,
    ResizeSplit = 19,
};

/// The wire encoding of a split's first-child share: a fraction in (0, 1) carried as an integer
/// per ten-thousand, so a ratio survives the wire without a float format.
///
/// Both directions live here because the encoder and every decoder must agree bit-for-bit: a client
/// decides whether to report a dragged divider by comparing its own ratio against the one the server
/// last sent, and that comparison is only sound when both were quantized the same way.
/// @param ratio The first child's space share.
/// @return The wire value.
[[nodiscard]] inline uint16_t toWireRatio(double ratio) noexcept
{
    // Guard the narrowing, not the value: callers pass an already-clamped vtworkspace::Pane ratio,
    // but converting a negative, huge or NaN double to uint16_t is undefined — and both saturating
    // answers map back to an even split through fromWireRatio, which is what a nonsense ratio means.
    if (!(ratio > 0.0))
        return 0;
    if (ratio >= 1.0)
        return 10000;
    return static_cast<uint16_t>(std::lround(ratio * 10000.0));
}

/// The inverse of @ref toWireRatio, falling back to an even split for a degenerate value so a
/// mirrored split never collapses to a zero-width pane.
/// @param wire The wire value.
/// @return The first child's space share, always in (0, 1).
[[nodiscard]] constexpr double fromWireRatio(uint16_t wire) noexcept
{
    auto const share = static_cast<double>(wire) / 10000.0;
    return (share > 0.0 && share < 1.0) ? share : 0.5;
}

/// An ident this decoder does not know (yet). Carried, never fatal.
struct Invalid
{
    uint64_t ident = 0;
    bool operator==(Invalid const&) const = default;
};

/// One frozen DEC private mode: a mode the hosted application may not change.
struct WireFrozenMode
{
    uint32_t mode = 0;    ///< The DEC private mode number (the `Ps` in `CSI ? Ps h`).
    uint8_t frozenAs = 0; ///< The value it is pinned to: 1 set, 0 reset.
    bool operator==(WireFrozenMode const&) const = default;
};

/// A client's preferred EMULATION settings for the sessions IT creates — never for sessions that
/// already exist, which keeps two clients on different profiles from fighting over one session.
///
/// Everything here is advisory and untrusted: the server applies it over its OWN settings and
/// validates every field (@see vthost::fromWireSessionSettings), so an unset or nonsensical value
/// falls back to the daemon's rather than to a wire default.
///
/// Two settings are deliberately ABSENT:
///
///  - The page size. The client area is negotiated by ResizeRequest and projected onto the pane
///    tree, and the host overrides each session's page size from that projection regardless.
///  - `goodImageProtocol`. Daemon mode always enables it (@see vthost::hostedSessionSettings),
///    because the knob is on its way to non-configurable. Carrying a setting that is about to
///    disappear would buy one release of fidelity and then cost a wire break to remove.
struct WireSessionSettings
{
    /// The scrollback limit: >= 0 a finite line count, -1 unlimited — the configuration's own
    /// spelling of `history.limit`, rather than a second encoding to keep in step with it.
    int64_t historyLineCount = 0;
    uint8_t terminalId = 0;         ///< vtbackend::VTType's numeric encoding (sparse; validate it).
    uint8_t graphemeClustering = 1; ///< Whether DEC mode 2027 starts out set.
    uint8_t allowReflowOnResize = 1;
    uint32_t maxImageRegisterCount = 256;
    // Both explicitly default-initialized, so naming a subset of this struct with designated
    // initializers stays clean under -Wmissing-designated-field-initializers.
    std::string wordDelimiters = {}; ///< UTF-8; the client's u32string re-encoded.
    std::vector<WireFrozenMode> frozenModes = {};
    bool operator==(WireSessionSettings const&) const = default;
};

/// Client's first PDU: its codec revision, (for TCP) a preshared auth token, and optionally the
/// emulation settings it wants the sessions it creates to have. Anything before it is a protocol
/// error.
struct ClientHello
{
    uint32_t codecVersion = CodecVersion;
    /// Preshared token authenticating the client on the opt-in TCP transport.
    /// Empty over AF_UNIX, where the hardened socket's permissions are the gate.
    std::string token = {};
    /// The client's preference for sessions it creates; absent means "whatever the daemon hosts
    /// with", which is what a client with no configuration of its own wants.
    std::optional<WireSessionSettings> sessionSettings = std::nullopt;
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

/// The client's CLIENT-AREA size proposal — the whole content area it renders the layout into, not
/// any one pane. The server projects the pane tree into it (@see SessionHost::applyClientSize) and
/// answers with authoritative state.
struct ResizeRequest
{
    uint32_t columns = 0;
    uint32_t lines = 0;
    bool operator==(ResizeRequest const&) const = default;
};

/// Client→server: the grid ONE pane is actually rendered at. The server's own projection of the
/// client area (ResizeRequest) is a ratio-driven estimate; a client that lays panes out by pixels —
/// or whose user dragged a divider — reports each pane here so its PTY matches what is on screen.
/// Refines the projection until the next structural change re-projects (@see
/// SessionHost::applyPaneSize).
struct ResizePane
{
    uint64_t session = 0;
    uint32_t columns = 0;
    uint32_t lines = 0;
    bool operator==(ResizePane const&) const = default;
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

/// The RESOLVED mouse-reporting state — what the input generator will actually encode.
///
/// This is NOT derivable from a set of DEC mode numbers, which is why it rides its own fields
/// rather than the mirrored-mode table: nine mouse modes write these three values, and several
/// write the SAME one (1005/1006/1015/1016 all pick the transport; 9/1000/1002/1003 all pick the
/// protocol), so a set of bits cannot say which spelling won. Same reasoning that gives
/// kittyKeyboardFlags and modifyOtherKeys their own fields.
///
/// One struct rather than three fields repeated per PDU, because it is one setting: an application
/// changes protocol, encoding and wheel handling together, and the three are meaningless apart.
/// @see vthost/MouseWire.h for the encoding each field uses.
struct MouseState
{
    uint16_t protocol = 0; ///< Active protocol as its DECSET number (9/1000/1001/1002/1003); 0 = off.
    uint8_t transport = 0; ///< vtbackend::MouseTransport.
    uint8_t wheelMode = 0; ///< vtbackend::InputGenerator::MouseWheelMode.
    bool operator==(MouseState const&) const = default;
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
    /// xterm's modifyOtherKeys level (XTMODKEYS resource 4), the OTHER way an app asks
    /// for modified keys as escape sequences. Re-emitted as `CSI > 4 ; level m`. The
    /// Kitty flags above win whenever they are non-zero, matching InputGenerator.
    uint8_t modifyOtherKeys = 0;
    MouseState mouse {}; ///< The resolved mouse-reporting state; stated outright on every snapshot.
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
    /// Raw `vtbackend::LineFlags` bits — whether the row may reflow and whether it CONTINUES the
    /// row above it, plus the shell's semantic marks and the DECDWL/DECDHL renditions.
    ///
    /// The bit values are frozen because they travel verbatim; `vthost/GridWire.cpp` pins them, so
    /// renumbering one is a build break rather than a silent mis-decode against an older peer.
    uint16_t flags = 0;
    /// Where the prompt stopped and where the previous command's output stopped, as LOGICAL columns
    /// (they may exceed @ref columns on a row whose logical line wrapped). Both are meaningful only
    /// with the matching flag — `PromptEnd`, `CommandEnd` — set in @ref flags, and both belong to
    /// the logical line's HEAD.
    ///
    /// They ride the wire rather than being reconstructed from OSC 133 because there is nothing to
    /// reconstruct them from: the terminal takes them from wherever the cursor stood when the shell
    /// spoke, and once the user types, the prompt and the input are the same run of cells.
    int32_t promptEndOffset = 0;
    int32_t commandEndOffset = 0;
    uint32_t columns = 0;

    /// The row's cells, from column 0, and possibly FEWER than @ref columns of them.
    ///
    /// A trailing run of cells indistinguishable from this row's fill is not sent: the
    /// receiver reconstructs it, and a uniformly-filled row therefore sends none at all.
    /// Omitting them is what keeps a snapshot of a deep scrollback affordable — padding
    /// every history row out to the grid's width costs ~24 bytes per unused column.
    ///
    /// **Both ends must agree on what an absent column means**: a cell wearing this row's WHOLE
    /// fill rendition — the four fields below. Concretely, every receiver must paint the fill
    /// across the row before writing cells over it; a path that renders `cells` alone truncates
    /// every filled region at the last column that happened to be sent.
    std::vector<WireCell> cells;

    /// The rendition an absent column wears — the pen the row was ERASED with, which is the
    /// cursor's pen at erase time and not necessarily the default one.
    ///
    /// All four parts of it travel, deliberately. While the fill could name only the two colours,
    /// a row cleared under `\e[7m\e[2J` or `\e[4:3m\e[K` was not describable by it: a blank row had
    /// to be materialized column by column, and a non-blank one found no trailing run to omit —
    /// so the one case the omission exists for (a full-screen clear over a deep scrollback) was
    /// exactly the case that paid ~24 bytes per column of every row instead.
    uint32_t fillForeground = 0;
    uint32_t fillBackground = 0;
    uint32_t fillUnderlineColor = 0;
    uint32_t fillFlags = 0; ///< Raw CellFlags bits, frozen by vthost/GridWire.cpp like WireCell's.

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
///
/// The last three fields describe the whole PLACEMENT rather than this cell, and are repeated per
/// cell for the same reason `layer` always was: a receiver meets the cells first and has to be able
/// to rebuild the placement from any one of them. They are what turn source pixels into the cells
/// they cover, so a receiver that rasterizes without them gets a differently cropped image.
struct ImageCellEntry
{
    int64_t stableId = 0; ///< Row.
    uint16_t column = 0;
    uint32_t imageId = 0;
    uint16_t offsetLine = 0; ///< Fragment offset within the image, in cells.
    uint16_t offsetColumn = 0;
    uint8_t layer = 0;     ///< vtbackend::ImageLayer's underlying value.
    uint8_t alignment = 0; ///< vtbackend::ImageAlignment's underlying value.
    uint8_t resize = 0;    ///< vtbackend::ImageResize's underlying value.
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
    /// The ANSI (non-private) modes currently SET, restricted to the mirrored table — LNM, whose
    /// input half decides whether Return sends CR or CR LF. Its OWN field, not folded into
    /// `setModes`: ANSI and DEC modes are distinct number spaces, so one list could not say which
    /// mode 20 it meant. @see vthost/MirroredModes.h.
    std::vector<uint32_t> setAnsiModes;
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
    /// Set (1) when xterm's modifyOtherKeys level changed in this batch;
    /// `modifyOtherKeys` then holds the new level to re-emit (CSI > 4 ; level m).
    uint8_t modifyOtherKeysChanged = 0;
    uint8_t modifyOtherKeys = 0;
    /// Set (1) when the resolved mouse-reporting state changed in this batch; `mouse` then holds
    /// it. @see MouseState for why the mode set above cannot carry this.
    uint8_t mouseChanged = 0;
    MouseState mouse {};

    /// Whether this delta says anything about the SESSION at all — a snapshot, changed rows, or any
    /// of the gated fields above.
    ///
    /// Lives beside the fields on purpose. The sender drops a delta that changes nothing, and while
    /// that decision was spelled out at the send site, every `…Changed` gate added here had to be
    /// remembered over there as well: forget it and the field is populated, the delta is discarded
    /// whenever nothing else moved, and the client never learns of the change — with no error
    /// anywhere. Adding a gated field above means adding it to this disjunction, and nothing else.
    ///
    /// Excludes the two peer-relative facts (the mode set and the cursor position), which are not
    /// properties of the delta but of what THIS peer was last told; the sender adds them.
    /// @return True when the delta is worth sending.
    [[nodiscard]] bool hasChanges() const noexcept
    {
        return snapshot != 0 || !lines.empty() || titleChanged != 0 || cursorShapeChanged != 0
               || cwdChanged != 0 || colorsChanged != 0 || statusChanged != 0 || statusLinesChanged != 0
               || kittyKeyboardChanged != 0 || modifyOtherKeysChanged != 0 || mouseChanged != 0;
    }

    bool operator==(Delta const&) const = default;
};

// --- transient session-app events ------------------------------------------
//
// Events carrying no screen state, pushed unsolicited (serial 0). The client re-emits each as the
// matching VT into its mirror terminal, so the frontend's own bell / notify / clipboard handling —
// and its permissions — apply.
//
// One PDU per event rather than one PDU with a kind discriminator: the discriminated form had to
// carry the union of every payload as two anonymous strings, so nothing but a comment said which
// field meant what, and every consumer re-derived it from the kind byte. Adding an event is a struct,
// a tag, a codec row, and one more alternative in SessionEventPdu — all of which the compiler
// demands, where a new `kind` value silently reached consumers that ignored it.

/// The bell rang (BEL) in a session.
struct SessionBell
{
    uint64_t session = 0;
    bool operator==(SessionBell const&) const = default;
};

/// A desktop notification a session raised (OSC 9 / OSC 777 / OSC 99).
struct SessionNotify
{
    uint64_t session = 0;
    std::string title = {};
    std::string body = {};
    bool operator==(SessionNotify const&) const = default;
};

/// A session's clipboard write (OSC 52). Forwarded unconditionally — the CLIENT applies its own
/// clipboard-write permission, since it is the one with a clipboard.
struct SessionClipboard
{
    uint64_t session = 0;
    std::string selection = {}; ///< The OSC 52 selection ("c" = CLIPBOARD, "p" = PRIMARY, …).
    std::string data = {};      ///< The raw, decoded text.
    bool operator==(SessionClipboard const&) const = default;
};

/// Any transient session event, as a type-safe alternative set: what a consumer handling "a session
/// did something transient" dispatches over, without enumerating the catalog's tags itself.
using SessionEventPdu = std::variant<SessionBell, SessionNotify, SessionClipboard>;

/// One node of a tab's split tree on the wire (pre-order): a leaf carries a
/// session and no children; a split carries a ratio and exactly two children.
struct WirePane
{
    uint64_t paneId = 0;
    uint8_t split = 0;                   ///< vtworkspace::SplitState: 0 leaf, 1 horizontal, 2 vertical.
    uint64_t session = 0;                ///< Leaf only: the SessionId.
    uint16_t ratio = 5000;               ///< Split only: first child's share, @see toWireRatio.
    std::vector<WirePane> children = {}; ///< 0 for a leaf, 2 for a split.
    bool operator==(WirePane const&) const = default;

    /// Whether this node is a split rather than a leaf — the ONE place that answers it, so every
    /// consumer treats a malformed node the same way.
    ///
    /// The child count is part of the question, not a redundant check: a decoded PDU cannot carry a
    /// split without its two children (decodePane cross-checks them), but a hand-built WirePane can,
    /// and a consumer that trusted `split` alone would index children[0] out of bounds. Such a node
    /// collapses to a leaf everywhere.
    [[nodiscard]] bool isSplit() const noexcept { return split != 0 && children.size() >= 2; }
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

/// Client→server: create a new tab (with a fresh session) in a daemon window.
/// The resulting model change re-pushes LayoutState to every attached client.
struct CreateTab
{
    /// Which WINDOW to create the tab in, named — like every other client→server verb — by a
    /// session it hosts; 0 means "the daemon's own first window".
    ///
    /// The daemon really does host several windows (@see NewWindow), so a request naming none put
    /// every client's "+" into the first one: click it in a second window and the tab appeared in
    /// the first, while the window clicked in gained nothing. Named by session rather than by
    /// window id for the reason SplitPane is: window ids are minted per model and the two ends
    /// have their own.
    uint64_t session = 0;
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

/// Client→server: the user moved the divider of the split separating the panes that host
/// @p firstSession and @p secondSession.
///
/// A ratio has to live in the SERVER's model or it does not survive: a re-attaching client rebuilds
/// every tab from LayoutState, so a divider the daemon never heard about comes back at the ratio the
/// split was CREATED with. ResizePane is not the same thing — it carries the cells one pane is drawn
/// at, which the next re-projection recomputes from the ratio and discards.
///
/// The split is named by two of its leaves rather than by a pane id, matching every other
/// client→server verb (SplitPane/ClosePane/ResizePane are all session-keyed): the client sends any
/// leaf from each side of the divider it moved, and the server resolves their lowest common ancestor
/// (@see vtworkspace::Pane::lowestCommonAncestor). A client mirrors the server's tree shape, so that
/// ancestor is the same split node — and which leaf each side contributes does not matter.
struct ResizeSplit
{
    uint64_t firstSession = 0;  ///< A leaf session under the split's FIRST child.
    uint64_t secondSession = 0; ///< A leaf session under the split's SECOND child.
    uint16_t ratio = 5000;      ///< First child's share, as @ref toWireRatio encodes it.
    bool operator==(ResizeSplit const&) const = default;
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
                                SessionBell,
                                SessionNotify,
                                SessionClipboard,
                                LayoutState,
                                CreateTab,
                                SplitPane,
                                ClosePane,
                                NewWindow,
                                ResizePane,
                                ResizeSplit>;

/// Encodes @p pdu (body + frame) into @p sink.
/// @param sink The output writer.
/// @param serial Request correlation; 0 = unsolicited push.
/// @param pdu Any catalog PDU.
void encodePdu(Writer& sink, uint64_t serial, DecodedPdu const& pdu);

/// The catalog tag @p pdu carries, for diagnostics and dispatch.
///
/// Delegates to the same tag table `encodePdu` uses, so the two can never disagree.
/// @param pdu Any catalog PDU.
/// @return Its tag; PduType::Invalid for the Invalid alternative, whose off-catalog
///         ident is data on the alternative itself rather than a tag.
[[nodiscard]] PduType typeOf(DecodedPdu const& pdu) noexcept;

/// The transient session event @p pdu holds, if it is one.
///
/// The projection from the whole catalog onto @ref SessionEventPdu, so a consumer that cares about
/// transient events asks that question once instead of testing each event tag itself — and gains a
/// fourth event without changing.
/// @param pdu Any catalog PDU.
/// @return The event, or std::nullopt when @p pdu is not a session event.
[[nodiscard]] std::optional<SessionEventPdu> asSessionEvent(DecodedPdu const& pdu);

/// The session a transient event belongs to.
/// @param event Any session event.
/// @return Its session id.
[[nodiscard]] uint64_t sessionOf(SessionEventPdu const& event) noexcept;

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
