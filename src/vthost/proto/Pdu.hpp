// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native protocol's typed PDU catalog.
///
/// Tags are EXPLICIT integers, and an unknown ident decodes to Invalid{ident} — data, not an error —
/// so a newer peer's PDU is carried rather than fatal. Adding a PDU is: a struct, a tag in PduType,
/// one row in each of the encode/decode tables in Pdu.cpp.
///
/// The catalog is kept CONTIGUOUS. A new PDU is gated by a Capability; renumbering or retiring a tag
/// changes existing PDUs' meaning, so it takes a protocol version step, and the handshake keeps a peer
/// that speaks no version in common from sending a single other PDU. The two hellos are the exception:
/// their tags are how two versions meet at all, so every version keeps 0.7.0's.
/// @see SupportedVersions
///
/// A PDU body grows only by appending fields at its end. A decoder ignores the bytes left after the fields
/// it knows, and gives a field it finds missing at the end its default, so a peer older or newer by one
/// appended field still reads the PDU. A field that is itself a growing record travels as a blob, so its
/// own appended fields stay inside it and the fields after it keep their place.
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
#include <utility>
#include <variant>
#include <vector>

#include <vthost/proto/Negotiation.hpp>
#include <vthost/proto/Wire.hpp>

namespace vthost::proto
{

/// The wire tag of each PDU (the wire carries it as a varint; the enum's base type only bounds the
/// catalog, not the protocol). Contiguous, and stable only for the two hellos, as the file comment says.
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
    SessionSettingsChanged = 20,
    PerformOperation = 21,
    PermissionRequest = 22,
    PermissionReply = 23,
    Unsupported = 24,
    DaemonStatusRequest = 25,
    DaemonStatusReply = 26,
    StopDaemon = 27,
    EditTab = 28,
    Heartbeat = 29,
    ControlRequest = 30,
    ControlReply = 31,
    ControlEvent = 32,
    ProgramStatus = 33,
    SessionInputEcho = 34,
};

static_assert(std::to_underlying(PduType::ClientHello) == 1 && std::to_underlying(PduType::ServerHello) == 2,
              "the hellos keep the tags contour 0.7.0 gave them: they are how two revisions meet");

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
    /// Whether a sixel image scrolls the page at the bottom margin, as the session starts and as RIS
    /// returns it: 1 yes (DECSDM reset), 0 no. The configuration's `images.sixel_scrolling`.
    uint8_t sixelScrolling = 1;
    // Both explicitly default-initialized, so naming a subset of this struct with designated
    // initializers stays clean under -Wmissing-designated-field-initializers.
    std::string wordDelimiters = {}; ///< UTF-8; the client's u32string re-encoded.
    std::vector<WireFrozenMode> frozenModes = {};
    bool operator==(WireSessionSettings const&) const = default;
};

/// How a ServerHello is laid out on the wire.
enum class HelloLayout : uint8_t
{
    /// NegotiatingHelloMarker, then one blob of the agreed version, our range, our capabilities, the
    /// daemon's machine, and whatever a later version appends. What a negotiating client reads.
    Negotiating = 0,
    /// The version alone: all of a ServerHello a pre-negotiation client (codec 1 = contour 0.7.0, 2, 3)
    /// reads, so it can tell its user which version refused it.
    VersionAlone = 1,
};

/// What a connection is for. A Control connection asks the daemon about itself (DaemonStatusRequest,
/// StopDaemon): it is no client, so it is shown no layout, spawns no first session and keeps nothing alive.
/// Decoding fails closed: a role byte this version does not know reads as Control, so a future role never
/// spawns a session.
enum class ConnectionRole : uint8_t
{
    Attach = 0,  ///< A terminal client: it is shown the daemon's sessions and layout.
    Control = 1, ///< A probe or a command to the daemon itself.
};

/// The client area a ClientHello states, in cells: the whole content area the client renders the
/// layout into, as a ResizeRequest would propose it.
struct WireClientArea
{
    uint32_t columns = 0; ///< The client area's width in cells.
    uint32_t lines = 0;   ///< The client area's height in cells.
    bool operator==(WireClientArea const&) const = default;
};

/// Which of the daemon's windows a connection wants to follow, and whether attaching creates one.
/// Decoding is lenient: a byte no enumerator names reads as AllWindows, which is what a daemon without
/// Capability::AttachIntent does anyway.
enum class AttachIntentKind : uint8_t
{
    AllWindows = 0, ///< Every window, including those created later: the behaviour before intents existed.
    Launch = 1,     ///< A fresh launch: every window when no other client is attached, else a new one.
    NewWindow = 2,  ///< A new window of its own, mirroring none of the existing ones.
    Windows = 3,    ///< Exactly the listed windows (those that exist).
    Resume = 4,     ///< The listed windows after a reconnect; when none survives, as Launch.
};

/// What a ClientHello asks to be shown.
struct AttachIntent
{
    AttachIntentKind kind = AttachIntentKind::AllWindows;
    /// The daemon windows named; meaningful for Windows and Resume only.
    std::vector<uint64_t> windows = {};
    bool operator==(AttachIntent const&) const = default;
};

