// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `AttachClient` — the native-protocol client engine (Qt-free).
///
/// The client runs NO parser: it mirrors each remote session's screen from the
/// server's stable-id-addressed Delta stream into a `RemoteScreen` — a plain
/// data model any frontend can render (the TTY attach client, later the GUI's
/// remotely-populated display seam). Input flows the other way as Input PDUs.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <coro/Task.hpp>
#include <muxserver/proto/Pdu.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/WriteQueue.h>

namespace muxserver::client
{

/// The client-side mirror of one remote session's screen.
struct RemoteScreen
{
    uint64_t session = 0;
    uint32_t columns = 0;
    uint32_t lines = 0;
    uint8_t screenType = 0;
    int32_t cursorLine = 0;
    int32_t cursorColumn = 0;
    std::string title;

    uint64_t generation = 0;
    uint64_t seqno = 0;
    int64_t viewportBase = 0; ///< Stable id of viewport row 0.

    /// Rows by stable id (ordered, so eviction trims the oldest first).
    std::map<int64_t, proto::WireLine> rows;
    /// Hyperlink id → URI, merged from the deltas' side tables.
    std::unordered_map<uint16_t, std::string> hyperlinks;
    /// The mirrored DEC private modes currently SET remotely (by number).
    std::vector<uint32_t> setModes;

    /// How many rows above the viewport to keep as client-side scrollback.
    static constexpr int64_t HistoryKeep = 10000;

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
};

/// One attached native-protocol connection.
class AttachClient final
{
  public:
    /// @param loop The event loop everything runs on.
    /// @param connection The server transport (owned).
    AttachClient(net::EventLoop& loop, std::unique_ptr<net::ISocket> connection);

    /// The connection flow: sends ClientHello, mirrors server pushes until the
    /// server disconnects or detach() is called.
    [[nodiscard]] coro::Task<void> run();

    /// Invoked after every applied Delta with the updated screen.
    void setUpdateHandler(std::function<void(RemoteScreen const&, proto::Delta const&)> handler)
    {
        _onUpdate = std::move(handler);
    }

    /// Sends keyboard/paste bytes to @p session's PTY.
    void sendInput(uint64_t session, std::string_view bytes);

    /// Proposes a client size; the server answers with fresh snapshots.
    void requestResize(uint32_t columns, uint32_t lines);

    /// Requests an image's pixels by stable id.
    void fetchImage(uint32_t imageId);

    /// Closes the connection; run() finishes.
    void detach();

    /// @return All mirrored screens, keyed by session id.
    [[nodiscard]] std::map<uint64_t, RemoteScreen> const& screens() const noexcept { return _screens; }

    /// @return True once the ServerHello arrived with a matching version.
    [[nodiscard]] bool connected() const noexcept { return _connected; }

    /// @return True if the server answered with an incompatible codec version.
    [[nodiscard]] bool versionMismatch() const noexcept { return _versionMismatch; }

  private:
    void handlePdu(proto::DecodedPdu const& pdu);
    void send(proto::DecodedPdu const& pdu);

    net::EventLoop& _loop;
    std::unique_ptr<net::ISocket> _connection;
    net::WriteQueue _writer;
    std::function<void(RemoteScreen const&, proto::Delta const&)> _onUpdate;
    std::map<uint64_t, RemoteScreen> _screens;
    uint64_t _nextSerial = 1;
    bool _connected = false;
    bool _versionMismatch = false;
    bool _detached = false;
};

} // namespace muxserver::client
