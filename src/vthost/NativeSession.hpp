// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeSession` — one native-protocol (cells+deltas) client connection.
///
/// The server emulates, the client renders: after the ClientHello/ServerHello
/// version handshake the session pushes a full snapshot (SessionState + a
/// snapshot Delta per hosted session), then per-line deltas driven by the
/// host's screen-updated signal, debounced so bursts coalesce into one Delta.
/// Grid rows are addressed by stable id; a generation change triggers one
/// resync snapshot. Hyperlink URIs ship once per connection on first
/// reference; image pixels only on FetchImage.

#include <vtbackend/core/Primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <coro/Task.hpp>
#include <net/EventLoop.hpp>
#include <net/ISocket.hpp>
#include <net/WriteQueue.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// One connected native-protocol client.
class NativeSession final: public SessionStreamEvents
{
  public:
    /// The default send-queue bound (see the constructor).
    static constexpr std::size_t DefaultWriteQueueBytes = std::size_t { 4 } * 1024 * 1024;

    /// @param loop The event loop everything runs on.
    /// @param host The session host (not owned; outlives this).
    /// @param id This connection's diagnostic identity, prefixed onto its every log line so
    ///        the accept, handshake, trace and disconnect lines read as one story.
    /// @param connection The client transport (owned).
    /// @param maxWriteQueueBytes Send-queue byte bound; a client whose backlog
    ///        exceeds it is disconnected rather than under-served (its delta
    ///        cursor has already moved past what an overflow would drop).
    /// @param expectedToken The preshared token this endpoint requires in the
    ///        ClientHello; empty accepts any (the AF_UNIX default, where the
    ///        socket permissions are the gate).
    NativeSession(net::EventLoop& loop,
                  SessionHost& host,
                  ConnectionId id,
                  std::unique_ptr<net::ISocket> connection,
                  std::size_t maxWriteQueueBytes = DefaultWriteQueueBytes,
                  std::string expectedToken = {});

    /// The connection flow: handshake, initial snapshot, then serve until the
    /// peer disconnects.
    [[nodiscard]] coro::Task<void> run();

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
                       std::string const& body) override;
    void sessionCopyToClipboard(vtworkspace::SessionId session, std::string const& data) override;

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

  private:
    friend struct NativeSessionFollowTester; ///< Test-only view of _followed.

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
        /// The default fg/bg (0xRRGGBB) last sent (-1 until the first snapshot),
        /// so an OSC 10/11 change carries (and forces) a delta only on change.
        int lastDefaultForeground = -1;
        int lastDefaultBackground = -1;
        /// The status-display type/active last sent (-1 until the first snapshot),
        /// so a DECSSDT/DECSASD change carries (and forces) a delta only on change.
        int lastStatusDisplayType = -1;
        int lastActiveStatusDisplay = -1;
        /// The host-writable status-line rows last sent, so the (tiny) status page
        /// re-ships only when its content changed.
        std::vector<proto::WireLine> lastStatusLines;
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

    /// Encodes @p pdu and hands it to the write queue, disconnecting on refusal.
    /// @param serial The request serial to answer, or 0 for an unsolicited push.
    /// @param pdu What to send.
    /// @param sessionTag The session this frame describes, or 0 when it describes none
    ///        (hello, layout). A later snapshot for the same session supersedes the
    ///        tagged ones still unwritten — @see net::WriteQueue::dropTagged.
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

    /// Validates the peer's ClientHello, answers, and pushes the attach
    /// snapshot (spawning the first session on an empty daemon).
    /// @return False when the hello was missing or version-mismatched.
    [[nodiscard]] bool completeHandshake(proto::DecodedFrame const& frame);

    /// Sends SessionState + a snapshot/delta for @p session (under its lock).
    void pushDelta(vtworkspace::SessionId session, bool forceSnapshot);

    /// Pulls the session's live renditional state (title, cursor shape, cwd,
    /// colours, status display, Kitty-keyboard flags) into @p delta as diffs and —
    /// on a snapshot — captures the full state into @p state. Called by pushDelta
    /// with the terminal already locked; split out to keep pushDelta within the
    /// cognitive-complexity budget. Static: it reads only its arguments.
    /// @param screenTypeValue The wire screen-type discriminator (std::to_underlying).
    static void collectLiveState(vtbackend::Terminal& terminal,
                                 FollowState& follow,
                                 proto::Delta& delta,
                                 std::optional<proto::SessionState>& state,
                                 vtworkspace::SessionId session,
                                 uint8_t screenTypeValue,
                                 bool snapshot);

    /// What every session THIS connection creates is spawned with.
    ///
    /// One spelling for all four creation paths (the attach-time first session, CreateTab, NewWindow,
    /// SplitPane), so a fifth cannot quietly forget the client's preference.
    [[nodiscard]] SessionSpawnRequest spawnRequest() const
    {
        return SessionSpawnRequest { .settings = _clientSessionSettings };
    }

    [[nodiscard]] coro::Task<void> flushSoon();

    net::EventLoop& _loop;
    SessionHost& _host;
    std::unique_ptr<net::ISocket> _connection;
    net::WriteQueue _writer;
    ConnectionId _id;           ///< Prefixes this connection's every log line.
    std::string _expectedToken; ///< Required ClientHello token; empty accepts any.
    /// The emulation settings this client asked the sessions IT creates to have, layered onto the
    /// host's own in completeHandshake; nullopt when the client stated no preference.
    ///
    /// Per CONNECTION rather than per daemon, and applied only at creation: that is what lets two
    /// clients on different profiles share a daemon without either re-emulating the other's
    /// sessions. An application that already read DA1 cannot be told its terminal changed identity.
    std::optional<vtbackend::Settings> _clientSessionSettings;
    LayoutObserver _layoutObserver;
    std::unordered_map<uint64_t, FollowState> _followed;
    std::unordered_set<uint64_t> _pendingSessions;
    bool _flushScheduled = false;
    bool _handshaken = false;
    bool _closed = false;
};

/// The daemon's connection-handler factory for native-protocol clients.
/// @param expectedToken The preshared token required in each ClientHello (empty
///        accepts any — the AF_UNIX default).
[[nodiscard]] ConnectionHandler makeNativeHandler(net::EventLoop& loop,
                                                  SessionHost& host,
                                                  std::string expectedToken = {});

} // namespace vthost
