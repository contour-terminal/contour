// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeSession` — one native-protocol (cells+deltas) client connection.
///
/// The server emulates, the client renders: after the ClientHello/ServerHello
/// version handshake the session pushes a full snapshot (SessionState + a
/// snapshot Delta per hosted session), then per-line deltas driven by the
/// host's screen-updated signal, debounced so bursts coalesce into one Delta.
///
/// Snapshots and increments travel differently, and the difference is the point. An increment is
/// small by construction — the rows that changed in one 20ms window. A snapshot is the whole
/// grid INCLUDING all scrollback, which is legitimately megabytes and has no smaller form. So
/// snapshots are queued per session and emitted by a single streamer coroutine that waits for
/// room in the send queue before each piece and splits anything over `snapshotPieceBytes`.
/// Emitting them inline instead is what made a daemon with a couple of scrollback-heavy panes
/// permanently unattachable: the burst never yielded, so the drain never ran, so the whole attach
/// payload had to fit the send-queue bound at once — and past two panes it did not.
/// Grid rows are addressed by stable id; a generation change triggers one
/// resync snapshot. Hyperlink URIs ship once per connection on first
/// reference; image pixels only on FetchImage.

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/WriteQueue.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/Unsupported.hpp>

namespace vthost
{

/// What the daemon side of the handshake admits: the token, versions and capabilities it requires
/// and offers.
struct HandshakePolicy
{
    /// The preshared token every ClientHello must carry; empty accepts any (the AF_UNIX default, where
    /// the socket's permissions are the gate).
    std::string expectedToken = {};
    /// The protocol versions this endpoint speaks; the highest one shared with the client is agreed.
    proto::VersionRange versions = proto::SupportedVersions;
    /// The optional features this endpoint offers on top of the agreed version.
    proto::CapabilitySet capabilities = proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions });
};

/// The collaborators and policy a native connection is constructed with.
///
/// Later phases append members, every one with a default, so a test or an embedded host names only
/// what it needs, and no phase adds a constructor parameter.
struct NativeSessionServices
{
    HandshakePolicy handshake = {}; ///< What the handshake admits.
};

/// One connected native-protocol client.
class NativeSession final: public SessionStreamEvents
{
  public:
    /// The default send-queue bound (see the constructor).
    static constexpr std::size_t DefaultWriteQueueBytes = std::size_t { 4 } * 1024 * 1024;

    /// The largest snapshot payload one PDU carries; a grid bigger than this travels as several.
    ///
    /// Two cliffs make a lone unbounded frame wrong, and this clears both. It sits far below the
    /// send-queue bound, so the streamer can always wait for room for a whole piece — and a piece
    /// that lands on a drained backlog is never refused, whatever its size. And it sits orders of
    /// magnitude below @ref proto::MaxFrameSize, so no scrollback depth can build a frame the
    /// peer's own decoder rejects as FrameTooLarge. A bound too small for that cuts finer:
    /// @see snapshotPieceBytes
    static constexpr std::size_t SnapshotChunkBytes = std::size_t { 256 } * 1024;

    /// The most command-block record bytes one INCREMENTAL delta carries (by
    /// proto::estimatedEncodedSize, which never under-counts).
    ///
    /// An increment is emitted inline, unpaced, so whatever it carries must fit beside a backlog. A
    /// flood of OSC 133 cycles — a `cat` of a recorded session — can change a whole store's worth of
    /// records within one debounce window, at up to MaxRecordedCommandLineBytes each, which unbounded
    /// is megabytes in one frame. Budgeted, the remainder waits for the next window: records arrive
    /// late, never lost, and never at the cost of the connection. A connection whose snapshot piece
    /// is smaller spends that instead, for the reason the piece is smaller; and while its send queue
    /// has no room, records wait altogether.
    /// @see snapshotPieceBytes
    /// @see hasSendRoom
    static constexpr std::size_t CommandBlockDeltaBytes = std::size_t { 64 } * 1024;