/// Client's first PDU: the protocol versions and capabilities it speaks, (for TCP) a preshared auth
/// token, and optionally the emulation settings it wants the sessions it creates to have. Anything
/// before it is a protocol error.
///
/// On the wire: `u32 NegotiatingHelloMarker`, one blob of `varint min, varint max, varint count +
/// varint idents, string token, optional session settings` and whatever a later version appends, then
/// a zero byte. A daemon of contour 0.7.0 reads that blob as its token and the zero as "no session
/// settings", and codecs 2 and 3 consume everything after a version not their own: each reads this
/// hello far enough to answer with its version, as the encoder in Pdu.cpp explains.
///
/// A hello whose leading `u32` is not the marker is a pre-negotiation peer's: it decodes as far as that
/// version, into a range of it alone, and the rest is consumed unread.
struct ClientHello
{
    /// The hello's leading `u32`: NegotiatingHelloMarker, or a pre-negotiation peer's codec version.
    uint32_t codecVersion = NegotiatingHelloMarker;
    /// The protocol versions the client speaks; `{ v, v }` for a pre-negotiation peer of codec `v`.
    VersionRange versions = SupportedVersions;
    /// The optional features the client offers on top of the negotiated version.
    CapabilitySet capabilities = {};
    /// Preshared token authenticating the client on the opt-in TCP transport.
    /// Empty over AF_UNIX, where the hardened socket's permissions are the gate.
    std::string token = {};
    /// The client's preference for sessions it creates; absent means "whatever the daemon hosts
    /// with", which is what a client with no configuration of its own wants.
    std::optional<WireSessionSettings> sessionSettings = std::nullopt;
    /// What the connection is for; absent on the wire means Attach.
    ConnectionRole role = ConnectionRole::Attach;
    /// The client area the first session should spawn at, so it needs no resize once attached;
    /// absent means "the daemon's default", and the client's ResizeRequest corrects it. Honoured on
    /// an Attach connection only.
    std::optional<WireClientArea> clientArea = std::nullopt;
    /// Which windows to follow; honoured only when both ends negotiated Capability::AttachIntent.
    AttachIntent attachIntent = {};
    /// The profile the connection's sessions run under, resolved by the daemon in its own configuration;
    /// empty means the daemon's default. Honoured only with Capability::ProfileByName.
    std::string profileName = {};
    bool operator==(ClientHello const&) const = default;
};

/// Server's answer to ClientHello: the version it agreed on (0 refuses the client), the range and
/// capabilities it speaks and, when it accepts the client, the machine it runs on.
///
/// Laid out as @ref layout says. Decoding yields HelloLayout::VersionAlone for a leading `u32` other than
/// NegotiatingHelloMarker -- a pre-negotiation daemon's, the rest consumed unread -- and for the marker with
/// nothing after it; either way `versions` is then the range of that version alone.
struct ServerHello
{
    /// Which of the two layouts this hello travels in.
    HelloLayout layout = HelloLayout::Negotiating;
    /// The hello's leading `u32`: NegotiatingHelloMarker, or a pre-negotiation daemon's codec version.
    uint32_t codecVersion = NegotiatingHelloMarker;
    /// The version both ends speak and the connection continues in; 0 is a refusal.
    uint32_t agreedVersion = 0;
    /// The protocol versions the daemon speaks.
    VersionRange versions = SupportedVersions;
    /// The optional features the daemon offers on top of the agreed version.
    CapabilitySet capabilities = {};
    /// The machine the daemon runs on (vtbackend::HostIdentity), which every WireCommandBlock::locality
    /// was judged against: "local" there means local to the DAEMON, and a client attached from another
    /// machine must not take it for its own. Stated once per connection, as a connection never moves to
    /// another daemon. Each is empty when unknown, and both are empty in the answer to a refused
    /// ClientHello, which tells a peer nothing about the daemon. They name a machine, so a trace never
    /// shows them.
    /// The daemon's machine id derived for this application (vthost::appSpecificMachineId), never
    /// /etc/machine-id itself; empty where the daemon has none.
    std::string machineId = {};
    std::string hostname = {}; ///< The daemon's host name; empty when unknown.
    /// The daemon's release (e.g. "0.8.1"); empty when unknown, and in every refusal.
    std::string releaseVersion = {};
    /// The daemon windows this connection follows, ascending; empty in a refusal and from a daemon that
    /// predates attach intents.
    std::vector<uint64_t> followedWindows = {};
    /// Which run of the daemon answers: drawn at random as the daemon starts, so it differs across a
    /// restart. Window and session ids count from 1 in every run, so this is how a reconnecting client tells
    /// a daemon that restarted -- whose window 1 is not the window 1 it showed -- from the one it lost. 0 is
    /// unknown: in a refusal, and from a daemon that predates the field.
    ///
    /// A daemon that restores a saved workspace after a restart (phase 6) is a new instance all the same: a
    /// client that stayed connected sees its old windows lost, and the restored ones arrive as new windows.
    uint64_t daemonInstance = 0;
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

/// The presence bits a context record's `present` field may carry, as the WIRE numbers them.
///
/// Proto-owned rather than taken from vtbackend, because it travels verbatim. The vtbackend side is
/// pinned against this in vthost/ContextWire.hpp, which is where a renumbering becomes a build error
/// instead of a silent protocol change.
///
/// A record naming a bit outside the mask is refused on decode (DecodeError::MalformedPdu), never
/// masked. The handshake admits only a peer of a version this build speaks, which assigns these bits
/// and no others, so an unknown one can only come from a broken or hostile peer -- and clearing it would
/// leave the reads that follow out of step with whatever field that peer wrote for it.
constexpr inline uint16_t ContextPresentMask = 0x7FFF; // bits 0..14: fifteen fields

/// One OSC 3008 hierarchical context, as the host holds it.
///
/// The same id-plus-pool shape @ref HyperlinkEntry has, one level up: a grid LINE carries the 16-bit
/// id and the record travels in a side table, sent once per connection on first reference so it is
/// immune to the sender's own retention bound dropping it later.
///
/// `present` is what keeps the common record small AND what keeps it honest. Only the fields it names
/// are encoded, so a systemd `type=command` context sends six of fifteen -- and a receiver can still
/// tell "sent empty" from "never mentioned", which is what lets the nearest-cwd walk skip a context
/// rather than stop at it with an empty answer.
struct WireContext
{
    uint16_t id = 0;
    uint16_t parent = 0;
    std::string identifier; ///< The application's own CTXID, unescaped.
    uint8_t type = 0;       ///< vtbackend::ContextType.
    uint16_t present = 0;   ///< vtbackend::ContextFields bits; validated, never cast blindly.

