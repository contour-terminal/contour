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

#include <vtbackend/primitives.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <coro/Task.hpp>
#include <muxserver/SessionHost.h>
#include <muxserver/proto/Pdu.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/WriteQueue.h>

namespace muxserver
{

/// One connected native-protocol client.
class NativeSession final: public SessionStreamEvents
{
  public:
    /// The default send-queue bound (see the constructor).
    static constexpr std::size_t DefaultWriteQueueBytes = std::size_t { 4 } * 1024 * 1024;

    /// @param loop The event loop everything runs on.
    /// @param host The session host (not owned; outlives this).
    /// @param connection The client transport (owned).
    /// @param maxWriteQueueBytes Send-queue byte bound; a client whose backlog
    ///        exceeds it is disconnected rather than under-served (its delta
    ///        cursor has already moved past what an overflow would drop).
    /// @param expectedToken The preshared token this endpoint requires in the
    ///        ClientHello; empty accepts any (the AF_UNIX default, where the
    ///        socket permissions are the gate).
    NativeSession(net::EventLoop& loop,
                  SessionHost& host,
                  std::unique_ptr<net::ISocket> connection,
                  std::size_t maxWriteQueueBytes = DefaultWriteQueueBytes,
                  std::string expectedToken = {});

    /// The connection flow: handshake, initial snapshot, then serve until the
    /// peer disconnects.
    [[nodiscard]] coro::Task<void> run();

    /// Marks @p session changed and schedules a debounced delta flush (the
    /// connection subscribes itself to the host's stream fan-out).
    void sessionScreenUpdated(vtmux::SessionId session) override;

    /// Drops the follow state for a session the host destroyed, so a long-lived
    /// connection that churns through many sessions does not accumulate it.
    void sessionClosed(vtmux::SessionId session) override;

    void sessionBell(vtmux::SessionId session) override;
    void sessionNotify(vtmux::SessionId session, std::string const& title, std::string const& body) override;
    void sessionCopyToClipboard(vtmux::SessionId session, std::string const& data) override;

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
        std::vector<uint32_t> lastModes; ///< Mirrored-mode set as last sent.
        /// The cursor position last sent to the mirror. A cursor-only move (no cell
        /// change, no mode flip) must still produce a delta, or the mirror's cursor
        /// stays put; -1 until the first (always-snapshot) delta sends one.
        int32_t lastCursorLine = -1;
        int32_t lastCursorColumn = -1;
        /// The screen the cursor followed last: primary and alternate are distinct
        /// grids with independent generations, so a flip must force a resync (and
        /// nullopt — a session never pushed before — forces the initial snapshot).
        std::optional<vtbackend::ScreenType> lastScreenType;
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
    };

    void handlePdu(proto::DecodedFrame const& frame);
    void send(uint64_t serial, proto::DecodedPdu const& pdu);

    /// Pushes a transient SessionEvent (bell / notification / clipboard) as an
    /// unsolicited frame (serial 0), once the handshake completed.
    void emitSessionEvent(vtmux::SessionId session,
                          proto::SessionEventKind kind,
                          std::string a,
                          std::string b);

    /// Validates the peer's ClientHello, answers, and pushes the attach
    /// snapshot (spawning the first session on an empty daemon).
    /// @return False when the hello was missing or version-mismatched.
    [[nodiscard]] bool completeHandshake(proto::DecodedFrame const& frame);

    /// Sends SessionState + a snapshot/delta for @p session (under its lock).
    void pushDelta(vtmux::SessionId session, bool forceSnapshot);

    [[nodiscard]] coro::Task<void> flushSoon();

    net::EventLoop& _loop;
    SessionHost& _host;
    std::unique_ptr<net::ISocket> _connection;
    net::WriteQueue _writer;
    std::string _expectedToken; ///< Required ClientHello token; empty accepts any.
    std::unordered_map<uint64_t, FollowState> _followed;
    std::unordered_set<uint64_t> _pendingSessions;
    bool _flushScheduled = false;
    bool _handshaken = false;
    bool _closed = false;
};

/// The daemon's connection-handler factory for native-protocol clients.
/// @param expectedToken The preshared token required in each ClientHello (empty
///        accepts any — the AF_UNIX default).
[[nodiscard]] std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeNativeHandler(
    net::EventLoop& loop, SessionHost& host, std::string expectedToken = {});

} // namespace muxserver
