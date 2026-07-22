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
#include <unordered_set>
#include <utility>
#include <vector>

#include <coro/Task.hpp>
#include <muxserver/proto/Pdu.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/WriteQueue.h>

namespace muxserver::client
{

/// Appends @p cell's grapheme cluster (base codepoint plus any combining extras)
/// to @p out as UTF-8. The empty-codepoint (blank) case is the caller's to
/// handle. Shared by every wire-cell → text/tty path so cluster emission lives
/// in exactly one place.
/// @param out The byte stream to append to.
/// @param cell The wire cell whose cluster to emit.
void appendCluster(std::string& out, proto::WireCell const& cell);

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
    uint8_t cursorShape = 0; ///< DECSCUSR Ps (0 = unknown/default); re-emitted as CSI Ps SP q.
    std::string cwd;         ///< OSC 7 working-directory URL, re-emitted as OSC 7.

    uint64_t generation = 0;
    uint64_t seqno = 0;
    int64_t viewportBase = 0; ///< Stable id of viewport row 0.
    int64_t stableFloor = 0;  ///< Oldest stable id the server still holds; rows below are evicted.

    /// Rows by stable id (ordered, so eviction trims the oldest first).
    std::map<int64_t, proto::WireLine> rows;
    /// Hyperlink id → URI, merged from the deltas' side tables.
    std::unordered_map<uint16_t, std::string> hyperlinks;
    /// The mirrored DEC private modes currently SET remotely (by number).
    std::vector<uint32_t> setModes;

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

    /// @return The image-cell entry at (@p stableId, @p column), or nullptr.
    [[nodiscard]] proto::ImageCellEntry const* imageAt(int64_t stableId, uint16_t column) const;

    /// @return The cached pixels for @p imageId, or nullptr if not fetched yet.
    [[nodiscard]] proto::ImageData const* imageData(uint32_t imageId) const;

    /// Drops @p imageId: forgets its pixels and clears every cell referencing it
    /// (those cells render blank until redrawn). Called on ImageGone.
    void dropImage(uint32_t imageId);
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

    /// Invoked when an image's pixels arrive (ImageData) or the image is dropped
    /// (ImageGone) for @p imageId in @p screen — so the frontend can repaint the
    /// cells referencing it. After a drop, `screen.imageData(imageId)` is null.
    void setImageHandler(std::function<void(RemoteScreen const&, uint32_t imageId)> handler)
    {
        _onImage = std::move(handler);
    }

    /// Invoked when a transient session event (bell / desktop notification /
    /// OSC 52 clipboard write) arrives for @p screen's session — the frontend
    /// re-emits the matching VT into its mirror terminal or acts on it directly.
    void setSessionEventHandler(std::function<void(RemoteScreen const&, proto::SessionEvent const&)> handler)
    {
        _onSessionEvent = std::move(handler);
    }

    /// Sends keyboard/paste bytes to @p session's PTY.
    void sendInput(uint64_t session, std::string_view bytes);

    /// Proposes a client size; the server answers with fresh snapshots.
    void requestResize(uint32_t columns, uint32_t lines);

    /// Requests an image's pixels: @p session scopes the per-session image pool,
    /// @p imageId is the pool-local id carried by the cell's ImageCellEntry.
    void fetchImage(uint64_t session, uint32_t imageId);

    /// Closes the connection; run() finishes.
    void detach();

    /// @return All mirrored screens, keyed by session id.
    [[nodiscard]] std::map<uint64_t, RemoteScreen> const& screens() const noexcept { return _screens; }

    /// @return True once the ServerHello arrived with a matching version.
    [[nodiscard]] bool connected() const noexcept { return _connected; }

    /// @return True if the server answered with an incompatible codec version.
    [[nodiscard]] bool versionMismatch() const noexcept { return _versionMismatch; }

  private:
    void handlePdu(proto::DecodedFrame const& frame);
    /// Encodes @p pdu with the next serial, enqueues it, and returns that serial
    /// so image fetches can correlate the (session-less) ImageData/ImageGone reply.
    uint64_t send(proto::DecodedPdu const& pdu);

    std::unique_ptr<net::ISocket> _connection;
    net::WriteQueue _writer;
    std::function<void(RemoteScreen const&, proto::Delta const&)> _onUpdate;
    std::function<void(RemoteScreen const&, uint32_t)> _onImage;
    std::function<void(RemoteScreen const&, proto::SessionEvent const&)> _onSessionEvent;
    std::map<uint64_t, RemoteScreen> _screens;
    /// Outstanding image fetches: request serial → (session, imageId). The reply
    /// carries no session, so the serial is what routes it to the right screen.
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> _pendingImages;
    uint64_t _nextSerial = 1;
    bool _connected = false;
    bool _versionMismatch = false;
    bool _detached = false;
};

} // namespace muxserver::client