    /// Only the fields `present` names carry meaning; the rest are left empty by the encoder.
    std::string user;
    std::string hostname;
    /// The context's `machineid=` derived for this application (vthost::appSpecificMachineId), as the
    /// ServerHello's is; never the id as the context named it. Empty for one that is no machine id.
    std::string machineId;
    std::string bootId;
    std::string comm;
    std::string workingDirectory;
    std::string commandLine;
    std::string vm;
    std::string container;
    std::string targetUser;
    std::string targetHost;
    std::string sessionId;
    uint64_t pid = 0;
    uint64_t pidFdId = 0;

    uint8_t exitKind = 0; ///< vtbackend::ContextExit.
    uint8_t signal = 0;   ///< vtbackend::ContextSignal, as a Linux signal number.
    uint64_t status = 0;

    bool operator==(WireContext const&) const = default;
};

/// The optional fields a command-block record's `present` byte may name, as the WIRE numbers them.
///
/// Proto-owned and frozen for the reason ContextPresentMask is: they travel verbatim. A bit this build
/// does not assign is refused on decode, as one outside ContextPresentMask is.
constexpr inline uint8_t CommandBlockHasExitCode = 1U << 0;     ///< `exitCode` travelled.
constexpr inline uint8_t CommandBlockHasCommandStart = 1U << 1; ///< `commandStartedAt` travelled.
constexpr inline uint8_t CommandBlockHasDuration = 1U << 2;     ///< `duration` travelled.
constexpr inline uint8_t CommandBlockHasPromptStart = 1U << 3;  ///< `promptStartedAt` travelled.
/// The host no longer holds the block's head row: it scrolled out of the host's history, or an erase left the
/// block no row. Carries no field.
/// A mirror cannot tell a head that left from one whose row has not arrived yet, so the host says which.
constexpr inline uint8_t CommandBlockHeadEvicted = 1U << 4;
constexpr inline uint8_t CommandBlockPresentMask = 0x1F; ///< Every bit this build assigns.

/// One shell command block's record (vtbackend::CommandBlockRecord), as the host holds it.
///
/// A row names its block by WireLine::blockId; the record travels beside the rows — stated outright
/// in a snapshot's SessionState, and in a Delta whenever it changed since this connection last saw
/// it. The id is the HOST's, and a receiver keeps it as-is.
///
/// Deliberately ABSENT: the record's cached head position (`headStableId`, `headIdGeneration`). Those
/// are stable row ids in the HOST's grid, and a mirror's grid mints its own — carried across they
/// would name the wrong row, or, once the two generations happened to coincide, a plausible wrong
/// one. The mirror derives its own from the head rows it writes. Leaving them out also keeps a
/// resize's head rescan, which touches every record, from re-sending every record. Only whether the
/// host's head row is gone travels (CommandBlockHeadEvicted), judged as the record is sent.
///
/// Also absent: `commandContext`, the OSC 3008 context an end= is matched by. Only the host matches an
/// end= against its records; the outcome a match produces reaches the mirror in the record's own
/// outcome fields.
///
/// Enum-ish bytes travel raw and are validated by the consumer, which is what keeps this codec free
/// of vtbackend.
/// @see vthost/CommandBlockWire.hpp
struct WireCommandBlock
{
    uint32_t id = 0;                   ///< The host's block id.
    uint8_t state = 0;                 ///< vtbackend::CommandBlockState.
    uint8_t end = 0;                   ///< vtbackend::CommandBlockEnd.
    uint8_t commandLineSource = 0;     ///< vtbackend::CommandLineSource.
    uint8_t locality = 0;              ///< vtbackend::ContextLocality of `workingDirectory`.
    uint8_t present = 0;               ///< CommandBlockHas* bits.
    std::string commandLine = {};      ///< RAW, as the host recorded it; sanitized per use, never here.
    std::string workingDirectory = {}; ///< The directory snapshot taken at OSC 133;C.
    uint8_t endLocality = 0;           ///< vtbackend::ContextLocality of `endWorkingDirectory`.
    /// The directory the command left the shell in, snapshot at the next prompt's OSC 133;A; empty until
    /// then.
    std::string endWorkingDirectory = {};
    /// vtbackend::ForeignDirectory: whether a directory on another machine was reported while the block was
    /// open.
    uint8_t foreignDirectory = 0;
    uint8_t outcomeExit = 0;    ///< vtbackend::ContextExit.
    uint8_t outcomeSignal = 0;  ///< vtbackend::ContextSignal, as a Linux signal number.
    uint64_t outcomeStatus = 0; ///< `vtbackend::ContextOutcome::status`, verbatim (a varint on the wire).
    /// The four optionals: meaningful only with their CommandBlockHas* bit, zero otherwise. All
    /// signed 64-bit so one table drives their encoding; an exit code outside `int` is the
    /// receiver's to reject.
    ///
    /// The prompt's start is absent for a block a ;C opened with no ;A before it.
    int64_t promptStartedAt = 0; ///< Wall clock, nanoseconds since the Unix epoch (vthost/TimeWire.hpp).
    int64_t exitCode = 0;
    int64_t commandStartedAt = 0; ///< Wall clock, nanoseconds since the Unix epoch.
    int64_t duration = 0;         ///< Steady-clock nanoseconds from OSC 133;C to ;D.
    bool operator==(WireCommandBlock const&) const = default;
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
    /// The application's progress indicator (OSC 9;4), replayed so a re-attaching client sees the
    /// bar an operation still in flight had drawn. @see vtbackend::ProgressState.
    uint8_t progressState = 0;      ///< vtbackend::ProgressState: 0 inactive … 4 paused.
    uint8_t progressPercentage = 0; ///< 0..100.
    /// The OSC 3008 context pool and the ancestry, stated OUTRIGHT rather than behind a changed-gate.
    ///
    /// A snapshot has to assert every field including one sitting at its default, because it routes
    /// state through here rather than through a delta's gate -- and the server, having recorded the
    /// value as sent, never re-sends it. An EMPTY ancestry is exactly such a value, and is how a
    /// Kitty-keyboard reset used to get lost.
    std::vector<WireContext> contexts;
    std::vector<uint16_t> contextChain; ///< The ancestry, outermost first; empty when there is none.
    /// The host terminal's session epoch — the wall-clock instant every WireLine::bornAt counts
    /// from — as nanoseconds since the Unix epoch (vthost/TimeWire.hpp); 0 = not stated. A mirror
    /// adopts it, or the stamps it copied would name instants shifted by however much later the
    /// client terminal was created than the host's.
    int64_t sessionEpoch = 0;
    /// The host's wall clock as it built this snapshot, as nanoseconds since the Unix epoch; 0 = not
    /// stated. The instants a host records -- when a prompt or a command started -- are its own clock's,
    /// and a mirror on a machine whose clock is set differently measures that difference against this,
    /// to read them on its own clock. The session epoch cannot say it: it is when the host's terminal
    /// began, not what its clock reads now.
    int64_t hostTime = 0;
    /// How many times the host's command-block ids have started over (vtbackend::CommandBlockStore::idEpoch):
    /// an id names a block only within its epoch. A receiver holding ids from an older one -- on rows the
    /// host no longer sends, such as a mirror's deeper scrollback -- forgets them when it moves.
    uint64_t commandBlockIdEpoch = 0;
    /// The lowest command-block id this snapshot speaks for: a receiver replaces the records it holds at
    /// or above it with `commandBlocks`, and keeps the older ones until a Delta retires them. Zero speaks
    /// for every id, so the stated set becomes the whole table.
    ///
    /// A snapshot capped below the host's whole store states the oldest record it carries here. A
    /// resize resyncs every pane, and a floor of zero would have the client drop the history it already
    /// holds below the cap, only for the increments to send it all again.
    uint32_t commandBlocksFloor = 0;
    /// The command-block records, oldest first, stated outright: the newest of them whose estimated
    /// size fits the connection's snapshot piece (NativeSession::snapshotPieceBytes, at most
    /// NativeSession::SnapshotChunkBytes). Any older remainder the receiver lacks follows as
    /// increments. Replaces what the receiver holds from `commandBlocksFloor` on.
    std::vector<WireCommandBlock> commandBlocks = {};
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
    /// row above it, plus the semantic marks (the shell's and the user's) and the DECDWL/DECDHL
    /// renditions.
    ///
    /// The bit values are frozen because they travel verbatim; `vthost/GridWire.cpp` pins them, so
    /// renumbering one is a build break rather than a silent mis-decode against an older peer.
    uint16_t flags = 0;
    /// The OSC 3008 context that wrote this row, or 0 for none. Resolved against the context pool the
    /// snapshot and deltas carry; an id with no record simply resolves to nothing, exactly as an
    /// unmapped hyperlink id does.
    uint16_t contextId = 0;
    /// The host's CommandBlockId for this row, or 0 for none.
    ///
    /// Unlike @ref contextId it is NOT translated by a receiver: a mirror's record store holds only
    /// the records the host replicated, under the host's own ids, so the number means the same thing
    /// on both ends.
    uint32_t blockId = 0;
    /// When the cursor first reached this row, as seconds since the host's session began, plus one;
    /// 0 = never stamped. 24 bits of meaning (vtbackend::Line::MaxBornAt) in a 32-bit field, counted
    /// from the host's session epoch, which travels in SessionState.
    uint32_t bornAt = 0;
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

/// Where one PDU sits in a snapshot that was too large to travel as a single frame.
///
/// A full-grid snapshot of a deep scrollback is legitimately megabytes, and a producer that
/// enqueues it whole either exceeds its connection's send-queue bound or hands the peer a frame
/// its decoder must reject. Splitting it means `snapshot != 0` alone can no longer say "clear
/// what you hold and start over" — only the FIRST piece may say that — so each piece names its
/// place instead of the receiver inferring it from the piece before. Inferring it is what breaks
/// when a superseding snapshot drops the remaining pieces of the one in flight: the new run's
/// first piece must clear, and only an explicit marker still says so.
enum class SnapshotPart : uint8_t
{
    Whole = 0,  ///< The entire snapshot in one PDU. Also what a non-snapshot delta carries.
    First = 1,  ///< The first of several: clear, then accumulate.
    Middle = 2, ///< Accumulate.
    Last = 3,   ///< Accumulate, then apply — this piece carries the state the run must land with.
};

/// A batch of changed rows plus the side tables they reference. `snapshot`
/// marks a full resync (attach or generation change) rather than an increment.
struct Delta
{
    uint64_t session = 0;
    uint64_t generation = 0;
    uint64_t seqno = 0;
    uint8_t snapshot = 0;
    /// This PDU's place in a chunked snapshot, as a @ref SnapshotPart. Zero (`Whole`) on every
    /// unchunked delta, so a producer that never splits says nothing and a reader that ignores
    /// it sees exactly the unchunked protocol.
    uint8_t snapshotPart = 0;
    /// The stable id of page row 0 at delta time — what lets the client map
    /// stable-id-addressed rows onto its viewport.
    int64_t stableViewportBase = 0;
    /// The oldest stable id the server still holds (its scrollback floor). Rows
    /// below it were evicted server-side — a `clear`/CSI 3 J jumps this up with
    /// no line changes, so the client MUST drop history below it or keep showing
    /// scrollback the real terminal already threw away.
    int64_t stableFloor = 0;
    /// How many rows have left the server's grid for good since it was created or last reset
    /// (vtbackend::Grid::evictedRowCount()), so the row at `stableFloor` is the session's row
    /// number `evictedRowCount + 1`. A client numbers absolute lines from this rather than from
    /// its own grid's count, which sees neither the rows evicted before it attached, nor a
    /// server-side reset, and which counts a discarding replay's clear as evictions.
    uint64_t evictedRowCount = 0;
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
    /// Set (1) when a status line's content the mirror cannot draw itself changed this batch;
    /// `statusLines` then holds its whole (tiny) grid, painted onto the mirror's status display
    /// `statusLinesDisplay` names. A separate page from the main grid — the multi-page carrier.
    uint8_t statusLinesChanged = 0;
    /// Which display `statusLines` are for, a StatusDisplayType read through vthost::statusDisplayTypeOf():
    /// 2 (host-writable) the application's own status line; 1 (indicator) the indicator row the host's
    /// status-line extensions drew, which the mirror shows in place of its own template -- and no rows there
    /// give the mirror its own back.
    uint8_t statusLinesDisplay = 0;
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
    /// Set (1) when the application's progress indicator changed in this batch (OSC 9;4); the two
    /// bytes below then hold the new state. @see vtbackend::ProgressState.
    uint8_t progressChanged = 0;
    uint8_t progressState = 0;      ///< vtbackend::ProgressState: 0 inactive … 4 paused.
    uint8_t progressPercentage = 0; ///< 0..100.
    /// Set (1) when the OSC 3008 ancestry moved in this batch; `activeContext` then holds the id now
    /// in effect, and 0 means the ancestry is empty.
    uint8_t contextChanged = 0;
    uint16_t activeContext = 0;
    /// Context records first referenced in this batch, in the same "sent once per connection" shape
    /// as @ref hyperlinks. Not gated: like the hyperlink table it is simply empty when there is
    /// nothing new to say, so a bare `!contexts.empty()` would be a second way to spell the same
    /// thing and hasChanges() below does not need one.
    std::vector<WireContext> contexts;
    /// The epoch the command-block ids of this delta's rows and records count in, stated on every delta.
    /// Not one of hasChanges()'s: the ids start over only as the host mints a record, which changes records
    /// too.
    /// @see SessionState::commandBlockIdEpoch
    uint64_t commandBlockIdEpoch = 0;
    /// The grid's stable-id generation (vtbackend::Grid::stableIdGeneration): the counter a row's stable id
    /// names that row within, a strict subset of @ref generation's moves -- a change of the line count alone
    /// leaves every id in place. What a client names a row by when it asks the host to act on one.
    uint64_t stableIdGeneration = 0;
    /// Command-block records that changed since this connection last saw them, oldest first, within
    /// a per-delta byte budget (the rest follow in the next delta). Empty on a snapshot, whose
    /// records travel in SessionState. Not gated, for the reason @ref contexts is not.
    /// @see NativeSession::collectCommandBlockDelta
    std::vector<WireCommandBlock> commandBlocks = {};
    /// Host ids this connection was told about that the host no longer holds — a prompt discarded by
    /// the next one, the oldest evicted by the bound, a reset — ascending.
    std::vector<uint32_t> retiredCommandBlocks = {};

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
               || kittyKeyboardChanged != 0 || modifyOtherKeysChanged != 0 || mouseChanged != 0
               || progressChanged != 0 || contextChanged != 0 || !contexts.empty() || !commandBlocks.empty()
               || !retiredCommandBlocks.empty();
    }