    /// The snapshot piece size under a send-queue bound of @p maxWriteQueueBytes — which is also
    /// the backlog the streamer waits to get under before each piece.
    ///
    /// A quarter of the bound, capped at @ref SnapshotChunkBytes, and the watermark is one piece: a
    /// LOW-water mark, not "the bound minus a piece". A run then keeps under two pieces — about half the
    /// bound, the cut being by estimated size — in the backlog, so every frame sent beside it that cannot
    /// pace itself (an increment, a bell, a LayoutState, a FetchImage answer) has the other half, and a
    /// piece overshooting its estimate twice over still lands under the bound. Riding the backlog just
    /// under the bound instead would leave those frames a single piece of headroom, and the queue would
    /// refuse them — the connection dropped by the pacing meant to keep it. A piece of a fixed size
    /// fails the same way: under a bound smaller than itself it waits in the backlog already over the
    /// bound, refusing everything enqueued behind it. The default bound is sixteen chunks, so it keeps
    /// the cap.
    /// @param maxWriteQueueBytes The connection's send-queue bound.
    /// @return The budget, in estimated encoded bytes; never zero, which a watermark cannot get under.
    [[nodiscard]] static constexpr std::size_t snapshotPieceBytes(std::size_t maxWriteQueueBytes) noexcept
    {
        return std::max(std::size_t { 1 }, std::min(SnapshotChunkBytes, maxWriteQueueBytes / 4));
    }

    /// @param loop The event loop everything runs on.
    /// @param host The session host (not owned; outlives this).
    /// @param id This connection's diagnostic identity, prefixed onto its every log line so
    ///        the accept, handshake, trace and disconnect lines read as one story.
    /// @param connection The client transport (owned).
    /// @param maxWriteQueueBytes Send-queue byte bound; a client whose backlog
    ///        exceeds it is disconnected rather than under-served (its delta
    ///        cursor has already moved past what an overflow would drop).
    /// @param services The collaborators and policy of this connection, its handshake's among them.
    NativeSession(core::net::EventLoop& loop,
                  SessionHost& host,
                  ConnectionId id,
                  std::unique_ptr<core::net::ISocket> connection,
                  std::size_t maxWriteQueueBytes = DefaultWriteQueueBytes,
                  NativeSessionServices services = {});

    /// The connection flow: handshake, initial snapshot, then serve until the
    /// peer disconnects.
    [[nodiscard]] core::async::Task<void> run();

    /// Marks @p session changed and schedules a debounced delta flush (the
    /// connection subscribes itself to the host's stream fan-out).
    void sessionScreenUpdated(vtworkspace::SessionId session) override;

    /// Drops the follow state for a session the host destroyed, so a long-lived
    /// connection that churns through many sessions does not accumulate it.
    void sessionClosed(vtworkspace::SessionId session) override;

    void sessionResized(vtworkspace::SessionId session) override;

    void sessionBell(vtworkspace::SessionId session) override;
    void sessionNotify(vtworkspace::SessionId session,
                       std::string const& title,
                       std::string const& body,
                       std::optional<vtbackend::DisplayOccasion> occasion) override;
    void sessionCopyToClipboard(vtworkspace::SessionId session, std::string const& data) override;

    /// Asks this client's user, whose GUI shows the prompt in @p session's pane. Sent as the question opens;
    /// a client attaching later is asked from SessionHost::pendingPermissionRequests() instead.
    void sessionPermissionRequested(vtworkspace::SessionId session,
                                    proto::PermissionRequest const& request) override;

