// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeSession` — one native-protocol (cells+deltas) client connection.
///
/// The server emulates, the client renders: after the ClientHello/ServerHello
/// version handshake the session pushes a full snapshot (SessionState + a
/// snapshot Delta per hosted session), then per-line deltas driven by the
/// host's screen-updated signal. A screen update that answers a client's input (its echo), and the first
/// one after a snapshot, flush at once; everything else coalesces for 20ms into one Delta (@see FlushPolicy).
///
/// Snapshots and increments travel differently, and the difference is the point. An increment is
/// small by construction — the rows that changed in one coalescing window. A snapshot is the whole
/// grid INCLUDING all scrollback, which is legitimately megabytes and has no smaller form. So
/// snapshots are queued per session and emitted by a single streamer coroutine that waits for
/// room in the send queue before each piece and splits anything over `snapshotPieceBytes`.
/// Emitting them inline instead is what made a daemon with a couple of scrollback-heavy panes
/// permanently unattachable: the burst never yielded, so the drain never ran, so the whole attach
/// payload had to fit the send-queue bound at once — and past two panes it did not.
/// Grid rows are addressed by stable id; a generation change triggers one
/// resync snapshot. Hyperlink URIs ship once per connection on first
/// reference; image pixels only on FetchImage.
///
/// All of that is for an Attach connection, the hello's default role. A Control connection only asks the
/// daemon about itself and calls its control API: it is sent its ServerHello, the answers to its
/// daemon-control and control requests and the control events it subscribed to, and nothing else — no
/// layout, no snapshot, no screen of the host's — and it counts as no client.

#include <vtbackend/core/ClipboardTarget.hpp>
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <core/Random.hpp>
#include <core/async/Task.hpp>
#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsContext.hpp>
#include <core/net/WriteQueue.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlApi.hpp>
#include <vtcontrol/Events.hpp>
#include <vthost/AccessFile.hpp>
#include <vthost/AttachPlan.hpp>
#include <vthost/AuthorizedKeys.hpp>
#include <vthost/ClientCensus.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/FlushPolicy.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/Unsupported.hpp>

namespace vthost
{

/// How a connection authenticates by SSH key: the keys it may use, and what binds a proof to this connection.
struct KeyAuthPolicy
{
    std::vector<AuthorizedKey> keys; ///< The keys that may log in, each with its grants.
    DaemonAuth auth;                 ///< Whether a token is still accepted beside the keys.
    /// This connection's TLS channel binding (RFC 9266), which the signed message covers.
    std::array<std::byte, 32> channelBinding;
    std::reference_wrapper<core::IRandom> random; ///< Where each challenge's nonce comes from.
};

// PreAuthTimeout, how long a connection has by default to authenticate, lives in KeyAuth.hpp, where a client
// that waits on it can name it too.

/// What the daemon side of the handshake admits: the token, versions and capabilities it requires
/// and offers.
struct HandshakePolicy
{
    /// The preshared tokens a ClientHello may carry, each with what it grants; empty accepts any hello with
    /// every grant (the AF_UNIX default, where the socket's permissions are the gate). An Attach hello needs
    /// a token that grants everything.
    std::vector<AcceptedToken> acceptedTokens = {};
    /// Key authentication, for a connection that can bind it to its TLS session; empty for a unix socket,
    /// whose permissions are the gate, and for any endpoint without keys. Per connection, so set only by the
    /// handler that has the connection's TLS session in hand.
    std::optional<KeyAuthPolicy> keyAuth = std::nullopt;
    /// How long a connection has, from its accept, to have a hello accepted: the TLS handler's budget for
    /// the handshake and the hello together, the local handler's for the hello alone. A local peer is no
    /// stranger, but one that says nothing would hold its connection -- and the relay of a stdio attach in
    /// front of it -- for ever. The handlers turn it into @ref authDeadline as they accept.
    std::chrono::milliseconds helloTimeout = PreAuthTimeout;
    /// The instant, on the event loop's clock, by which a hello must have been accepted; the connection is
    /// closed at it otherwise. Empty for a session constructed without a handler. Per connection, as it runs
    /// from the accept: set by the handler that knows when that was, from @ref helloTimeout.
    std::optional<core::platform::SteadyTimePoint> authDeadline = std::nullopt;
    /// The protocol versions this endpoint speaks; the highest one shared with the client is agreed.
    proto::VersionRange versions = proto::SupportedVersions;
    /// The optional features this endpoint offers on top of the agreed version.
    proto::CapabilitySet capabilities = proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions,
                                                                   proto::Capability::InitialClientArea,
                                                                   proto::Capability::AttachIntent,
                                                                   proto::Capability::TabEdits,
                                                                   proto::Capability::Heartbeat,
                                                                   proto::Capability::ProgramStatus,
                                                                   proto::Capability::ProgramStatusTiming,
                                                                   proto::Capability::InputKinds,
                                                                   proto::Capability::ControlPermissions,
                                                                   proto::Capability::InputEcho,
                                                                   proto::Capability::TabMerge,
                                                                   proto::Capability::MoveNode,
                                                                   proto::Capability::LayoutBatch,
                                                                   proto::Capability::TabEditAcks,
                                                                   proto::Capability::ColorPreference,
                                                                   proto::Capability::ClipboardTargets,
                                                                   proto::Capability::SoftFonts,
                                                                   proto::Capability::WorkspaceSwitch,
                                                                   proto::Capability::DaemonFollow,
                                                                   proto::Capability::WorkspaceMeta,
                                                                   proto::Capability::ClientGoodbye });
};

/// When a connection sends a session's program-status set.
enum class ProgramStatusPush : uint8_t
{
    Always = 0, ///< It changed: even an empty set is news, since it says the records are gone.
    IfAny,      ///< A snapshot: an empty set says nothing a fresh mirror does not already hold.
};

/// Decides where a session's clipboard write may land on a client, given what the connection shares.
///
/// A client sharing Capability::ClipboardTargets lands each write where it names. One predating it puts every
/// write on the clipboard, so it gets only a write that names the clipboard, narrowed to that; a write to
/// the primary selection alone would overwrite what its user copied, and is not sent at all.
/// @param targets Where the session's application asked the copy to land.
/// @param shared The capabilities both ends of the connection share.
/// @return The targets to send, or nothing when the write is not to be sent.
[[nodiscard]] std::optional<vtbackend::ClipboardTargets> forwardedClipboardTargets(
    vtbackend::ClipboardTargets targets, proto::CapabilitySet const& shared);

/// The collaborators and policy a native connection is constructed with.
///
/// Later phases append members, every one with a default, so a test or an embedded host names only
/// what it needs, and no phase adds a constructor parameter.
struct NativeSessionServices
{
    HandshakePolicy handshake = {}; ///< What the handshake admits.
    /// The daemon behind this endpoint: what a Control connection asks about, and what counts each Attach
    /// connection as a client. The null object, which serves nothing, for an endpoint without a daemon.
    std::reference_wrapper<DaemonControl> daemon = nullDaemonControl();
    FlushPolicy flush = {}; ///< When a pending screen increment is flushed: at once, or coalesced.
    /// The control API a Control connection that shares Capability::ControlApi calls, and that is told of
    /// every Attach connection coming and going. The null object, which serves nothing, by default.
    std::reference_wrapper<vtcontrol::ControlApi> control = vtcontrol::nullControlApi();
};

