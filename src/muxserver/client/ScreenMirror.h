// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ScreenMirror` — re-serializes `RemoteScreen` state into VT bytes for a
/// LOCAL mirror terminal (the GUI attach seam). Instead of populating a
/// render surface directly, the mirror drives a full `vtbackend::Terminal`
/// through its parser, so scrollback, selection, search and rendering all
/// work natively on the mirrored content.
///
/// The mirror OWNS its terminal's screen: it turns autowrap off and paints
/// with explicit cursor addressing, so every emitted byte has a
/// deterministic effect. Viewport rows repaint BOTTOM-UP because erasing any
/// continuation cell of a scaled-text block (OSC 66) destroys the whole
/// block — painting upward re-claims each block after its rows cleared.
/// History enters the local terminal by scrolling rendered lines through the
/// page, so the mirror's scrollback is real scrollback.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <muxserver/client/AttachClient.h>
#include <muxserver/proto/Pdu.h>

namespace muxserver::client
{

/// Stateful per-session reserializer. Feed every Delta (after the owning
/// RemoteScreen applied it) and write the returned bytes into the mirror
/// terminal via `Terminal::writeToScreen` or a ChannelPty feed.
class ScreenMirror
{
  public:
    /// Produces the byte stream bringing the mirror terminal up to date with
    /// @p screen after @p delta was applied to it. Falls back to a full
    /// replay on the first call, on snapshot deltas, and on generation, size
    /// or screen-type changes.
    [[nodiscard]] std::string apply(RemoteScreen const& screen, proto::Delta const& delta);

    /// Produces the full replay (history + viewport + cursor + title)
    /// unconditionally — the stream for a freshly constructed mirror
    /// terminal.
    [[nodiscard]] std::string fullReplay(RemoteScreen const& screen);

    /// Re-emits image @p imageId now that its pixels arrived, or releases it if
    /// the server dropped it — the byte stream the client's image handler feeds
    /// to the mirror terminal so the image appears where the delta placed it.
    /// Empty when the image's anchor is not in the current viewport.
    [[nodiscard]] std::string applyImage(RemoteScreen const& screen, uint32_t imageId);

  private:
    /// Emits DECSET/DECRST for every mirrored mode whose state differs from
    /// what the mirror terminal was last told, and remembers the new state.
    void syncModes(std::string& out, RemoteScreen const& screen);

    /// Emits a GIP upload (once per id, tracked in `_storedImages`) plus a
    /// placement for every cached image whose anchor sits in the current
    /// viewport. Called after the cells are painted, since painting clears
    /// image fragments the placement then re-establishes.
    void appendImages(std::string& out, RemoteScreen const& screen);

    bool _primed = false;
    uint64_t _generation = 0;
    int64_t _viewportBase = 0;
    int64_t _floor = 0; ///< Last applied scrollback floor (see apply()).
    uint32_t _columns = 0;
    uint32_t _lines = 0;
    uint8_t _screenType = 0;
    std::vector<uint32_t> _setModes;
    bool _modesKnown = false;
    /// Server image ids already uploaded to the mirror terminal (by the name
    /// `muximg_<id>`) — so pixels transmit once and later placements reference
    /// the stored image. Cleared per id when the server drops it.
    std::unordered_set<uint32_t> _storedImages;
};

} // namespace muxserver::client