    /// @return Where this PDU sits in its snapshot, or `Whole` for anything that is not part of a
    ///         multi-PDU run — including a value no enumerator names, which a peer built from a
    ///         later revision could send. Treating that as `Whole` is the safe reading: the piece
    ///         is applied on its own rather than held for a run terminator that never comes.
    [[nodiscard]] SnapshotPart part() const noexcept
    {
        // A switch rather than a range check, and every enumerator named: adding a fifth then
        // fails the build here instead of being silently folded into Whole.
        auto const value = static_cast<SnapshotPart>(snapshotPart);
        switch (value)
        {
            case SnapshotPart::Whole:
            case SnapshotPart::First:
            case SnapshotPart::Middle:
            case SnapshotPart::Last: return value;
        }
        return SnapshotPart::Whole; // a byte no enumerator names, from a revision we do not know
    }

    /// @return Whether the receiver must discard what it holds for this session before applying
    ///         this PDU. True for a snapshot that opens a run, or is a run of one.
    [[nodiscard]] bool startsSnapshot() const noexcept
    {
        return snapshot != 0 && (part() == SnapshotPart::Whole || part() == SnapshotPart::First);
    }

    /// @return Whether the receiver holds a whole screen once this PDU is applied, and may render
    ///         it. False only mid-run: publishing then would paint a grid half of which has not
    ///         arrived. Every non-snapshot delta completes trivially.
    [[nodiscard]] bool completesSnapshot() const noexcept
    {
        return snapshot == 0 || part() == SnapshotPart::Whole || part() == SnapshotPart::Last;
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

/// When a notification asks to be shown: what OSC 99's `o=` said, or that nothing was said.
///
/// OSC 9 and OSC 777 state no occasion, and an OSC 99 notification without `o=` states one, so the two
/// differ: the client decides an unstated notification by its own rule for those, as it does a local
/// session's. The codec carries any byte; the client reads one no enumerator names as Unstated
/// (@see vthost::displayOccasionOf).
enum class NotifyOccasion : uint8_t
{
    Unstated = 0,  ///< OSC 9 / OSC 777: no occasion of its own.
    Always = 1,    ///< `o=always`, or OSC 99 without `o=`.
    Unfocused = 2, ///< `o=unfocused`.
    Invisible = 3, ///< `o=invisible`.
};

/// A desktop notification a session raised (OSC 9 / OSC 777 / OSC 99).
///
/// Only the occasion travels of OSC 99's metadata: its identifier, and with it a close or an activation
/// reported back to the application, stay on the daemon.
struct SessionNotify
{
    uint64_t session = 0;
    std::string title = {};
    std::string body = {};
    NotifyOccasion occasion = NotifyOccasion::Unstated; ///< When the notification asks to be shown.
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

/// One OSC 7501 program-status record on the wire. The enums travel as their bytes; a reader that does
/// not know a state drops the record (vthost::control::fromWire), one that does not know a kind reads none.
struct WireProgramStatusRecord
{
    /// The `progress` byte that says the record has no progress.
    static constexpr uint8_t AbsentProgress = 255;

    std::string id = {};               ///< The record's path; empty for the root.
    uint8_t state = 0;                 ///< vtbackend::ProgramState.
    uint8_t kind = 0;                  ///< vtbackend::BlockedKind.
    uint8_t progress = AbsentProgress; ///< Percent done, 0..100, or AbsentProgress.
    std::string app = {};              ///< The record's own app (not an inherited one).
    std::string title = {};            ///< Decoded UTF-8.
    std::string message = {};          ///< Decoded UTF-8.

    bool operator==(WireProgramStatusRecord const&) const = default;
};

/// Server→client push (Capability::ProgramStatus): a session's whole set of program-status records, least
/// recently updated first, replacing what the client mirrored before. An empty list clears it. Not
/// transient like the events above, but pushed per session like them, so it reaches a mirror the same way;
/// a snapshot repeats it. Its text is what a program sent, so the trace never shows it.
struct ProgramStatus
{
    uint64_t session = 0;                              ///< The session the records belong to.
    std::vector<WireProgramStatusRecord> records = {}; ///< The whole set.
    bool operator==(ProgramStatus const&) const = default;
};

/// Server→client push (Capability::InputEcho): what a session's terminal does with typed text, sent when it
/// changes and in a snapshot. Raw on the wire: `echo` is a vtpty::InputEcho's value (0 Unknown, 1 Echoed,
/// 2 Raw, 3 Hidden); a value this build does not know reads as Unknown (vthost::inputEchoFromWire).
struct SessionInputEcho
{
    uint64_t session = 0; ///< The session whose terminal it is.
    uint8_t echo = 0;     ///< A vtpty::InputEcho's value.
    bool operator==(SessionInputEcho const&) const = default;
};

/// Any per-session push a mirror applies as it arrives -- the transient events, and the program-status set --
/// as a type-safe alternative set: what a consumer handling "a session did something" dispatches over,
/// without enumerating the catalog's tags itself.
using SessionEventPdu =
    std::variant<SessionBell, SessionNotify, SessionClipboard, ProgramStatus, SessionInputEcho>;

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
    /// The profile the new tab's session runs under; empty means the connection's profile.
    std::string profileName = {};
    bool operator==(CreateTab const&) const = default;
};

/// Client→server: create a new daemon window (with a first tab + session). The
/// daemon then pushes that window's LayoutState; the client opens a GUI window for
/// it (B4).
struct NewWindow
{
    /// The profile the new window's first session runs under; empty means the connection's profile.
    std::string profileName = {};
    bool operator==(NewWindow const&) const = default;
};

/// Client→server: split the pane hosting @p session (the daemon activates it
/// first), backing the new leaf with a fresh session.
struct SplitPane
{
    uint64_t session = 0;    ///< The pane to split, by the session it hosts.
    uint8_t orientation = 1; ///< vtworkspace::SplitState: 1 horizontal, 2 vertical.
    uint16_t ratio = 5000;   ///< First child's share × 10000.
    /// The profile the new pane's session runs under; empty means the connection's profile.
    std::string profileName = {};
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

/// Client→server: replaces the emulation settings the client stated in its ClientHello, for the sessions
/// it creates from now on — sent when the client's configuration changed, say by a reload.
///
/// Sessions that already exist keep theirs, for the reason ClientHello's apply only at creation: an
/// application that already read DA1 cannot be told its terminal changed identity.
struct SessionSettingsChanged
{
    /// The new preference; absent means "whatever the daemon hosts with" from now on.
    std::optional<WireSessionSettings> sessionSettings = std::nullopt;
    bool operator==(SessionSettingsChanged const&) const = default;
};

/// An operation on a hosted session's terminal engine that a client asks the daemon to perform.
///
/// The codec carries any byte: what an operation does, and whether the daemon knows it at all, is the
/// host's to decide (@see vthost::SessionHost::performOperation), so a value no enumerator names -- from a
/// newer client -- reaches the host and is refused there rather than failing the frame.
enum class EngineOperation : uint8_t
{
    ClearToPrompt = 0,        ///< Drop every row above the current prompt, scrollback and page alike.
    ClearHistoryAndReset = 1, ///< Hard reset (RIS), which drops the scrollback.
    SoftReset = 2,            ///< DECSTR.
    WriteScreen = 3,          ///< Parse the request's text as if the application had written it.
    SetUserMark = 4,          ///< Place the user mark (vi `mm`) on the logical line of the request's row.
    ClearUserMark = 5,        ///< Take the user mark off the logical line of the request's row.
};

/// A row of a hosted session's primary grid, as the host numbers it: a stable id, which names that row only
/// within the stable-id generation it was taken in (Delta::stableIdGeneration).
struct RowAddress
{
    int64_t stableId = 0;    ///< The row's stable id.
    uint64_t generation = 0; ///< The stable-id generation @ref stableId was taken in.
    bool operator==(RowAddress const&) const = default;
};

/// Client→server: perform @p operation on the terminal hosting @p session.
///
/// A client's terminal is a mirror rebuilt from deltas, so an action that changes the engine's state has
/// to run where the session lives; the mirror then follows through the Delta stream, as it follows output.
/// One verb carrying an enum rather than one PDU per action, so the next such action is an enumerator.
struct PerformOperation
{
    uint64_t session = 0;                                       ///< The session to act on.
    EngineOperation operation = EngineOperation::ClearToPrompt; ///< What to perform.
    std::string text = {};                                      ///< WriteScreen's text; empty otherwise.
    RowAddress row = {}; ///< The row SetUserMark and ClearUserMark act on; zero otherwise.
    bool operator==(PerformOperation const&) const = default;
};

/// One command a status-line extension's manifest declares, as a permission request carries it.
struct WireDeclaredCommand
{
    std::string name = {};              ///< The name the extension runs it by.
    std::vector<std::string> argv = {}; ///< Its argv template, each element verbatim.
    bool operator==(WireDeclaredCommand const&) const = default;
};

/// Server→client: a status-line extension of @p session asks whether it may run the commands its manifest
/// declares. The daemon runs extensions; a client with a GUI asks its user, and answers with a
/// PermissionReply. Sent when an extension asks, and again to each client that attaches while the question
/// is still open: one with no client attached stays unanswered, and its command calls fail as pending.
///
/// Everything but @ref session and @ref hostId is the extension's own manifest, and so guest-chosen text:
/// a client shows it only as the consent prompt shows a local extension's.
struct PermissionRequest
{
    uint64_t session = 0;    ///< The session whose extension asks; where the prompt belongs.
    std::string hostId = {}; ///< Which installed extension asks: its config key and its module, as the
                             ///< daemon assigned it.
    std::string name = {};   ///< The manifest's name.
    uint32_t version = 0;    ///< The manifest's format revision.
    std::string digest = {}; ///< The manifest's digest, which the answer is bound to.
    std::vector<WireDeclaredCommand> commands = {}; ///< What it may run, if allowed.
    /// The control-API permissions it asks for (`read`, `screen`, `input`, `write`), granted with the
    /// commands. Appended by a later version: an older sender's body ends before it. Sent only to a client
    /// that shares Capability::ControlPermissions, which shows them in its prompt.
    std::vector<std::string> control = {};
    bool operator==(PermissionRequest const&) const = default;
};

/// Client→server: the user's answer to a PermissionRequest. Keyed as a grant is, by the installed extension
/// and the manifest digest; the daemon records it in its own grant store, for every session.
struct PermissionReply
{
    std::string hostId = {}; ///< PermissionRequest::hostId.
    std::string digest = {}; ///< PermissionRequest::digest.
    /// vthost::GrantState as the wire spells it; read through vthost::grantStateOf(), which answers nothing
    /// for a byte no enumerator has.
    uint8_t grantState = 0;
    bool operator==(PermissionReply const&) const = default;
};

/// Either direction, after the handshake: the frame with serial @ref refusedSerial carried a PDU tag this
/// peer does not know, or knows but cannot serve (a Control connection's attach-only request, a daemon
/// control request on an endpoint with no daemon behind it). Its receiver stops sending that tag for the rest
/// of the connection, and never answers an Unsupported itself. Before the handshake no PDU is answered with
/// one: the daemon answers anything but a ClientHello with a version-alone ServerHello and ends the
/// connection, and the client ignores anything but the ServerHello.
/// @see UnsupportedTracker
struct Unsupported
{
    uint64_t refusedSerial = 0; ///< The serial of the frame that carried the unknown tag.
    uint64_t ident = 0;         ///< The tag this peer does not know or cannot serve.
    bool operator==(Unsupported const&) const = default;
};

/// Client→server: asks for a DaemonStatusReply. It carries nothing. Served on a connection of either
/// role; a Control connection exists to send it without becoming a client.
struct DaemonStatusRequest
{
    bool operator==(DaemonStatusRequest const&) const = default;
};

/// Server→client: the answer to a DaemonStatusRequest.
struct DaemonStatusReply
{
    std::string releaseVersion = {}; ///< The daemon's release.
    uint64_t processId = 0;          ///< The daemon's process id.
    uint64_t uptimeSeconds = 0;      ///< Seconds since the daemon started serving.
    std::string socketPath = {};     ///< The endpoint it listens on.
    std::string executablePath = {}; ///< The image it runs from.
    std::string logFile = {};        ///< Where it logs; empty when it does not log to a file.
    uint32_t sessions = 0;           ///< How many sessions it hosts.
    uint32_t clients = 0;            ///< How many attached clients it counts (Control connections excluded).
    uint8_t lifecycle = 0;           ///< Its DaemonLifecycle, as the wire spells it: when it ends on its own.
    bool operator==(DaemonStatusReply const&) const = default;
};

/// Why a daemon is asked to stop.
enum class StopReason : uint8_t
{
    User = 0,           ///< The user asked.
    UpgradeRestart = 1, ///< It is replaced by a newer release.
    /// The user asked, and the daemon keeps its workspace for its next start. Sent only to a daemon that
    /// offers Capability::WorkspaceRestore: an older one refuses the byte.
    UserKeepingWorkspace = 2,
};

/// Client→server: asks the daemon to shut down. Served on a connection of either role, as a
/// DaemonStatusRequest is.
struct StopDaemon
{
    /// A StopReason as the wire spells it; read through vthost::stopReasonFrom().
    uint8_t reason = 0;
    bool operator==(StopDaemon const&) const = default;
};

/// What an EditTab changes. The codec carries any byte; the host refuses one it does not know.
enum class TabEditKind : uint8_t
{
    ActivatePane = 0, ///< Make the pane hosting `session` the tab's active pane.
    Zoom = 1,         ///< Zoom the pane hosting `session`.
    Unzoom = 2,       ///< Unzoom the tab holding `session`.
    SetTitle = 3,     ///< Set the tab's title to `title`.
    ClearTitle = 4,   ///< Return the tab to its automatic title.
    SetColor = 5,     ///< Set the tab's colour to `color`.
    ClearColor = 6,   ///< Return the tab to its default colour.
    ActivateTab = 7,  ///< Make the tab holding `session` the window's active tab.
};

/// Client→server: the user changed how a tab is presented, and the daemon's model follows so every other
/// client sees it.
struct EditTab
{
    /// The pane (ActivatePane, Zoom) or any pane of the tab (the rest) the edit addresses.
    uint64_t session = 0;
    TabEditKind kind = TabEditKind::ActivatePane;
    std::string title = {}; ///< The new title; SetTitle only.
    uint32_t color = 0;     ///< The new colour as 0xRRGGBB; SetColor only.
    bool operator==(EditTab const&) const = default;
};

/// Either direction: a liveness probe. The client sends it under a request serial and the daemon echoes the
/// same nonce under the same serial, so a client can tell a quiet daemon from a vanished one.
struct Heartbeat
{
    uint64_t nonce = 0;
    bool operator==(Heartbeat const&) const = default;
};

/// Whether a ControlReply carries the reply or an error.
enum class ControlOutcome : uint8_t
{
    Ok = 0,    ///< `body` is the method's reply JSON.
    Error = 1, ///< `body` is a `ControlError` JSON.
};

/// Client→server: one call of the control API (Capability::ControlApi). The reply comes under the same
/// serial and carries the same `requestId`. `params`, `body` and `event` can hold pane text, so the trace
/// never shows them.
struct ControlRequest
{
    uint64_t requestId = 0;   ///< Chosen by the caller; echoed by the reply.
    std::string method = {};  ///< The method name, e.g. `pane.split`.
    std::string params = {};  ///< The request JSON; empty reads as `{}`.
    std::string context = {}; ///< The caller's pane id (`$CONTOUR_PANE`), or empty.
    bool operator==(ControlRequest const&) const = default;
};

/// Server→client: the outcome of a ControlRequest.
struct ControlReply
{
    uint64_t requestId = 0;                      ///< The request's id.
    ControlOutcome outcome = ControlOutcome::Ok; ///< Whether `body` is a reply or an error.
    std::string body = {};                       ///< The reply JSON, or a `ControlError` JSON on `Error`.
    bool operator==(ControlReply const&) const = default;
};

/// Server→client push: an event of a subscription made with `events.subscribe`.
struct ControlEvent
{
    uint32_t subscription = 0; ///< The subscription it belongs to.
    std::string event = {};    ///< The event JSON, as `vtcontrol::encodeEvent` writes it.
    bool operator==(ControlEvent const&) const = default;
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
                                ResizeSplit,
                                SessionSettingsChanged,
                                PerformOperation,
                                PermissionRequest,
                                PermissionReply,
                                Unsupported,
                                DaemonStatusRequest,
                                DaemonStatusReply,
                                StopDaemon,
                                EditTab,
                                Heartbeat,
                                ControlRequest,
                                ControlReply,
                                ControlEvent,
                                ProgramStatus,
                                SessionInputEcho>;

/// Encodes @p pdu (body + frame) into @p sink.
/// @param sink The output writer.
/// @param serial Request correlation; 0 = unsolicited push.
/// @param pdu Any catalog PDU.
void encodePdu(Writer& sink, uint64_t serial, DecodedPdu const& pdu);

/// @return Roughly how many bytes @p line occupies once encoded.
///
/// For deciding where to split a payload that is too large to send as one frame, and for nothing
/// else. The exact size is known only after encoding, and encoding a row twice to learn a number
/// whose only job is to pick a split point would double the work; being within a small factor is
/// enough for a budget whose consumer pads it anyway.
///
/// Lives beside the encoder it approximates for the reason `Delta::hasChanges` lives beside its
/// gates: a field added to `encodeCell` has to be remembered here too, and nothing points across
/// a module boundary to say so. `Pdu_test` pins the two together against a real `Writer`.
[[nodiscard]] std::size_t estimatedEncodedSize(WireLine const& line) noexcept;

/// @return An upper bound on how many bytes @p block occupies once encoded.
///
/// Unlike the row estimate this one is a BOUND, never below the real size, because what it sizes is
/// not split further: it decides how many records one delta carries and how many a snapshot states,
/// and an under-count there is a frame larger than the budget it was sized against. `Pdu_test` pins
/// it against a real `Writer`.
/// @param block The record to size.
[[nodiscard]] std::size_t estimatedEncodedSize(WireCommandBlock const& block) noexcept;

/// One piece of a snapshot whose rows did not fit a single frame: an index range plus its
/// place in the run.
struct SnapshotPiece
{
    std::size_t begin = 0; ///< Index of this piece's first row.
    std::size_t count = 0; ///< How many rows it covers; zero only for an empty input.
    /// Where this piece sits in the run.
    ///
    /// Carried here rather than left to the caller so the rule with the sharp edge — a lone piece
    /// is `Whole`, never a `First` whose `Last` never comes — has one home and one test. A caller
    /// re-deriving it from "am I the first element, am I the last" gets that case wrong by writing
    /// the two conditions independently, and the peer then waits forever for a terminator.
    SnapshotPart part = SnapshotPart::Whole;
};

/// Splits @p lines into consecutive pieces, each aiming to stay within @p budget bytes.
///
/// Two rules make the result always usable rather than sometimes impossible:
/// - **A row is never split.** It is the smallest thing the wire can address, so a row that alone
///   exceeds @p budget gets a span to itself and the budget is overshot. The budget is a target,
///   not a guarantee — its consumer must not depend on the bound.
/// - **An empty input yields one empty span.** A payload with no rows is still a payload, and its
///   sender still has one PDU's worth of state to deliver.
/// @param lines The rows to partition, in order.
/// @param budget The byte target per piece, by @ref estimatedEncodedSize.
/// @return One entry per piece, covering @p lines exactly once, in order, each marked with its
///         place in the run — a single piece is `Whole`, never `First` followed by nothing.
[[nodiscard]] std::vector<SnapshotPiece> partitionSnapshotRows(std::span<WireLine const> lines,
                                                               std::size_t budget);

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