/// One connected native-protocol client.
class NativeSession final: public SessionStreamEvents, public UpgradeObserver
{
  public:
    /// The default send-queue bound (see the constructor).
    static constexpr std::size_t DefaultWriteQueueBytes = std::size_t { 4 } * 1024 * 1024;

    /// Where a connection is in its life, and so what it is for. The single stored state the handshake
    /// decides; what was agreed on along with it is negotiated().
    enum class ConnectionPhase : uint8_t
    {
        AwaitingHello = 0, ///< No ClientHello accepted yet: nothing but a hello is read.
        Viewing,           ///< An accepted Attach hello: a client that shows the host's workspaces.
        Controlling, ///< An accepted Control hello: calls the daemon and its control API, views nothing.
    };

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

    /// Stops counting this connection as the host's client (@see SessionHost::noteClientDetached).
    ~NativeSession() override;

    NativeSession(NativeSession const&) = delete;
    NativeSession& operator=(NativeSession const&) = delete;
    NativeSession(NativeSession&&) = delete;
    NativeSession& operator=(NativeSession&&) = delete;

    /// The connection flow: handshake, initial snapshot, then serve until the
    /// peer disconnects.
    [[nodiscard]] core::async::Task<void> run();

    /// Tells the peer the daemon is upgrading (DaemonUpgrading), when it negotiated Capability::LiveUpgrade.
    void announceUpgrade(std::string_view toRelease) override;

    /// Ends the connection in an orderly way: what is queued (DaemonUpgrading among it) is flushed first.
    void closeForUpgrade() override;
    void abortForUpgrade() override;

    /// Marks @p session changed and schedules its delta flush, at once or coalesced (an Attach
    /// connection subscribes itself to the host's stream fan-out as its hello is accepted).
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
    void sessionCopyToClipboard(vtworkspace::SessionId session,
                                std::string const& data,
                                vtbackend::ClipboardTargets targets) override;

    /// A session's program-status records changed: marked, and pushed whole by the next debounced flush to a
    /// viewer sharing Capability::ProgramStatus. Every other signal is the control service's.
    void sessionSignal(vtworkspace::SessionId session, SessionSignal const& signal) override;
    void raiseRoleChanged(proto::RaiseRole role) override;

    /// A workspace's git state changed: told to a following client as a WorkspaceMeta delta
    /// (@see sendWorkspaceGit).
    void workspaceGitChanged(WorkspaceGitChangedSignal const& signal) override;

    /// Tells the client of @p workspace's git state (WorkspaceMeta), if it follows the daemon (the sidebar is
    /// a following window's), shares Capability::WorkspaceMeta and was told the workspace already; one it was
    /// not told yet is told the state in full with it.
    /// @param workspace The daemon's workspace id.
    /// @param git Its git state now; none when it is in no repository (anymore), sent as
    ///        GitHeadKindWire::None.
    void sendWorkspaceGit(uint64_t workspace, std::optional<proto::WireGitInfo> const& git);

    /// Asks this client's user, whose GUI shows the prompt in @p session's pane. Sent as the question opens;
    /// a client attaching later is asked from SessionHost::pendingPermissionRequests() instead.
    void sessionPermissionRequested(vtworkspace::SessionId session,
                                    proto::PermissionRequest const& request) override;

