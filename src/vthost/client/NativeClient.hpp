// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeClient` — the native-protocol client engine (Qt-free).
///
/// The client runs NO parser: it mirrors each remote session's screen from the
/// server's stable-id-addressed Delta stream into a `RemoteScreen` — a plain
/// data model any frontend can render (the TTY attach client, later the GUI's
/// remotely-populated display seam). Input flows the other way as Input PDUs.

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/WriteQueue.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vthost/PduPump.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::client
{

/// Appends @p cell's grapheme cluster (base codepoint plus any combining extras)
/// to @p out as UTF-8. The empty-codepoint (blank) case is the caller's to
/// handle. Shared by every wire-cell → text/tty path so cluster emission lives
/// in exactly one place.
/// @param out The byte stream to append to.
/// @param cell The wire cell whose cluster to emit.
void appendCluster(std::string& out, proto::WireCell const& cell);

/// One command-block record a mirror holds, and the RemoteScreen revision it last changed at — so a
/// consumer that remembers the revision it last synced at adopts only what moved since.
struct MirroredCommandBlock
{
    proto::WireCommandBlock wire; ///< The record as the host last sent it.
    uint64_t revision = 0;        ///< The RemoteScreen::commandBlocksRevision it last changed at.
};

/// The client-side mirror of one remote session's screen.
struct RemoteScreen
{
    /// @param historyKeep How many rows above the viewport to retain; `nullopt` is unbounded.
    ///        Defaulted so a test can build a bare mirror, but `NativeClient` always states it.
    /// @param commandBlockLimits How many command-block records to keep; defaulted likewise.
    explicit RemoteScreen(std::optional<int64_t> historyKeep = DefaultHistoryKeep,
                          vtbackend::CommandBlockStoreLimits commandBlockLimits = {}):
        historyKeep(historyKeep),
        // At least one, as the store raises it: a table that could hold nothing would refuse every record.
        commandBlockLimits { .maxRecords = std::max(commandBlockLimits.maxRecords, std::size_t { 1 }) }
    {
    }

    uint64_t session = 0;
    uint32_t columns = 0;
    uint32_t lines = 0;
    uint8_t screenType = 0;
    int32_t cursorLine = 0;
    int32_t cursorColumn = 0;
    std::string title;
    uint8_t cursorShape = 0; ///< DECSCUSR Ps (0 = unknown/default); re-emitted as CSI Ps SP q.
    /// The OSC 7 working-directory URL, as the host's terminal holds it; handed to the mirror's terminal
    /// directly (vtbackend::Terminal::setCurrentWorkingDirectory), not re-emitted as OSC 7.
    std::string cwd;
    uint32_t defaultForeground = 0;  ///< 0xRRGGBB default foreground, re-emitted as OSC 10.
    uint32_t defaultBackground = 0;  ///< 0xRRGGBB default background, re-emitted as OSC 11.
    uint8_t statusDisplayType = 0;   ///< StatusDisplayType, re-emitted as DECSSDT.
    uint8_t activeStatusDisplay = 0; ///< ActiveStatusDisplay, re-emitted as DECSASD.
    uint8_t kittyKeyboardFlags = 0;  ///< Kitty keyboard flags, re-emitted as CSI = flags ; 1 u.
    uint8_t modifyOtherKeys = 0;     ///< xterm modifyOtherKeys level, re-emitted as CSI > 4 ; level m.
    /// The RESOLVED mouse-reporting state, applied to the mirror's input generator directly rather
    /// than through DEC modes. @see proto::MouseState.
    proto::MouseState mouse {};
    uint8_t progressState = 0;      ///< ProgressState (OSC 9;4), applied to the mirror's terminal.
    uint8_t progressPercentage = 0; ///< The progress percentage, 0..100.
    /// Status-line rows the host drew, painted on the status display @ref statusLinesDisplay names.
    std::vector<proto::WireLine> statusLines;
    /// Which display @ref statusLines are for: a StatusDisplayType, as the wire spells it.
    /// @see proto::Delta::statusLinesDisplay
    uint8_t statusLinesDisplay = 0;

    uint64_t generation = 0;
    /// The stable-id generation the host's row ids count in.
    /// @see proto::Delta::stableIdGeneration
    uint64_t stableIdGeneration = 0;
    uint64_t seqno = 0;
    int64_t viewportBase = 0; ///< Stable id of viewport row 0.
    int64_t stableFloor = 0;  ///< Oldest stable id the server still holds; rows below are evicted.
    /// Rows the server's grid has evicted for good, so the row at `stableFloor` is the session's row
    /// number `evictedRowCount + 1`. @see proto::Delta::evictedRowCount.
    uint64_t evictedRowCount = 0;

    /// Rows by stable id (ordered, so eviction trims the oldest first).
    std::map<int64_t, proto::WireLine> rows;
    /// Hyperlink id → URI, merged from the deltas' side tables.
    std::unordered_map<uint16_t, std::string> hyperlinks;

    /// OSC 3008 context records, keyed by the SENDER's id, accumulated the way `hyperlinks` is: sent
    /// once on first reference and kept here so a later fullReplay can re-assert them.
    std::unordered_map<uint16_t, proto::WireContext> contexts;

    /// The ancestry, outermost first, in the sender's id space. Empty when there is none -- and that
    /// emptiness is itself asserted, so a stack that emptied is not mistaken for one never sent.
    std::vector<uint16_t> contextChain;

    /// The host's session epoch (nanoseconds since the Unix epoch) every row's bornAt counts from;
    /// 0 until a SessionState stated it.
    /// @see proto::SessionState::sessionEpoch.
    int64_t sessionEpoch = 0;

    /// The host's wall clock as the last SessionState stated it (nanoseconds since the Unix epoch); 0 until
    /// one did. @see proto::SessionState::hostTime.
    int64_t hostTime = 0;

    /// The epoch the host's command-block ids count in, as the last SessionState or Delta stated it.
    /// @see proto::SessionState::commandBlockIdEpoch.
    uint64_t commandBlockIdEpoch = 0;

    /// The command-block records the host holds, keyed by the HOST's id and ordered by it (oldest
    /// first). A snapshot replaces the table from its floor on (proto::SessionState::commandBlocksFloor);
    /// a delta merges into it and erases what it retires — so,
    /// like `contexts`, it is always the latest state, and a mirror that missed a callback catches up
    /// from it rather than from the deltas it never saw.
    ///
    /// Bounded as the store a mirror feeds is, for the peer chooses what it sends: each record's strings
    /// are held to what a record keeps (vtbackend::MaxRecordedCommandLineBytes,
    /// vtbackend::MaxRecordedWorkingDirectoryBytes), and the count to `commandBlockLimits`, the oldest
    /// dropped first. One older than every record a full table holds is not taken at all.
    std::map<uint32_t, MirroredCommandBlock> commandBlocks;
    /// Each record of `commandBlocks` under the revision it last changed at, mapped to its id: one entry
    /// per record. A consumer synced at a revision finds what changed since with an upper_bound, rather
    /// than by walking the table.
    std::map<uint64_t, uint32_t> commandBlocksByRevision;
    /// Bumped once per record that changed (and stamped on it), and once per batch of removals.
    uint64_t commandBlocksRevision = 0;
    /// The revision of the last removal -- a retirement the host sent, or a record the cap dropped -- so a
    /// mirror synced before it has records to drop.
    uint64_t commandBlocksRetiredAt = 0;

    /// Whether a chunked snapshot is still arriving: set by a piece that opens or continues a
    /// run, cleared by the one that completes it.
    ///
    /// A frontend must never be handed a screen mid-run — it holds part of a grid, and painting
    /// it would render the rows not yet delivered as absent. This is what every publish gate in
    /// NativeClient asks, including the Delta one that could equally have asked the PDU: an
    /// ImageData/ImageGone reply carries no marker and CAN land between two pieces:
    /// the client sends a FetchImage on seeing a new image id in an early piece, and the server
    /// answers it into the same stream the rest of the run is still using. So the screen has to
    /// remember what its last piece said.
    bool snapshotInProgress = false;
    /// The mirrored DEC private modes currently SET remotely (by number).
    std::vector<uint32_t> setModes;
    /// The mirrored ANSI modes currently SET remotely (by number) — a separate
    /// number space from the DEC one. @see vthost/MirroredModes.h.
    std::vector<uint32_t> setAnsiModes;

    /// Image-covered cells, keyed by stable row id then column: which image tile
    /// (pool-local id + in-image cell offset + layer) is shown at that cell.
    /// Replaced per row as deltas redraw it; trimmed with the row eviction.
    std::map<int64_t, std::map<uint16_t, proto::ImageCellEntry>> imageCells;
    /// Fetched image pixels by pool-local image id. The id pool is per session,
    /// so this map is already session-scoped. Filled on ImageData, dropped on
    /// ImageGone.
    std::unordered_map<uint32_t, proto::ImageData> images;
    /// Image ids already requested (or delivered): avoids re-issuing a FetchImage
    /// whose answer is still in flight. Cleared for an id on ImageGone.
    std::unordered_set<uint32_t> requestedImages;

    /// How many rows above the viewport to keep as client-side scrollback when the client stated no
    /// profile of its own. @see historyKeep for why it is not simply a constant.
    static constexpr int64_t DefaultHistoryKeep = 10000;

    /// How many rows above the viewport to keep as client-side scrollback; `nullopt` is unbounded.
    ///
    /// Configuration, fixed at construction: `NativeClient` resolves it once from the profile the
    /// client stated in its ClientHello. A hardcoded ceiling silently contradicted a user who had
    /// asked for deeper scrollback — the daemon would serve the rows and the mirror would throw
    /// them away — so the same `history.limit` governs both ends.
    ///
    /// Unbounded is safe rather than reckless: `stableFloor` is authoritative in the eviction
    /// below, so the retained set is bounded by the daemon's own `maxHistoryLineCount` regardless.
    std::optional<int64_t> historyKeep;

    /// How many command-block records `commandBlocks` keeps: what the terminal a mirror feeds keeps
    /// (`command_blocks.max_records`), so a peer cannot make this client hold more than its store would.
    /// Configuration, fixed at construction like `historyKeep`; maxRecords is at least one.
    vtbackend::CommandBlockStoreLimits commandBlockLimits;

    /// Applies the session-state snapshot PDU.
    void apply(proto::SessionState const& state);

    /// Applies one delta (or snapshot) and evicts rows past the history cap.
    void apply(proto::Delta const& delta);

    /// @return The row currently at viewport row @p line (0-based), or nullptr
    ///         for a row the client has no data for (render as blank).
    [[nodiscard]] proto::WireLine const* rowAt(int32_t line) const;

    /// The viewport as plain text (one LF-terminated line per row, trailing
    /// blanks trimmed) — the test- and debug-friendly projection.
    [[nodiscard]] std::string viewportText() const;

    /// @return The image-cell entry at (@p stableId, @p column), or nullptr.
    [[nodiscard]] proto::ImageCellEntry const* imageAt(int64_t stableId, uint16_t column) const;

    /// @return The cached pixels for @p imageId, or nullptr if not fetched yet.
    [[nodiscard]] proto::ImageData const* imageData(uint32_t imageId) const;

    /// Drops @p imageId: forgets its pixels and clears every cell referencing it
    /// (those cells render blank until redrawn). Called on ImageGone.
    void dropImage(uint32_t imageId);
};

/// One attached native-protocol connection.
class NativeClient final
{
  public:
    /// Handler invoked after every applied Delta with the updated screen.
    using UpdateHandler = std::function<void(RemoteScreen const&, proto::Delta const&)>;
    /// Handler invoked when an image's pixels arrive (ImageData) or the image is
    /// dropped (ImageGone).
    using ImageHandler = std::function<void(RemoteScreen const&, uint32_t imageId)>;
    /// Handler invoked when a transient session event (bell / desktop notification /
    /// OSC 52 clipboard write) arrives.
    using SessionEventHandler = std::function<void(RemoteScreen const&, proto::SessionEventPdu const&)>;
    /// Handler invoked when the daemon's tab/pane layout arrives.
    using LayoutHandler = std::function<void(proto::LayoutState const&)>;
    /// Handler invoked when a status-line extension the daemon runs asks this client's user whether it may
    /// run its declared commands; answered with answerPermission().
    using PermissionHandler = std::function<void(proto::PermissionRequest const&)>;

    /// Everything this client states about itself in its ClientHello.
    ///
    /// Grouped rather than passed as two more constructor parameters: they are one message's
    /// payload, and a constructor already carrying four handlers should not grow an argument per
    /// wire field.
    struct HandshakeOptions
    {
        /// The preshared auth token (empty over AF_UNIX, where the socket permissions are the gate).
        std::string token = {};
        /// The emulation settings to ask for the sessions THIS client creates; absent means
        /// "whatever the daemon hosts with". @see proto::WireSessionSettings.
        std::optional<proto::WireSessionSettings> sessionSettings = std::nullopt;
    };

    /// @param loop The event loop everything runs on.
    /// @param connection The server transport (owned).
    /// @param handshake What to state in the ClientHello.
    /// @param commandBlockLimits How many command-block records each mirrored screen keeps: the cap of the
    ///        terminals the mirrors feed (RemoteScreen::commandBlockLimits).
    /// @param onUpdate Invoked after every applied Delta with the updated screen.
    /// @param onImage Invoked when an image's pixels arrive (ImageData) or the
    ///        image is dropped (ImageGone).
    /// @param onSessionEvent Invoked when a transient session event (bell /
    ///        desktop notification / OSC 52 clipboard write) arrives.
    /// @param onLayout Invoked when the daemon's tab/pane layout arrives.
    /// @param onPermissionRequest Invoked when a daemon's extension asks this client's user; none for a
    ///        client that shows no prompt, whose questions wait for another.
    NativeClient(core::net::EventLoop& loop,
                 std::unique_ptr<core::net::ISocket> connection,
                 HandshakeOptions handshake,
                 vtbackend::CommandBlockStoreLimits commandBlockLimits,
                 UpdateHandler onUpdate,
                 ImageHandler onImage,
                 SessionEventHandler onSessionEvent,
                 LayoutHandler onLayout,
                 PermissionHandler onPermissionRequest = {});

    /// The connection flow: sends ClientHello, mirrors server pushes until the
    /// server disconnects or detach() is called.
    [[nodiscard]] core::async::Task<void> run();

    /// Replaces the update handler at runtime. The constructor is the primary
    /// configuration path; use this only when a handler must be swapped mid-life.
    void setUpdateHandler(UpdateHandler handler) { _onUpdate = std::move(handler); }

    /// Replaces the image handler at runtime. The constructor is the primary
    /// configuration path; use this only when a handler must be swapped mid-life.
    void setImageHandler(ImageHandler handler) { _onImage = std::move(handler); }

    /// Replaces the session-event handler at runtime. The constructor is the primary
    /// configuration path; use this only when a handler must be swapped mid-life.
    void setSessionEventHandler(SessionEventHandler handler) { _onSessionEvent = std::move(handler); }

    /// Replaces the layout handler at runtime. The constructor is the primary
    /// configuration path; use this only when a handler must be swapped mid-life.
    void setLayoutHandler(LayoutHandler handler) { _onLayout = std::move(handler); }

    /// Sends keyboard/paste bytes to @p session's PTY.
    void sendInput(uint64_t session, std::string_view bytes);

    /// Proposes the CLIENT AREA size — the whole content area the layout is rendered into, not any
    /// one pane. The server projects the pane tree into it and answers with fresh snapshots.
    void requestResize(uint32_t columns, uint32_t lines);

    /// Reports the grid ONE pane is actually rendered at, refining the server's ratio-driven
    /// projection of the client area. Send it for a pane whose extent the projection cannot derive —
    /// a pixel-driven layout, or a divider the user dragged. @see proto::ResizePane.
    void resizePane(uint64_t session, uint32_t columns, uint32_t lines);

    /// Requests an image's pixels: @p session scopes the per-session image pool,
    /// @p imageId is the pool-local id carried by the cell's ImageCellEntry.
    void fetchImage(uint64_t session, uint32_t imageId);

    /// Layout authoring (F2): ask the daemon to create a tab, split @p tab's
    /// active pane (orientation 1 horizontal / 2 vertical, ratio × 10000), or
    /// close the pane hosting @p session. The daemon honors it and re-pushes a
    /// LayoutState to every attached client.
    /// @param beside A session of the window the new tab belongs in; 0 = the daemon's own first
    ///        window. Named by session because window ids are per model (@see proto::CreateTab).
    void createTab(uint64_t beside = 0);
    void createWindow();
    void splitPane(uint64_t session, uint8_t orientation, uint16_t ratio);
    void closePane(uint64_t session);

    /// Reports a divider the user moved, so the ratio lives in the daemon's model and a re-attaching
    /// client rebuilds the tree at it. The split is named by the leftmost leaf session of each of its
    /// children. @see proto::ResizeSplit.
    void resizeSplit(uint64_t firstSession, uint64_t secondSession, uint16_t ratio);

    /// Asks the daemon to perform an engine operation on a session's terminal, where the session lives; the
    /// mirror follows through the deltas that change produces.
    /// @param request The session, the operation, and what it acts with.
    void performOperation(proto::PerformOperation request);

    /// Answers a status-line extension's PermissionRequest: the daemon records it for every session.
    /// @param hostId PermissionRequest::hostId.
    /// @param digest PermissionRequest::digest.
    /// @param answer The user's answer; Pending for a dismissed prompt, which records nothing.
    void answerPermission(std::string hostId, std::string digest, GrantState answer);

    /// Restates the emulation settings to ask for the sessions this client creates from now on, when
    /// the client's configuration changed; the sessions that exist keep theirs. Sends nothing when
    /// @p settings is what the daemon already has. @see proto::SessionSettingsChanged.
    /// @param settings The new preference; nullopt for whatever the daemon hosts with.
    void restateSessionSettings(std::optional<proto::WireSessionSettings> settings);

    /// Closes the connection; run() finishes.
    void detach();

    /// @return All mirrored screens, keyed by session id.
    [[nodiscard]] std::map<uint64_t, RemoteScreen> const& screens() const noexcept { return _screens; }

    /// @return True once the ServerHello arrived with a matching version.
    [[nodiscard]] bool connected() const noexcept { return _connected; }

    /// @return True if the server answered with an incompatible codec version.
    [[nodiscard]] bool versionMismatch() const noexcept { return _versionMismatch; }

    /// The machine the daemon runs on, as its ServerHello stated it: what every record's locality was
    /// judged against, so "local" in a record means local to THAT machine, which need not be this one.
    /// Fixed for the connection once connected() — before any session is mirrored — and empty until
    /// then; a field stays empty where the daemon does not know it.
    /// @see proto::ServerHello::machineId, vthost::daemonMachineOf
    /// @return The daemon's identity.
    [[nodiscard]] vtbackend::HostIdentity const& daemonIdentity() const noexcept { return _daemonIdentity; }

    /// How many rows above the viewport a mirror keeps; `nullopt` is unbounded.
    ///
    /// Public because it is a pure decision about @p handshake with no state behind it, and one
    /// worth stating on its own: it is where `history.limit`'s wire spelling — `-1` unlimited,
    /// `0` meaning "the default" exactly as `vthost::hostedSessionSettings` reads it on the server
    /// — is turned into a retention bound. @see RemoteScreen::historyKeep.
    /// @param handshake What this client states about itself.
    /// @return The bound to stamp on every screen this client creates.
    [[nodiscard]] static std::optional<int64_t> resolveHistoryKeep(HandshakeOptions const& handshake);

  private:
    void handlePdu(proto::DecodedFrame const& frame);
    /// Encodes @p pdu with the next serial, enqueues it, and returns that serial
    /// so image fetches can correlate the (session-less) ImageData/ImageGone reply.
    /// Once run() has begun to close the connection nothing more is written, so no write can
    /// start under its half-close; the answer is then 0, which no request carries.
    uint64_t send(proto::DecodedPdu const& pdu);

    /// Records why the read loop ended. A protocol or transport failure is the only
    /// explanation the user will ever get for an attach that silently stopped working, so it
    /// is logged as an error; an ordinary detach or peer close is not worth a line.
    /// @param outcome What pumpPdus reported.
    static void reportPumpOutcome(PumpResult const& outcome);

    /// The mirror of @p session, constructed with this client's configuration on first mention.
    /// @param session The session id the server named.
    /// @return Its mirror.
    [[nodiscard]] RemoteScreen& screenFor(uint64_t session);

    core::net::EventLoop& _loop; ///< The loop the connection runs on; run() ends it in order there.
    std::unique_ptr<core::net::ISocket> _connection;
    core::net::WriteQueue _writer;
    /// Declared before _handshake so the constructor can resolve it from its own parameter, rather
    /// than from a member whose initialization order it would otherwise have to reason about.
    std::optional<int64_t> _historyKeep;
    /// Sent verbatim in the ClientHello; its settings are then kept as what the daemon was last told,
    /// which restateSessionSettings() replaces.
    HandshakeOptions _handshake;
    vtbackend::CommandBlockStoreLimits _blockLimits; ///< Stamped on every screen this client creates.
    UpdateHandler _onUpdate;
    ImageHandler _onImage;
    SessionEventHandler _onSessionEvent;
    LayoutHandler _onLayout;
    PermissionHandler _onPermissionRequest;
    std::map<uint64_t, RemoteScreen> _screens;
    /// Outstanding image fetches: request serial → (session, imageId). The reply
    /// carries no session, so the serial is what routes it to the right screen.
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> _pendingImages;
    uint64_t _nextSerial = 1;
    vtbackend::HostIdentity _daemonIdentity; ///< @see daemonIdentity()
    bool _connected = false;
    bool _versionMismatch = false;
    bool _detached = false;
    bool _closing = false; ///< Set once run() has begun its orderly close; send() then writes nothing.
};

} // namespace vthost::client