    /// Adapts the host's model-change fan-out into a single "layout changed"
    /// callback: every structural change re-pushes the whole LayoutState. It is a
    /// `vtworkspace::ModelEvents` the caller subscribes to the host (see @ref
    /// layoutObserver), so the model stays transport-agnostic.
    struct LayoutObserver final: vtworkspace::ModelEvents
    {
        std::function<void()> onChange;
        void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int) override { onChange(); }
        void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int) override { onChange(); }
        void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int) override { onChange(); }
        void tabMovedToWindow(
            vtworkspace::WindowId, vtworkspace::TabId, int, vtworkspace::WindowId, int) override
        {
            onChange();
        }
        void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int) override { onChange(); }
        void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { onChange(); }
        void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { onChange(); }
        void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override { onChange(); }
        void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override { onChange(); }
        void paneOrientationChanged(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::SplitState) override
        {
            onChange();
        }
        void paneSwapped(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override
        {
            onChange();
        }
        void paneZoomChanged(vtworkspace::TabId, std::optional<vtworkspace::PaneId>) override { onChange(); }
        void paneTreeRestructured(vtworkspace::TabId) override { onChange(); }
        void tabTitleChanged(vtworkspace::TabId) override { onChange(); }
        void tabColorChanged(vtworkspace::TabId) override { onChange(); }
    };

    /// The layout observer to subscribe to the host's model fan-out (see
    /// serveNativeClient) so live tab/pane changes reach this connection.
    [[nodiscard]] LayoutObserver& layoutObserver() noexcept { return _layoutObserver; }

    /// @param type A PDU type this connection may send.
    /// @return Whether the client answered it with Unsupported, so it is no longer sent.
    [[nodiscard]] bool peerRefused(proto::PduType type) const noexcept
    {
        return _unsupported.peerRefused(type);
    }

    /// @return What the handshake agreed on; nullopt until a ClientHello was accepted.
    [[nodiscard]] std::optional<proto::Negotiated> const& negotiated() const noexcept { return _negotiated; }

  private:
    friend struct NativeSessionFollowTester; ///< Test-only view of _followed.

    /// A walk over a session's command-block records that a budget, a cap or a full send queue cut short;
    /// the next increment resumes it where it stopped rather than from the oldest record.
    ///
    /// It sends, oldest first, every record stamped after FollowState::sentBlockStamp, and once it reaches
    /// its end this peer holds every record stamped at or below @ref target in its current form.
    struct CommandBlockWalk
    {
        vtbackend::CommandBlockId from {}; ///< The oldest record the walk has yet to look at.
        /// Where it ends: the oldest record a capped snapshot stated, which this peer holds as of
        /// @ref target already; nullopt walks on to the newest.
        std::optional<vtbackend::CommandBlockId> until {};
        uint64_t target = 0; ///< The CommandBlockStore::contentRevision() the walk brings this peer up to.
    };

    /// Per followed grid: the delta cursor plus what this connection has seen.
    struct FollowState
    {
        vtbackend::GridDeltaCursor cursor;
        /// id -> the URI last sent for it. A map, not a set: the terminal's 16-bit
        /// HyperlinkId counter wraps and reuses ids, so an id whose URI changed
        /// must be resent — keyed by id alone the mirror would keep the stale URI.
        std::unordered_map<uint16_t, std::string> sentHyperlinks;
        std::vector<uint32_t> lastModes;     ///< Mirrored DEC-mode set as last sent.
        std::vector<uint32_t> lastAnsiModes; ///< Mirrored ANSI-mode set as last sent.
        /// The cursor position last sent to the mirror. A cursor-only move (no cell
        /// change, no mode flip) must still produce a delta, or the mirror's cursor
        /// stays put; -1 until the first (always-snapshot) delta sends one.
        int32_t lastCursorLine = -1;
        int32_t lastCursorColumn = -1;
        /// The scrollback floor last sent to this connection. A `clear`/CSI 3 J evicts history
        /// through Grid::clearHistory, which deliberately bumps no generation and dirties no line —
        /// so the floor is the ONLY thing that moves, and without remembering it here the delta
        /// carrying it would be gated away and the mirror would keep rendering the history the
        /// terminal threw away. INT64_MIN until the first (always-snapshot) delta sends one; no real
        /// floor can equal it, so the first push always counts as a move.
        int64_t lastStableFloor = std::numeric_limits<int64_t>::min();
        /// The displayed page last mirrored. Every one of the 16 pages (primary,
        /// the DEC pages 1..14, the xterm alternate at 15) is a distinct grid with
        /// its own, independently-advancing generation, so a page flip is a
        /// wholesale identity change that must force a resync — keyed on the page
        /// index, not on primary-vs-alternate (which collapses DEC pages 1..14).
        /// nullopt — a session never pushed before — forces the initial snapshot.
        std::optional<vtbackend::PageIndex> lastDisplayedPage;
        /// The window title last sent to this connection, so an incremental delta
        /// carries the title (and forces a send) only when it actually changed.
        std::string lastTitle;
        /// The DECSCUSR Ps last sent (-1 until the first snapshot), so a cursor-shape
        /// change carries (and forces) a delta only when it actually changed.
        int lastCursorShape = -1;
        /// The OSC 7 working-directory URL last sent, so a cwd change carries (and
        /// forces) a delta only when it actually changed. `_cwdKnown` distinguishes
        /// "never sent" from "sent empty".
        std::string lastCwd;
        bool cwdKnown = false;
        /// The OSC 3008 context records this peer has been told about, keyed by id and remembering
        /// the WHOLE record. A map for the reason `sentHyperlinks` is: the id space is a uint16_t
        /// that wraps, so a REUSED id has to be recognised as a new context rather than silently
        /// re-pointing every line already stamped with it. The whole record rather than its
        /// identifier alone because a context is also reinitialised IN PLACE -- a re-`start=` on
        /// every prompt, an `end=` recording an outcome -- and those carry news too.
        std::unordered_map<uint16_t, proto::WireContext> sentContexts;
        /// The ContextStack revision `sentContexts` was last brought in line with, so the per-flush
        /// walk over the retained pool is skipped entirely while the ancestry has not moved. Reset to
        /// its sentinel wherever `sentContexts` is cleared, or the re-send that clearing exists to
        /// force would be gated away.
        uint64_t lastContextRevision = 0;
        bool contextRevisionKnown = false;
        /// The active context last sent (-1 until the first snapshot), so a change carries -- and
        /// forces -- a delta only when the ancestry actually moved.
        int lastActiveContext = -1;
        /// While no walk is open: this peer holds every command-block record stamped at or below this
        /// CommandBlockStore::contentRevision() in its current form. A record changes in place under one id
        /// (C, then D, then an OSC 3008 enrichment), and the store restamps it each time, so a record is
        /// re-sent exactly when something the wire carries moved — without a copy of it kept here to
        /// compare against. Zero, which no record is stamped with, until a snapshot says otherwise.
        uint64_t sentBlockStamp = 0;
        /// The walk an increment left unfinished; nullopt between walks.
        std::optional<CommandBlockWalk> blockWalk;
        /// The ids of the records this peer holds, ascending: a merge-join against the store's names the
        /// ones the host let go of, which the peer is told to retire.
        std::vector<uint32_t> sentBlockIds;
        /// The content revision `sentBlockIds` was last joined against the store at, so a walk resumed
        /// over a store that has not moved since joins nothing.
        uint64_t blockIdsJoinedAt = 0;
        /// The default fg/bg (0xRRGGBB) last sent (-1 until the first snapshot),
        /// so an OSC 10/11 change carries (and forces) a delta only on change.
        int lastDefaultForeground = -1;
        int lastDefaultBackground = -1;
        /// The status-display type/active last sent (-1 until the first snapshot),
        /// so a DECSSDT/DECSASD change carries (and forces) a delta only on change.
        int lastStatusDisplayType = -1;
        int lastActiveStatusDisplay = -1;
        /// The status-line rows last sent, and the display they were for (StatusDisplayType; 0 for none
        /// sent), so the (tiny) status page re-ships only when its content changed, and a mirror showing
        /// the host's indicator row is told when that stops.
        std::vector<proto::WireLine> lastStatusLines;
        uint8_t lastStatusLinesDisplay = 0;
        /// The Kitty keyboard protocol flags last sent (-1 until the first snapshot),
        /// so a change carries (and forces) a delta only when it actually changed.
        int lastKittyKeyboardFlags = -1;
        /// xterm's modifyOtherKeys level last sent (-1 until the first snapshot); the
        /// other half of the input-encoding state a client has to track.
        int lastModifyOtherKeys = -1;

        /// The resolved mouse-reporting state last sent, diffed as ONE value because that is what
        /// it is — the wire's own struct, so there is no second declaration to keep in step.
        ///
        /// Unlike the -1 sentinels above, the zero value here is not "unknown" but the state a
        /// freshly built terminal is genuinely in — so a first delta against an app that already
        /// enabled tracking reports the difference rather than assuming it was sent.
        proto::MouseState lastMouse {};

        /// The progress indicator last sent, diffed as ONE value for the same reason `lastMouse` is:
        /// its two fields change together and mean nothing apart. Zero here is likewise the genuine
        /// state of a fresh terminal ("nothing shown"), not an "unknown" sentinel.
        vtbackend::Progress lastProgress {};
    };

    void handlePdu(proto::DecodedFrame const& frame);

    /// Encodes @p pdu and hands it to the write queue, disconnecting on refusal. A PDU the client answered
    /// with Unsupported is not sent at all.
    /// @param serial The request serial to answer, or 0 for an unsolicited push.
    /// @param pdu What to send.
    /// @param sessionTag The session this frame describes, or 0 when it describes none
    ///        (hello, layout). A later snapshot for the same session supersedes the
    ///        tagged ones still unwritten — @see core::net::WriteQueue::dropTagged.
    void send(uint64_t serial, proto::DecodedPdu const& pdu, uint64_t sessionTag = 0);

    /// Records why this connection's read loop ended.
    ///
    /// Before this existed, a client that sent a malformed frame was simply dropped, with the
    /// decoder's verdict discarded — the daemon's single most opaque failure mode.
    /// @param outcome What pumpPdus reported.
    void reportPumpOutcome(PumpResult const& outcome) const;

    /// Pushes a transient session event (bell / notification / clipboard) as an
    /// unsolicited frame (serial 0), once the handshake completed.
    ///
    /// Takes the built event rather than its parts, so the pre-handshake / post-close guard is stated
    /// once for all three event shapes instead of once per shape — and takes the narrow
    /// @ref proto::SessionEventPdu rather than the whole catalog, so nothing but an event can reach it.
    /// @param event A SessionBell, SessionNotify or SessionClipboard.
    void emitSessionEvent(proto::SessionEventPdu const& event);

    /// Serializes the host's window/tab/pane tree into a LayoutState and pushes it
    /// (serial 0), once the handshake completed — on attach and on every change.
    void pushLayout();

    /// Validates the peer's ClientHello, answers, and queues the attach
    /// snapshot (spawning the first session on an empty daemon).
    /// @return False when the hello was missing or version-mismatched.
    [[nodiscard]] bool completeHandshake(proto::DecodedFrame const& frame);

    /// Adopts the emulation settings the client states for the sessions it creates from now on, in
    /// its ClientHello or later in a SessionSettingsChanged.
    /// @param stated The client's preference; nullopt for the host's own.
    void adoptClientSessionSettings(std::optional<proto::WireSessionSettings> const& stated);

    /// Whether a push re-describes the whole grid or reports only what changed since this
    /// connection's delta cursor.
    enum class SnapshotMode : uint8_t
    {
        Delta = 0,  ///< Report the rows changed since @c FollowState::cursor.
        Forced = 1, ///< Re-describe the whole grid: attach, resize, page flip or a lost cursor.
    };

    /// What became of the frames a snapshot supersedes that were still waiting in the send queue.
    enum class SupersededFrames : uint8_t
    {
        None = 0,    ///< None waited: everything sent before the snapshot reaches the peer ahead of it.
        Dropped = 1, ///< Some were dropped unsent, so what they carried never arrives.
    };

    /// Whether a push said everything this peer is owed about command-block records.
    enum class CommandBlockBacklog : uint8_t
    {
        Drained = 0, ///< Every record this peer has not seen travelled.
        Pending = 1, ///< A budget, a cap or a full send queue held records back for a later delta.
    };

    /// Whether @ref requeueOnSendRoom is parked on the send queue's watermark.
    enum class RoomWaiter : uint8_t
    {
        Idle = 0,   ///< No waiter: the next session whose records find no room spawns one.
        Parked = 1, ///< One waits, and re-queues every session in @ref _recordsAwaitingRoom when it wakes.
    };

    /// One session's push, built under the terminal's lock and emitted after it is released.
    ///
    /// Building and emitting are separate steps because the two emission policies differ and
    /// neither may hold the lock while it waits: an increment goes out at once, a snapshot is
    /// paced against the send queue and split across PDUs.
    struct BuiltPush
    {
        proto::Delta delta;                       ///< The rows and state to send.
        std::optional<proto::SessionState> state; ///< Precedes the delta, on a snapshot.
    };

    /// Why @ref buildPush produced no push.
    enum class BuildFailure : uint8_t
    {
        NoSuchSession = 0, ///< The host no longer hosts it.
        /// The increment asked for cannot describe this batch — row identity moved out from under
        /// the delta cursor. Reported rather than silently upgraded to a snapshot, because a
        /// snapshot is the whole grid and belongs on the paced path, not inline on the caller's.
        /// Nothing in the follow state has been advanced when this is returned, so the caller may
        /// simply ask for a snapshot instead.
        ResyncRequired = 1,
        /// The delta would tell this peer nothing it has not been told: no cell, mode, cursor or
        /// scrollback-floor movement since the last push. Unlike the two above, the follow state
        /// HAS been advanced when this is returned — there was simply nothing to send.
        NothingToSay = 2,
    };

    /// Collects @p session's push under the terminal's lock, advancing this connection's follow
    /// state to match what the returned push will say.
    ///
    /// Command-block records it leaves owed — a budget or a cap cut their walk short, or the send queue
    /// had no room — it sees followed up itself, once the lock is released, whatever it returns: a quiet
    /// session raises no further screen update to carry them.
    /// @see followUpRecords
    /// @param session The session to describe.
    /// @param mode Whether to re-describe the whole grid or only what changed.
    /// @return The built push, or why none was built.
    [[nodiscard]] std::expected<BuiltPush, BuildFailure> buildPush(vtworkspace::SessionId session,
                                                                   SnapshotMode mode);

    /// Sends SessionState + an incremental delta for @p session, deferring to @ref requestSnapshot
    /// when an increment turns out not to be able to describe the batch.
    void pushDelta(vtworkspace::SessionId session);

    /// Queues a full-grid snapshot of @p session and starts the streamer if it is idle.
    ///
    /// Queued rather than sent: a snapshot is the whole grid, several of them are asked for at
    /// once (attach walks every pane, a resize re-projects every pane), and emitting them inline
    /// puts the entire payload in the send queue before the drain has run even once.
    void requestSnapshot(vtworkspace::SessionId session);

    /// @return Whether @p session has a snapshot queued or in flight. An increment for such a
    ///         session must be held back: the snapshot re-describes everything the increment
    ///         would say, and one landing BETWEEN a snapshot's pieces would have the mirror
    ///         repaint a grid it has only half received.
    [[nodiscard]] bool snapshotPending(vtworkspace::SessionId session) const noexcept;

    /// Parks until the send queue has room for a snapshot piece.
    /// @return False when the connection is going away and the caller should stop.
    [[nodiscard]] core::async::Task<bool> awaitSendRoom();

    /// Awaited before every read of the connection: while this connection's input backs up in a session
    /// past the high-water mark, parks until the session holds no more than the low-water mark of it.
    /// @see SessionHost::inputWatermarks
    [[nodiscard]] core::async::Task<void> awaitInputRoom();

    /// Marks this connection backed up on @p session once the session holds the high-water mark of what
    /// clients handed it, so that it is read no further until awaitInputRoom() sees it drain.
    /// @param session The session this connection just handed something.
    void pauseReadingIfBackedUp(vtworkspace::SessionId session);

    /// Whether the send queue has room right now: the backlog is under one snapshot piece — the
    /// watermark @ref awaitSendRoom waits for, asked without waiting.
    ///
    /// What a producer that cannot wait but CAN defer consults: command-block records are state a
    /// later delta carries just as well, so under a slow reader they lag instead of piling onto a
    /// backlog until the queue refuses a frame and the connection goes.
    /// @return true when the backlog is under @ref _snapshotPieceBytes.
    [[nodiscard]] bool hasSendRoom() const noexcept;

    /// Sees the command-block records a push left owed to @p session sent later: by the next debounce
    /// window while the send queue has room, else once it has room again — parked on its watermark by
    /// @ref requeueOnSendRoom, rather than rebuilding the session every window to find none.
    /// @param session The session whose records are owed.
    void followUpRecords(vtworkspace::SessionId session);

    /// The one waiter for room for command-block records: parks until the send queue's backlog is under
    /// a piece, then queues an increment for every session in @ref _recordsAwaitingRoom.
    [[nodiscard]] core::async::Task<void> requeueOnSendRoom();

    /// Arms the debounced delta flush unless one is already armed or the connection is closing.
    void scheduleFlush();

    /// Marks @p session as owed an increment and arms the debounced flush: what a screen update
    /// does, and what a push that left command-block records unsent has done for it.
    /// @param session The session to push in the next debounce window.
    void queueIncrement(vtworkspace::SessionId session);

    /// The single snapshot streamer: drains @ref _snapshotQueue, one session at a time, waiting
    /// for room in the send queue before each piece. One streamer rather than one coroutine per
    /// snapshot, so two runs for the same session can never interleave on the wire.
    [[nodiscard]] core::async::Task<void> streamSnapshots();

    /// Emits @p delta as one PDU, or as a run of @ref _snapshotPieceBytes-sized ones when its rows
    /// exceed that, waiting for room before each.
    /// @param delta The snapshot to send (consumed).
    /// @param session The session it describes; also the supersede tag its pieces carry.
    [[nodiscard]] core::async::Task<void> sendSnapshotPieces(proto::Delta delta,
                                                             vtworkspace::SessionId session);

    /// Pulls the session's live renditional state (title, cursor shape, cwd,
    /// colours, status display, Kitty-keyboard flags) into @p delta as diffs and —
    /// on a snapshot — captures the full state into @p state. Called by buildPush
    /// with the terminal already locked; split out to keep buildPush within the
    /// cognitive-complexity budget. Static: it reads only its arguments.
    /// @param screenTypeValue The wire screen-type discriminator (std::to_underlying).
    static void collectLiveState(vtbackend::Terminal& terminal,
                                 FollowState& follow,
                                 proto::Delta& delta,
                                 std::optional<proto::SessionState>& state,
                                 vtworkspace::SessionId session,
                                 uint8_t screenTypeValue,
                                 SnapshotMode mode);

    /// Captures the live OSC 3008 ancestry into @p delta, or — on a snapshot, which carries the records
    /// through SessionState instead — only brings @p follow up to date.
    ///
    /// Split out of collectLiveState for the same reason that one is split out of pushDelta: it is a
    /// self-contained pull+diff over its own state, and inline it pushed collectLiveState past the
    /// cognitive-complexity budget. Static: it reads only its arguments.
    static void collectContextState(vtbackend::Terminal& terminal,
                                    FollowState& follow,
                                    proto::Delta& delta,
                                    SnapshotMode mode);

    /// States the command-block records into a snapshot's @p state: the newest whose estimated size
    /// fits this connection's snapshot piece, oldest first, converting only those. The piece caps them
    /// because SessionState is one unchunked frame, which has to fit beside a backlog the way a piece
    /// does.
    ///
    /// Uncapped, the snapshot speaks for every id. Capped, it speaks only from its floor, the oldest
    /// record it states, and a walk takes the older records this peer lacks — unless @p superseded
    /// says frames were dropped, after which nothing this peer holds below the cap can be trusted and
    /// it speaks for every id after all.
    /// @see proto::SessionState::commandBlocksFloor
    /// @return Pending when a cap left older records to the increments.
    [[nodiscard]] CommandBlockBacklog collectCommandBlockSnapshot(vtbackend::Terminal& terminal,
                                                                  FollowState& follow,
                                                                  proto::SessionState& state,
                                                                  SupersededFrames superseded) const;

    /// Captures into an increment's @p delta the command-block records this peer has not seen in their
    /// current form.
    ///
    /// The walk runs only while one is open or the store's content revision moved past
    /// FollowState::sentBlockStamp, and only while the send queue has room (@ref hasSendRoom): without
    /// it nothing is sent and the result is Pending. It sends, oldest first, each record stamped after
    /// FollowState::sentBlockStamp until CommandBlockDeltaBytes or the snapshot piece, whichever is
    /// less, is spent (always at least one), and lists as retired every id this peer holds that the
    /// store no longer does.
    ///
    /// Cost: nothing while the content revision is unchanged; otherwise a pass over the stamps from
    /// where the walk stands — integers only — converting just the records stamped after the last one
    /// sent, plus a merge-join of the ids this peer holds against the store's when the store moved.
    /// @return Pending when records were left unsent.
    [[nodiscard]] CommandBlockBacklog collectCommandBlockDelta(vtbackend::Terminal& terminal,
                                                               FollowState& follow,
                                                               proto::Delta& delta) const;

    /// What every session THIS connection creates is spawned with.
    ///
    /// One spelling for all four creation paths (the attach-time first session, CreateTab, NewWindow,
    /// SplitPane), so a fifth cannot quietly forget the client's preference.
    [[nodiscard]] SessionSpawnRequest spawnRequest() const
    {
        return SessionSpawnRequest { .settings = _clientSessionSettings };
    }

    /// Tells whether this client is asked extensions' permission questions: both ends offered
    /// proto::Capability::ExtensionPermissions. A client that did not stays unasked, and the questions
    /// wait for another, as they do while no client is attached.
    /// @return true once a handshake shared the capability.
    [[nodiscard]] bool asksPermissions() const noexcept
    {
        return _negotiated.has_value()
               && _negotiated->shared.contains(proto::Capability::ExtensionPermissions);
    }

    [[nodiscard]] core::async::Task<void> flushSoon();

    core::net::EventLoop& _loop;
    SessionHost& _host;
    std::unique_ptr<core::net::ISocket> _connection;
    core::net::WriteQueue _writer;
    /// What a snapshot is cut into, and the backlog each piece waits to get under before it is
    /// enqueued: @ref snapshotPieceBytes of this connection's bound.
    std::size_t _snapshotPieceBytes;
    ConnectionId _id;                ///< Prefixes this connection's every log line.
    NativeSessionServices _services; ///< The handshake's policy, and later phases' collaborators.
    /// What the handshake agreed on; engaged once a ClientHello was accepted, which is what "handshaken"
    /// means everywhere below.
    std::optional<proto::Negotiated> _negotiated;
    /// The emulation settings this client asked the sessions IT creates to have, layered onto the
    /// host's own in completeHandshake; nullopt when the client stated no preference.
    ///
    /// Per CONNECTION rather than per daemon, and applied only at creation: that is what lets two
    /// clients on different profiles share a daemon without either re-emulating the other's
    /// sessions. An application that already read DA1 cannot be told its terminal changed identity.
    std::optional<vtbackend::Settings> _clientSessionSettings;
    LayoutObserver _layoutObserver;
    /// Answers the client's unknown PDUs, and holds back the ones it said it does not know.
    proto::UnsupportedTracker _unsupported;
    std::unordered_map<uint64_t, FollowState> _followed;
    std::unordered_set<uint64_t> _pendingSessions;
    /// Sessions awaiting a full-grid snapshot, in the order they were asked for.
    std::deque<uint64_t> _snapshotQueue;
    /// The session @ref streamSnapshots is emitting right now; engaged exactly while a streamer
    /// coroutine is live, which is also how @ref run knows it may let `this` go.
    ///
    /// Distinct from the queue because a session is popped before its pieces go out, and an
    /// increment for it must stay held back for the whole run — until the send queue's backlog
    /// behind its last piece is back under one piece — not just while it waits its turn.
    std::optional<uint64_t> _streamingSession;
    /// Sessions whose command-block records wait for room in the send queue.
    /// @see followUpRecords
    std::unordered_set<uint64_t> _recordsAwaitingRoom;
    /// Whether @ref requeueOnSendRoom is live, which @ref run waits out before it lets `this` go — kept
    /// apart from the set, which a closed session leaves while the waiter still holds `this`.
    RoomWaiter _roomWaiter = RoomWaiter::Idle;
    bool _flushScheduled = false;
    bool _closed = false;
    /// The session this connection's input backed up in, past the high-water mark: the connection is read
    /// no further until it drains. @see awaitInputRoom
    std::optional<vtworkspace::SessionId> _inputBackedUp;
};

/// The daemon's connection-handler factory for native-protocol clients.
/// @param loop The event loop every connection runs on.
/// @param host The session host every connection serves (not owned; outlives the handler).
/// @param services What each connection is constructed with.
/// @return The handler.
[[nodiscard]] ConnectionHandler makeNativeHandler(core::net::EventLoop& loop,
                                                  SessionHost& host,
                                                  NativeSessionServices services = {});

} // namespace vthost