    /// Adapts the host's model-change fan-out into a "layout changed" callback naming the workspace or the
    /// tab the change is in: every structural change re-pushes the whole LayoutState of a workspace. It is a
    /// `vtworkspace::ModelEvents` an Attach connection subscribes to the host as its hello is accepted, so
    /// the model stays transport-agnostic.
    struct LayoutObserver final: vtworkspace::ModelEvents
    {
        std::function<void(vtworkspace::WorkspaceId)>
            onWorkspaceChange;                               ///< A change in one workspace's tab strip.
        std::function<void(vtworkspace::TabId)> onTabChange; ///< A change within one tab.
        std::function<void()> onWorkspaceOrderChange;        ///< A workspace moved in the workspace order.
        void workspaceMoved(vtworkspace::WorkspaceId, int, int) override { onWorkspaceOrderChange(); }
        /// A removed workspace is pushed emptied, which is how a client learns it is gone: one that held no
        /// tab any more fired no tab event to say so.
        void workspaceRemoved(vtworkspace::WorkspaceId workspace, int) override
        {
            onWorkspaceChange(workspace);
            onWorkspaceOrderChange();
        }
        void tabAdded(vtworkspace::WorkspaceId workspace, vtworkspace::TabId, int) override
        {
            onWorkspaceChange(workspace);
        }
        void tabClosed(vtworkspace::WorkspaceId workspace, vtworkspace::TabId, int) override
        {
            onWorkspaceChange(workspace);
        }
        void tabMoved(vtworkspace::WorkspaceId workspace, vtworkspace::TabId, int, int) override
        {
            onWorkspaceChange(workspace);
        }
        void tabMovedToWorkspace(
            vtworkspace::WorkspaceId from, vtworkspace::TabId, int, vtworkspace::WorkspaceId to, int) override
        {
            onWorkspaceChange(from);
            onWorkspaceChange(to);
        }
        void activeTabChanged(vtworkspace::WorkspaceId workspace, vtworkspace::TabId, int) override
        {
            onWorkspaceChange(workspace);
        }
        void paneSplit(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId) override
        {
            onTabChange(tab);
        }
        void paneClosed(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId) override
        {
            onTabChange(tab);
        }
        void activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId) override { onTabChange(tab); }
        void paneRatioChanged(vtworkspace::TabId tab, vtworkspace::PaneId, double) override
        {
            onTabChange(tab);
        }
        void paneOrientationChanged(vtworkspace::TabId tab,
                                    vtworkspace::PaneId,
                                    vtworkspace::SplitState) override
        {
            onTabChange(tab);
        }
        void paneSwapped(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId) override
        {
            onTabChange(tab);
        }
        void paneZoomChanged(vtworkspace::TabId tab, std::optional<vtworkspace::PaneId>) override
        {
            onTabChange(tab);
        }
        void paneTreeRestructured(vtworkspace::TabId tab) override { onTabChange(tab); }
        void tabTitleChanged(vtworkspace::TabId tab) override { onTabChange(tab); }
        void tabColorChanged(vtworkspace::TabId tab) override { onTabChange(tab); }
    };

    /// @param type A PDU type this connection may send.
    /// @return Whether the client answered it with Unsupported, so it is no longer sent.
    [[nodiscard]] bool peerRefused(proto::PduType type) const noexcept
    {
        return _unsupported.peerRefused(type);
    }

    /// @return What the handshake agreed on; nullopt until a ClientHello was accepted.
    [[nodiscard]] std::optional<proto::Negotiated> const& negotiated() const noexcept { return _negotiated; }

    /// @return Where the connection is in its life.
    [[nodiscard]] ConnectionPhase phase() const noexcept { return _phase; }

    /// @return What the hello's token granted: every permission over a socket that takes any hello, those of
    ///         the matched token otherwise; none before a hello is accepted.
    [[nodiscard]] vtcontrol::ControlGrants grants() const noexcept { return _grants; }

    /// @return The one workspace this connection mirrors: its sessions' screens and events. The attach
    ///         plan's, then whatever SelectWorkspace chose (or the daemon, when the selected one ended); 0
    ///         before the attach and when none could be.
    [[nodiscard]] uint64_t selectedWorkspace() const noexcept override { return _selectedWorkspace; }

    /// @return Whether the connection shares Capability::DaemonFollow.
    [[nodiscard]] bool followsDaemon() const noexcept override { return summarizesPanes(); }

    /// @param session A hosted session.
    /// @return Whether @p session sits in the workspace this connection selects; false for one in no
    ///         workspace.
    [[nodiscard]] bool mirrorsSession(vtworkspace::SessionId session) const;

    /// @return This connection's id.
    [[nodiscard]] ConnectionId connectionId() const override { return _id; }

    /// @return Whether the client offered Capability::WorkspaceSwitch, so can be sent a SwitchRequest.
    [[nodiscard]] bool canSwitch() const noexcept override
    {
        return _negotiated.has_value() && _negotiated->shared.contains(proto::Capability::WorkspaceSwitch);
    }

    /// Sends @p request to the client. @see SessionStreamEvents::requestSwitch
    void requestSwitch(proto::SwitchRequest const& request) override
    {
        send(0, proto::DecodedPdu { request });
    }

  private:
    friend struct NativeSessionFollowTester; ///< Test-only view of _followed.

    /// Makes @p workspace the one this connection mirrors: tells the host, forgets the sessions of the
    /// workspace left and queues a snapshot of every session of the new one. A workspace already selected
    /// only tells the host; one the model does not hold changes nothing.
    /// @param workspace The daemon workspace.
    void selectWorkspace(vtworkspace::WorkspaceId workspace);

    /// Requests a snapshot of every session @p workspace shows.
    /// @param workspace The workspace.
    void requestWorkspaceSnapshots(vtworkspace::Workspace& workspace);

    /// Sends the layout of @p workspace and, when it is the selected one, has every session it shows not yet
    /// mirrored to this client streamed after it (@see requestArrivedSnapshots).
    /// @param workspace The workspace.
    void sendWorkspaceLayout(vtworkspace::Workspace& workspace);

    /// Requests a snapshot of every session @p workspace shows that a move brought in from a workspace
    /// another client shows: one not yet mirrored to this client, already in the model as its last layout
    /// batch ended, and with no snapshot pending. Its shell may say nothing more for a long time. A session
    /// spawned since is mirrored as its first output comes, as before.
    /// @param workspace The selected workspace.
    void requestArrivedSnapshots(vtworkspace::Workspace& workspace);

    /// Records every session the host's model shows now (@ref _sessionsAtLastLayout).
    void noteModelSessions();

    /// Forgets every session mirrored to this client that the selected workspace does not show any more: one
    /// a move took to another workspace, or one left behind by a selection. One that comes back is then
    /// mirrored anew, from a snapshot, and not by increments against what this client was sent before it
    /// left.
    void forgetUnmirroredSessions();

    /// Drops every per-session state this connection keeps for @p session.
    /// @param session The session.
    void forgetSession(vtworkspace::SessionId session);

    friend struct NativeSessionFlushTester; ///< Test-only view of the armed flush deadline.

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
        /// The OSC 30 tab name last sent to this connection (empty: none), diffed like the title.
        std::string lastTabName;
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
        /// The input stamp (SessionHost::lastInputAt) this connection already flushed an echo for: one
        /// immediate flush per input, however many screen updates its output raises.
        std::optional<core::platform::SteadyTimePoint> answeredInput;
        /// Whether an increment went out since this follow's last snapshot: the first one flushes at once.
        IncrementHistory increments = IncrementHistory::NoneSinceSnapshot;
        /// What `increments` becomes when the snapshot queued for this follow is built. Attach, resize and a
        /// follow's first push leave it at NoneSinceSnapshot, so the first increment after them flushes at
        /// once; a resync
        /// forced by a batch the increment could not describe sets it to SomeSinceSnapshot, because a flood
        /// that keeps resyncing must coalesce rather than flush after every snapshot.
        IncrementHistory historyAfterSnapshot = IncrementHistory::NoneSinceSnapshot;

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

    /// A viewer's keys for one of its sessions: written through the host, and the session's echo read again
    /// at the next flush. Loop thread.
    /// @param input The PDU.
    void handleInput(proto::Input const& input);

    /// Whether a frame was a request to the daemon itself -- daemon control, or the control API -- which is
    /// served (or refused) before the frame would be dispatched on.
    enum class DaemonRequest : uint8_t
    {
        None = 0,   ///< It was not one: the frame is dispatched on.
        Served = 1, ///< It was, and it has been answered or carried out.
    };

    /// Serves @p frame if it is a DaemonStatusRequest or a StopDaemon, through NativeSessionServices::daemon.
    /// What the daemon does not serve, a stop for a reason this version does not know, and a request the
    /// token does not grant (`read` for the status, `write` for a stop) is answered Unsupported.
    /// @param frame A frame received after the handshake.
    /// @return Whether it was such a request.
    [[nodiscard]] DaemonRequest serveDaemonRequest(proto::DecodedFrame const& frame);

    /// The link between a connection and the upgrade request it spawned. The request's flow outlives neither
    /// the daemon's upgrade nor, as a claim on the connection would make it, the connection's own end: the
    /// requester's connection is among those the upgrade closes, and run() would wait for the very flow
    /// that waits for the connection to go. So the flow holds this instead, and finds the connection gone
    /// when @ref session is null.
    struct UpgradeLink
    {
        NativeSession* session = nullptr; ///< The requesting connection; null once it is closed.
    };

    /// Answers @p serial with @p reply through @p link's connection, if it is still there.
    static void replyThroughLink(UpgradeLink const& link, uint64_t serial, proto::UpgradeDaemonReply reply);

    /// Asks @p daemon to upgrade to @p target and answers @p serial through @p link: started just before the
    /// announcement, otherwise why not. A free-standing flow, since it outlives no connection.
    /// @param daemon The daemon (not owned; outlives every connection).
    /// @param link The requesting connection.
    /// @param serial The UpgradeDaemon's serial.
    /// @param target What to upgrade to.
    [[nodiscard]] static core::async::Task<void> serveUpgradeRequest(DaemonControl* daemon,
                                                                     std::shared_ptr<UpgradeLink> link,
                                                                     uint64_t serial,
                                                                     handover::UpgradeTarget target);

    /// Whether the connection is being ended for a live upgrade.
    enum class UpgradeEnd : uint8_t
    {
        Open = 0, ///< It is not.
        Ending,   ///< closeForUpgrade() ran: the pump reads no more.
    };

    /// Whether a frame was a verb that changes how a tab is presented or where its panes are.
    enum class TabVerb : uint8_t
    {
        None = 0,   ///< It was not one: the frame is dispatched on.
        Served = 1, ///< It was, and it has been carried out or refused.
    };

    /// Serves @p frame if it is an EditTab (gated on Capability::TabEdits), a MergeTabIntoSplit (gated on
    /// Capability::TabMerge) a MoveNode or a SetSplitOrientation (both gated on Capability::MoveNode):
    /// applied to the daemon's model, whose events push the new layout to every client, and answered
    /// Unsupported when the capability was not negotiated.
    /// @param frame A frame received after the handshake.
    /// @return Whether it was such a verb.
    [[nodiscard]] TabVerb serveTabVerb(proto::DecodedFrame const& frame);

    /// Serves @p frame if it is a SelectWorkspace or ReportFocus (Capability::DaemonFollow; ReportFocus is
    /// read and ignored), a SwitchReply (Capability::WorkspaceSwitch) or a Goodbye
    /// (Capability::ClientGoodbye), refusing it when the capability was not negotiated. Reported as a
    /// TabVerb: it is dispatched the same way.
    /// @param frame The frame read.
    /// @return Served when it was one of them.
    [[nodiscard]] TabVerb serveWorkspaceVerb(proto::DecodedFrame const& frame);

    /// What requireGrant() decided.
    enum class GrantCheck : uint8_t
    {
        Refused = 0, ///< The connection lacks the permission; the frame was answered Unsupported.
        Granted,     ///< Serve the frame.
    };

    /// Refuses @p frame unless the connection holds @p permission: answered Unsupported, and why logged.
    /// @param permission What serving @p frame needs.
    /// @param frame The request.
    /// @return Whether it was refused or is to be served.
    [[nodiscard]] GrantCheck requireGrant(vtcontrol::Permission permission, proto::DecodedFrame const& frame);

    /// Serves @p frame if it is a ControlRequest: through the control API on a Controlling connection that
    /// shares Capability::ControlApi, else answered Unsupported.
    /// @param frame A frame received after the handshake.
    /// @return Whether it was a ControlRequest.
    [[nodiscard]] DaemonRequest serveControlApiRequest(proto::DecodedFrame const& frame);

    /// Serves one call of the control API through NativeSessionServices::control, and answers it with a
    /// ControlReply under @p serial once the call completes, at once or later, unless the connection closed
    /// meanwhile.
    /// @param serial The request's serial.
    /// @param request The call.
    void serveControlRequest(uint64_t serial, proto::ControlRequest const& request);

    /// Sends one event of a subscription, or counts it dropped while the send backlog is past half its bound:
    /// a subscriber that does not read loses events, never the connection, and is told how many it lost as
    /// the next event that fits arrives.
    /// @param subscription The subscription.
    /// @param event The event.
    void deliverControlEvent(uint32_t subscription, vtcontrol::Event const& event);

    /// The connection as the control API's subscriptions and waits know it.
    struct ControlEventSink final: vtcontrol::EventSink
    {
        explicit ControlEventSink(NativeSession& owner): session(owner) {}
        void deliverEvent(uint32_t subscription, vtcontrol::Event const& event) override
        {
            session.deliverControlEvent(subscription, event);
        }
        void subscriptionEnded(uint32_t subscription) override { session._droppedEvents.erase(subscription); }
        NativeSession& session; ///< The connection.
    };

    /// Answers @p frame with an Unsupported naming its serial and tag: a PDU this connection knows but cannot
    /// serve.
    /// @param frame The frame refused.
    void refuse(proto::DecodedFrame const& frame);

    /// Encodes @p pdu and hands it to the write queue, disconnecting on refusal. A PDU the client answered
    /// with Unsupported is not sent at all.
    /// @param serial The request serial to answer, or 0 for an unsolicited push.
    /// @param pdu What to send.
    /// @param sessionTag The session this frame describes, or 0 when it describes none
    ///        (hello, layout). A later snapshot for the same session supersedes the
    ///        tagged ones still unwritten — @see core::net::WriteQueue::dropTagged.
    void send(uint64_t serial, proto::DecodedPdu const& pdu, uint64_t sessionTag = 0);

    /// The pre-authentication deadline passed with no hello accepted: ends the connection.
    void preAuthExpired();

    /// Sends an AuthChallenge with a fresh nonce, which the next key proof must cover.
    /// @param keyAuth This connection's key authentication.
    /// @param serial The serial to answer, or 0 for the opening challenge.
    void sendKeyChallenge(KeyAuthPolicy const& keyAuth, uint64_t serial);

    /// Checks a key proof against the outstanding challenge and this connection's channel binding.
    /// @param keyAuth This connection's key authentication.
    /// @param proof What the client sent.
    /// @return The grants of the listed key whose signature verifies; nothing otherwise.
    [[nodiscard]] std::optional<vtcontrol::ControlGrants> verifyKeyProof(KeyAuthPolicy const& keyAuth,
                                                                         proto::KeyProof const& proof) const;

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

    /// Serializes each followed workspace's tab/pane tree into a LayoutState and pushes it
    /// (serial 0), once the handshake completed — on attach, and on every change to a client not sharing
    /// Capability::LayoutBatch. To a client sharing it the push is ended by a LayoutBatchEnd (@see
    /// endLayoutBatch).
    void pushLayout();

    /// Sends @p layout (serial 0), stamped with the last EditTab served to a client sharing
    /// Capability::TabEditAcks.
    void sendLayout(proto::LayoutState layout);

    /// @return Whether this client shares Capability::LayoutBatch: its pushes are ended, and coalesced.
    [[nodiscard]] bool batchesLayouts() const noexcept;

    /// The model changed in @p workspace. A client sharing Capability::LayoutBatch has the workspace marked,
    /// and pushed once when the batch ends (@see flushChangedLayouts); any other is pushed every workspace's
    /// layout now, as a client of the release before it always was.
    /// @param workspace The workspace the change is in.
    void layoutChanged(vtworkspace::WorkspaceId workspace);

    /// Pushes the layout of each workspace changed since the batch opened (@see layoutChanged), each once, as
    /// it is now; a workspace the batch removed is pushed with no tab, which closes the client's own
    /// workspace for it. Settles first the selection of a connection whose workspace the batch removed.
    /// @return Whether any LayoutState went out.
    bool flushChangedLayouts();

    /// Opens a layout batch if none is: spawns the endLayoutBatch() that ends it.
    void openLayoutBatch();

    /// Selects another workspace when the selected one ended (left the model, or lost its last tab), before
    /// a layout push: the one a launch takes (@see planAttach), so never one another window shows; none when
    /// none is left. A connection that selected none stays so.
    void refreshSelection();

    /// @return The daemon's workspaces ascending by id, with their tab counts and what an attach sees of them
    ///         (SessionHost::shownForAttach), as of now.
    [[nodiscard]] std::vector<WorkspaceSummary> workspaceSummaries() const;

    /// @param workingDirectory The directory the attach states; empty when it states none.
    /// @return What planAttach() decides from: workspaceSummaries() and the host's primary workspace.
    [[nodiscard]] AttachContext attachContext(std::filesystem::path const& workingDirectory) const;

    /// Validates the peer's ClientHello and answers it. An Attach connection then becomes a client
    /// (see attachClient: the census ticket, the subscriptions, and the attach snapshot, spawning the
    /// first session on an empty daemon); a Control connection gets the ServerHello and nothing else.
    /// @return False when the hello was missing or version-mismatched.
    [[nodiscard]] bool completeHandshake(proto::DecodedFrame const& frame);

    /// What an accepted Attach hello starts: the census ticket and the host's subscriptions, the client's
    /// settings and client area, the attach plan and what it creates, then the ServerHello naming the
    /// followed workspaces, their layout, their sessions' snapshots and the open permission questions.
    ///
    /// The ServerHello waits for the plan because it states what the plan follows; nothing reaches the
    /// peer before it, as @ref _negotiated is engaged only once it is sent.
    /// @param serial The hello's serial, which the ServerHello answers.
    /// @param hello The accepted hello.
    /// @param reply The ServerHello, without its followed workspaces.
    /// @param negotiated What the handshake agreed on.
    void attachClient(uint64_t serial,
                      proto::ClientHello const& hello,
                      proto::ServerHello reply,
                      proto::Negotiated negotiated);

    /// Decides which workspaces this connection follows and creates what that asks for: a first tab in the
    /// primary workspace, a workspace of its own, or a tab in the first workspace followed. Counts the
    /// connection as the host's client after deciding, so that it is not among the clients it is decided
    /// against.
    /// @param intent What the hello asked for; AllWorkspaces when Capability::AttachIntent is not shared.
    /// @param workingDirectory The absolute directory the session the attach creates starts in; empty for
    ///        its profile's.
    void executeAttachPlan(proto::AttachIntent const& intent, std::filesystem::path const& workingDirectory);

    /// Bounds the cell size a client states alongside a grid (vthost::sanePixelSize); an out-of-range one is
    /// logged and dropped, and the grid still applies.
    /// @param stated The cell size the PDU carries; nullopt when it carries none.
    /// @param origin What carried it, for the log ("hello", "client", "pane").
    /// @return The bounded cell size, or nothing when none was stated or it was out of range.
    [[nodiscard]] std::optional<vtpty::ImageSize> statedCellPixelSize(
        std::optional<proto::WireCellPixelSize> const& stated, std::string_view origin) const;

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

    /// A claim a flow this connection spawned holds on `this`, which run() waits out before it lets `this`
    /// go (@see spawnedFlowsEnded). Given back exactly once, when the frame holding it lets go of it.
    ///
    /// Taken as a coroutine PARAMETER, and moved into a local as the body's first statement. The local gives
    /// it back as the body ends, however it ends; the parameter gives it back when the loop destroys a frame
    /// it never resumed -- parameters live in the frame from the call on, a body's locals only once it runs.
    class SpawnedFlowClaim
    {
      public:
        /// Undoes what the claim stands for on @p session; it runs in a destructor.
        using Release = void (*)(NativeSession& session) noexcept;

        /// @param session The connection claimed; it outlives the claim (run() waits for that).
        /// @param release What gives the claim back.
        SpawnedFlowClaim(NativeSession& session, Release release) noexcept:
            _session(&session), _release(release)
        {
        }
        SpawnedFlowClaim(SpawnedFlowClaim&& other) noexcept:
            _session(std::exchange(other._session, nullptr)), _release(other._release)
        {
        }
        SpawnedFlowClaim(SpawnedFlowClaim const&) = delete;
        SpawnedFlowClaim& operator=(SpawnedFlowClaim const&) = delete;
        SpawnedFlowClaim& operator=(SpawnedFlowClaim&&) = delete;
        /// Gives the claim back, unless it moved on, and wakes run()'s wait once it was the last one.
        ~SpawnedFlowClaim();

      private:
        NativeSession* _session; ///< The connection claimed; null once moved from.
        Release _release;        ///< What gives the claim back.
    };

    /// What run() awaits before it lets `this` go: ready once spawnedFlowsEnded(), else parked until the
    /// last SpawnedFlowClaim is given back. Defined in the source file.
    struct SpawnedFlowsEnded;

    /// Whether a layout push to a client sharing Capability::LayoutBatch still owes its LayoutBatchEnd.
    enum class LayoutBatch : uint8_t
    {
        Closed = 0, ///< Nothing pushed since the last end: the next push or change spawns endLayoutBatch().
        Open = 1,   ///< An endLayoutBatch() is spawned, and ends every push and change made before it runs.
        Pushed = 2, ///< As Open, and a LayoutState went out in it already, so its end is owed.
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

    /// Whether this connection is a viewer that mirrors program status (shares Capability::ProgramStatus).
    [[nodiscard]] bool mirrorsProgramStatus() const noexcept;

    /// Whether this connection is told what changed in the panes it does not mirror (its client shares
    /// Capability::DaemonFollow).
    [[nodiscard]] bool summarizesPanes() const noexcept;

    /// Whether this connection is told each workspace's label, control id and git state (its client shares
    /// Capability::WorkspaceMeta).
    [[nodiscard]] bool sharesWorkspaceMeta() const noexcept;

    /// Sends @p workspace's WorkspaceMeta in full, once: the first time its layout goes to this connection
    /// with a tab. A layout with none forgets it was sent, so the workspace is announced again once it holds
    /// a tab.
    /// @param workspace A workspace whose layout was just sent.
    void announceWorkspaceMeta(vtworkspace::Workspace const& workspace);

    /// Forgets the workspaces the model no longer holds, so a WorkspaceMeta delta never names one.
    void forgetRemovedWorkspaceMeta();

    /// Tells the client the position (WorkspaceMeta::position) of every announced workspace whose place in
    /// the daemon's workspace order differs from the one it was last told: after a reorder, and after a
    /// workspace was added or removed ahead of others. A client that does not share Capability::WorkspaceMeta
    /// is told nothing.
    void syncWorkspacePositions();

    /// @return @p workspace's 0-based place in the daemon's workspace order; 0 for one the model lacks.
    [[nodiscard]] int positionOf(vtworkspace::WorkspaceId workspace) const;

    /// Tells the client of a label the registry changed: a workspace's as a label delta, where the client
    /// shares Capability::WorkspaceMeta; a tab's with its workspace's layout; a pane's not at all.
    /// @param ref The object whose label changed.
    /// @param label Its label now; empty when it was cleared.
    void labelChanged(vtcontrol::ObjectRef const& ref, std::string_view label);

    /// Sends a label delta when @p ref is a workspace this connection was told of.
    /// @param ref The object whose label changed.
    /// @param label Its label now; empty when it was cleared.
    void workspaceLabelChanged(vtcontrol::ObjectRef const& ref, std::string_view label);

    /// @param workspace A workspace of the model.
    /// @return Its control id as WorkspaceMeta carries it; all zero for one the registry does not bind.
    [[nodiscard]] std::array<uint8_t, 16> controlIdOf(vtworkspace::WorkspaceId workspace) const;
    /// @param session A hosted session.
    /// @return Whether a bell or notification of @p session reaches this connection in the pane's summary
    ///         rather than as a SessionBell or SessionNotify: it summarizes panes and does not mirror this
    ///         one.
    [[nodiscard]] bool summarizesRaise(vtworkspace::SessionId session) const;

    /// Queues the status of an unmirrored pane for the next flush.
    /// @param session The pane's session.
    /// @param note Adds what changed to the pane's pending summary.
    template <std::invocable<proto::PaneSummary&> Note>
    void queueSummary(vtworkspace::SessionId session, Note&& note);

    /// Notes that an unmirrored @p session printed, at most once per FlushPolicy::activityMarkInterval.
    void noteActivity(vtworkspace::SessionId session);

    /// Sends the pending summaries, and forgets them.
    void flushSummaries();

    /// Queues each unmirrored pane's title and working directory now, and any program status it holds, for
    /// the next flush: what a window starts a pane's row from, as it attaches or selects another workspace.
    void announceUnmirroredPanes();

    /// @return Whether @p session has program-status records to tell.
    [[nodiscard]] bool holdsProgramStatus(vtworkspace::SessionId session) const;

    /// Sends @p session's whole program-status set, read under its terminal lock.
    /// @param session The session.
    /// @param when Whether an empty set is news (a change emptied it) or not worth a PDU (a snapshot).
    void pushProgramStatus(vtworkspace::SessionId session, ProgramStatusPush when);

    /// Whether this connection mirrors soft fonts (shares Capability::SoftFonts): it is sent each session's
    /// fonts, and the soft-glyph cells beside the rows.
    [[nodiscard]] bool mirrorsSoftFonts() const noexcept;

    /// Sends @p session's soft fonts when they differ from what this connection was last told, read under
    /// the terminal lock. Called ahead of every delta and snapshot, so a font reaches the receiver before
    /// the cells that use it. A Delta can still name a font the client has not received -- a font loaded
    /// after the push, whose next push is still to come -- and such a cell renders as its text until then.
    /// @param session The session.
    void pushSoftFonts(vtworkspace::SessionId session);

    /// Whether this connection is a viewer that mirrors the echo state (shares Capability::InputEcho).
    [[nodiscard]] bool mirrorsInputEcho() const noexcept;

    /// Sends what @p session's terminal does with typed text, when it differs from what this connection was
    /// last told. Read from the hosted PTY on the loop thread, never under the terminal lock.
    /// @param session The session.
    void pushInputEcho(vtworkspace::SessionId session);

    /// The idle check of the echo state (FlushPolicy::inputEchoCheck): once it is due, asks every followed
    /// session's PTY for its echo, so a change with no output or input after it still reaches the client; and
    /// arms the flush that makes the next one. Loop thread, from the flusher. A connection that does not
    /// mirror the echo state does nothing.
    void checkInputEchoWhenDue();

    /// Opens (or extends) the window in which the idle check keeps itself armed
    /// (FlushPolicy::inputEchoWindow): output of a followed session, an input, or the attach itself happened
    /// just now.
    void noteInputEchoActivity();

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
    /// workspace while the send queue has room, else once it has room again — parked on its watermark by
    /// @ref requeueOnSendRoom, rather than rebuilding the session every workspace to find none.
    /// @param session The session whose records are owed.
    void followUpRecords(vtworkspace::SessionId session);

    /// The one waiter for room for command-block records: parks until the send queue's backlog is under
    /// a piece, then queues an increment for every session in @ref _recordsAwaitingRoom.
    /// @param claim The waiter's claim on `this` (@see claimRoomWaiter).
    [[nodiscard]] core::async::Task<void> requeueOnSendRoom(SpawnedFlowClaim claim);

    /// Arms a delta flush due at @p deadline, unless one due no later is already armed or the connection
    /// is closing. An earlier deadline supersedes the armed one: its flusher wakes to find it moved, and
    /// returns.
    /// @param deadline When the flush is due.
    void scheduleFlush(core::platform::SteadyTimePoint deadline);

    /// Marks @p session as owed an increment and arms its flush: what a screen update does, and what a
    /// push that left command-block records unsent has done for it. At once for the echo of an input or
    /// the first increment after a snapshot, else after the coalescing delay (@see decideFlush).
    /// @param session The session to push in the next flush.
    void queueIncrement(vtworkspace::SessionId session);

    /// Why @p session's increment is flushed, and so how soon; spends @p session's echo credit when the
    /// answer is FlushReason::Echo.
    /// @param session The session owed an increment.
    /// @param now The loop clock's current time.
    /// @return Coalesce while the session's snapshot is pending or before its first push, which a snapshot
    ///         describes, else what decideFlush says of its follow.
    [[nodiscard]] FlushReason flushReasonFor(vtworkspace::SessionId session,
                                             core::platform::SteadyTimePoint now);

    /// The single snapshot streamer: drains @ref _snapshotQueue, one session at a time, waiting
    /// for room in the send queue before each piece. One streamer rather than one coroutine per
    /// snapshot, so two runs for the same session can never interleave on the wire.
    /// @param claim The streamer's claim on `this` (@see claimStreamer).
    [[nodiscard]] core::async::Task<void> streamSnapshots(SpawnedFlowClaim claim);

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
    /// One spelling for all four creation paths (the attach-time first session, CreateTab, SelectWorkspace 0,
    /// SplitPane), so a fifth cannot quietly forget the client's preference.
    /// @param verbProfile The profile the spawning verb names; empty for the connection's. Honoured only
    ///        with Capability::ProfileByName shared.
    /// @param directoryFrom The session the new one is opened from (a tab or split made from that pane): its
    ///        shell starts in that pane's working directory, as an in-process tab inherits it
    ///        (@see SessionSpawnRequest::directoryFrom).
    [[nodiscard]] SessionSpawnRequest spawnRequest(
        std::string_view verbProfile,
        std::optional<vtworkspace::SessionId> directoryFrom = std::nullopt) const
    {
        auto const verbNamesProfile = !verbProfile.empty() && _negotiated.has_value()
                                      && _negotiated->shared.contains(proto::Capability::ProfileByName);
        return SessionSpawnRequest { .settings = _clientSessionSettings,
                                     .profileName =
                                         verbNamesProfile ? std::string { verbProfile } : _profileName,
                                     .directoryFrom = directoryFrom };
    }

    /// What the session the attach itself creates is spawned with: @ref spawnRequest, its shell started in
    /// the directory the hello stated, when it stated one. The directory is the attach's alone -- a tab
    /// the client opens later starts where its profile says.
    /// @param workingDirectory The absolute directory the hello stated; empty for the profile's.
    [[nodiscard]] SessionSpawnRequest attachSpawnRequest(std::filesystem::path const& workingDirectory) const
    {
        auto request = spawnRequest({});
        if (!workingDirectory.empty())
            startIn(request, workingDirectory);
        return request;
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

    /// Tells whether this client is asked @p request: it is asked permission questions, and, for one that
    /// asks for control permissions, both ends offered proto::Capability::ControlPermissions -- a client
    /// that would prompt without them is not asked, and the question waits for another.
    /// @return true if @p request is to be sent.
    [[nodiscard]] bool asks(proto::PermissionRequest const& request) const noexcept
    {
        return asksPermissions()
               && (request.control.empty()
                   || _negotiated->shared.contains(proto::Capability::ControlPermissions));
    }

    /// Flushes every pending increment at @p deadline, unless an earlier deadline superseded it by then.
    /// @param claim The flush's claim on `this` (@see claimFlush).
    /// @param deadline The deadline scheduleFlush() armed it for.
    [[nodiscard]] core::async::Task<void> flushAt(SpawnedFlowClaim claim,
                                                  core::platform::SteadyTimePoint deadline);

    /// Pushes the workspaces the batch changed (@see flushChangedLayouts) and sends the LayoutBatchEnd of the
    /// pushes made since it was spawned: a flow, so that it runs once the change that pushed has run to its
    /// end. One change fires several model events -- a pane moved out of its tab is announced shrunk before
    /// the workspace it entered shows it -- and each workspace is pushed once for all of them, as the change
    /// left it. The end is sent when anything was pushed, and for a move of this client's even when it pushed
    /// nothing this client follows, so the move is known to have landed.
    /// @param claim The batch's claim on `this` (@see claimLayoutBatch).
    [[nodiscard]] core::async::Task<void> endLayoutBatch(SpawnedFlowClaim claim);

    /// @return A claim counted in @ref _flushesInFlight until a flushAt() frame gives it back.
    [[nodiscard]] SpawnedFlowClaim claimFlush();
    /// @param session The session the streamer starts on.
    /// @return A claim that holds @ref _streamingSession set until a streamSnapshots() frame gives it back.
    [[nodiscard]] SpawnedFlowClaim claimStreamer(vtworkspace::SessionId session);
    /// @return A claim that holds @ref _roomWaiter Parked until a requeueOnSendRoom() frame gives it back.
    [[nodiscard]] SpawnedFlowClaim claimRoomWaiter();
    /// @return A claim that holds @ref _layoutBatch Open until an endLayoutBatch() frame gives it back.
    [[nodiscard]] SpawnedFlowClaim claimLayoutBatch();

    /// @return Whether no flow this connection spawned holds `this` any more: no flush, no snapshot
    ///         streamer, no waiter for send room, no layout batch left to end.
    [[nodiscard]] bool spawnedFlowsEnded() const noexcept;

    core::net::EventLoop& _loop;
    SessionHost& _host;
    std::unique_ptr<core::net::ISocket> _connection;
    core::net::WriteQueue _writer;
    /// What a snapshot is cut into, and the backlog each piece waits to get under before it is
    /// enqueued: @ref snapshotPieceBytes of this connection's bound.
    std::size_t _snapshotPieceBytes;
    ConnectionId _id;                ///< Prefixes this connection's every log line.
    NativeSessionServices _services; ///< The handshake's policy, the daemon, and later phases' collaborators.
    /// Where the connection is in its life: decided by the accepted hello's role. @see phase
    ConnectionPhase _phase = ConnectionPhase::AwaitingHello;
    vtcontrol::ControlGrants _grants = vtcontrol::ControlGrants::none(); ///< @see grants()
    std::array<std::byte, 32> _keyNonce = {}; ///< The nonce of the AuthChallenge now outstanding.
    std::size_t _keyAttempts = 0;             ///< Key proofs refused so far on this connection.
    /// What the handshake agreed on, the shared capabilities among it; engaged once a ClientHello was
    /// accepted.
    std::optional<proto::Negotiated> _negotiated;
    /// The serial of the last EditTab this connection served; 0 before one. Stamped on every LayoutState
    /// pushed to a client sharing Capability::TabEditAcks (proto::LayoutState::servedEdit).
    uint64_t _servedEdit = 0;
    /// The send-queue bound, half of which is where control events start to be dropped.
    std::size_t _maxWriteQueueBytes;
    /// Where this connection's control events are delivered. @see deliverControlEvent
    ControlEventSink _controlEvents { *this };
    /// The events dropped per subscription since its last one sent, erased as the subscription ends; at most
    /// one entry per subscription, which ControlServiceConfig caps. @see deliverControlEvent
    std::unordered_map<uint32_t, uint32_t> _droppedEvents;
    /// The emulation settings this client asked the sessions IT creates to have, layered onto the
    /// host's own in completeHandshake; nullopt when the client stated no preference.
    ///
    /// Per CONNECTION rather than per daemon, and applied only at creation: that is what lets two
    /// clients on different profiles share a daemon without either re-emulating the other's
    /// sessions. An application that already read DA1 cannot be told its terminal changed identity.
    std::optional<vtbackend::Settings> _clientSessionSettings;
    /// The host profile this client's sessions run under, as its hello named it with
    /// Capability::ProfileByName shared; empty for none, which keeps @ref _clientSessionSettings' path.
    std::string _profileName;
    /// The workspace this connection mirrors; 0 for none. @see selectedWorkspace
    uint64_t _selectedWorkspace = 0;
    /// Whether the selected workspace was seen holding a tab: only one that did can end by losing them.
    enum class SelectedTabs : uint8_t
    {
        NeverHeld = 0, ///< It has held none since selected (an empty workspace made for a first tab).
        Held,          ///< It held at least one.
    };
    SelectedTabs _selectedTabs = SelectedTabs::NeverHeld;
    /// Every session the host's model showed as this connection's last layout batch ended, in any workspace:
    /// a session of a followed workspace that is among them and is not mirrored to this client yet came in by
    /// a move (@see requestArrivedSnapshots), and one that is not was spawned since.
    std::unordered_set<uint64_t> _sessionsAtLastLayout;
    LayoutObserver _layoutObserver;
    /// Answers the client's unknown PDUs, and holds back the ones it said it does not know.
    proto::UnsupportedTracker _unsupported;
    std::unordered_map<uint64_t, FollowState> _followed;
    std::unordered_set<uint64_t> _pendingSessions;
    /// Sessions whose program-status records changed since the last flush. @see sessionSignal
    std::unordered_set<uint64_t> _programStatusPending;
    /// What changed in each unmirrored pane since the last flush, merged into one summary per pane.
    std::map<uint64_t, proto::PaneSummary> _pendingSummaries;
    /// When each unmirrored pane last had its output marked, which paces the next mark.
    std::unordered_map<uint64_t, core::platform::SteadyTimePoint> _activityMarkedAt;
    /// The workspaces whose WorkspaceMeta this connection was sent in full, each with the position the
    /// connection was last told for it; a delta names only these. @see announceWorkspaceMeta
    std::unordered_map<uint64_t, uint32_t> _announcedMeta;
    /// The role the daemon gave this window. @see raiseRoleChanged
    proto::RaiseRole _raiseRole = proto::RaiseRole::Quiet;
    /// Sessions whose echo state is to be read at the next flush, beside those with new output: one that took
    /// typed input, whose program may have turned echo back on without printing. @see pushInputEcho
    std::unordered_set<uint64_t> _inputEchoPending;
    /// What this connection was last told of each session's echo, as its wire byte; absent is Unknown, which
    /// a client assumes before it is told anything.
    std::unordered_map<uint64_t, uint8_t> _inputEchoSent;
    /// Per session, the soft font store generation this connection was last sent (absent: none yet, which
    /// reads as generation 0, the generation of a store nothing was ever loaded into).
    std::unordered_map<uint64_t, uint64_t> _softFontsSent;
    /// When the idle check of the echo state is next due. @see checkInputEchoWhenDue
    core::platform::SteadyTimePoint _nextInputEchoCheck {};
    /// Until when the idle check keeps itself armed. @see noteInputEchoActivity
    core::platform::SteadyTimePoint _inputEchoActiveUntil {};
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
    /// How this connection's end is told to the host: Closed once the client said Goodbye, else a drop.
    DetachKind _detachKind = DetachKind::Dropped;
    /// Whether the attached client shares Capability::DaemonFollow: it selects one workspace, is told the
    /// others' panes as summaries, and takes part in routing (followsDaemon()). Set as it attaches.
    enum class DaemonFollowing : uint8_t
    {
        No = 0,
        Yes,
    };
    DaemonFollowing _following = DaemonFollowing::No;
    /// Whether an endLayoutBatch() is live, which @ref run waits out before it lets `this` go.
    LayoutBatch _layoutBatch = LayoutBatch::Closed;
    /// The serial of the latest MoveNode or SelectWorkspace the open layout batch answers, sent as its
    /// LayoutBatchEnd::causeSerial; 0 when no request of this client caused it.
    uint64_t _layoutBatchCause = 0;
    /// The serial of the MoveNode being applied while its model events push; 0 outside that call.
    uint64_t _servingMove = 0;
    /// The workspaces the open layout batch changed, pushed once as it ends (@see flushChangedLayouts).
    std::unordered_set<uint64_t> _changedWorkspaces;
    /// The earliest flush deadline armed; nullopt while no flush is due.
    std::optional<core::platform::SteadyTimePoint> _flushDeadline;
    /// The flushAt() coroutines live, superseded ones included: what @ref run waits out before it lets
    /// `this` go, since a superseded flusher still sleeps with `this` in its frame.
    std::size_t _flushesInFlight = 0;
    /// run(), while it waits for spawnedFlowsEnded(): what the last SpawnedFlowClaim given back resumes.
    std::coroutine_handle<> _spawnedFlowsWaiter;
    bool _closed = false;
    /// The session this connection's input backed up in, past the high-water mark: the connection is read
    /// no further until it drains. @see awaitInputRoom
    std::optional<vtworkspace::SessionId> _inputBackedUp;
    /// Counts this connection as a client of the daemon from its accepted Attach hello on; holds nothing for
    /// a Control connection. Released after the subscriptions below, so a census that drops to zero sees the
    /// connection gone from the host already.
    ClientCensus::Ticket _clientTicket;
    /// The host's stream and model events, taken for an accepted Attach hello only: a Control connection is
    /// never among the host's subscribers, so it is sent no screen, resize, bell or layout, and it neither
    /// reports a client area nor is told one. Optional because a ScopedSubscription cannot be moved, and
    /// the last members, so they are released before anything they deliver to.
    std::optional<ScopedStreamSubscription> _streamSubscription;
    std::optional<ScopedModelSubscription> _layoutSubscription;
    /// The registry's label changes (@see labelChanged()).
    std::optional<ObjectRegistry::Subscription> _labelSubscription;
    UpgradeEnd _upgradeEnd = UpgradeEnd::Open;
    std::shared_ptr<UpgradeLink> _upgradeLink = std::make_shared<UpgradeLink>(UpgradeLink { this });
    /// Told of the daemon's upgrade from run() on. Last, so it is released before anything it delivers to.
    std::optional<UpgradeSubscription> _upgradeSubscription;
    /// Closes the connection if no hello was accepted by HandshakePolicy::authDeadline; disarmed on accept.
    std::optional<core::net::DeadlineTimer> _preAuthTimer;
};

/// One native client's whole lifetime, as a free coroutine (a capturing lambda coroutine would dangle its
/// closure; the arguments live in the frame).
/// @param loop The event loop everything runs on.
/// @param host The session host (not owned; outlives the connection).
/// @param id The connection's diagnostic identity.
/// @param connection The client transport.
/// @param services What this connection is served with; a TLS listener builds them per connection, for its
///        key authentication is bound to the connection's own session.
[[nodiscard]] core::async::Task<void> serveNativeClient(core::net::EventLoop* loop,
                                                        SessionHost* host,
                                                        ConnectionId id,
                                                        std::unique_ptr<core::net::ISocket> connection,
                                                        NativeSessionServices services);

/// What a TLS listener authenticates by key with, fixed at startup; each connection adds its own channel
/// binding to it.
struct TlsKeyAuth
{
    std::vector<AuthorizedKey> keys;              ///< The authorized keys; none means no key authentication.
    DaemonAuth auth = DaemonAuth::Any;            ///< Whether a token still works beside them.
    std::reference_wrapper<core::IRandom> random; ///< Where the challenge nonces come from.
};

/// Composes a native connection handler that FIRST encrypts each accepted socket (server-side TLS), then
/// serves the native protocol over it. Each connection has HandshakePolicy::helloTimeout (PreAuthTimeout by
/// default), from its accept, to finish the TLS handshake and have its hello accepted; one that has not is
/// closed.
/// @param loop The event loop every connection runs on; its clock times the deadline.
/// @param host The session host every connection serves (not owned; outlives the handler).
/// @param tls The server-side TLS context.
/// @param services What each native connection is served with, the TCP token among them.
/// @param keyAuth The keys a client may log in with by signature; with none, no challenge is sent.
/// @return The handler; refused when neither @p keyAuth holds a key nor @p services a token, as a network
///         listener has no file permissions to fall back on and would serve anyone who connects.
[[nodiscard]] std::expected<ConnectionHandler, std::string> makeTlsNativeHandler(
    core::net::EventLoop& loop,
    SessionHost& host,
    std::shared_ptr<core::net::ITlsContext> tls,
    NativeSessionServices services,
    TlsKeyAuth keyAuth);

/// The daemon's connection-handler factory for native-protocol clients. Each connection has
/// HandshakePolicy::helloTimeout, from its accept, to have its hello accepted; one that has not is closed.
/// @param loop The event loop every connection runs on; its clock times the deadline.
/// @param host The session host every connection serves (not owned; outlives the handler).
/// @param services What each connection is constructed with.
/// @return The handler.
[[nodiscard]] ConnectionHandler makeNativeHandler(core::net::EventLoop& loop,
                                                  SessionHost& host,
                                                  NativeSessionServices services = {});

} // namespace vthost
