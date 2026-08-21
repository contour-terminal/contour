// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ScreenMirror` — brings a LOCAL `vtbackend::Terminal` up to date with a `RemoteScreen`, so a
/// daemon-hosted pane behaves and renders exactly like a locally hosted one.
///
/// It populates the terminal's grid DIRECTLY. It used to re-serialize each update into escape
/// sequences and feed them through a pty for the session's own parser to decode again, which was
/// appealing (one mechanism, no new API) but structurally lossy: the escape-sequence vocabulary
/// cannot express everything the wire carries. `LineFlag::Wrapped` has exactly one producer in the
/// whole engine — a real autowrap (`Screen::crlfIfWrapPending`) — so no sequence sets it, and a
/// mirror that cannot say "this row continues the one above" has no logical lines: reflow on resize,
/// double-click selection, search across a wrap and the shell's semantic marks all worked from wrap
/// state the mirror had invented. `Line::promptEndOffset()` had no spelling at all.
///
/// Writing the fields the wire names into the fields they name has no such ceiling, and it is
/// cheaper: an update no longer costs ~25 bytes of SGR per cell plus a full re-parse.
///
/// **What that obliges this class to do by hand.** Bypassing the parser means bypassing the
/// bookkeeping `Screen` normally performs, so every write goes through the narrowest engine
/// primitive that maintains it rather than at the arrays underneath:
///
///  - cells through `applyWireLine`/`writeCellToSoA` (the `trivial` render fast-path flag, the
///    grapheme-cluster pool, image-fragment replacement),
///  - scrolling through `Screen::scrollUp` (cursor iterator, plus `onBufferScrolled` for the
///    viewport, the Vi cursor and any live selection),
///  - the cursor through `Screen::moveCursorTo`, and the frame through `Terminal::screenUpdated`.
///
/// The mirror OWNS its terminal's screen, and its scrollback is real scrollback built by scrolling
/// rows through the page — so a resync must not erase it unless the server actually discarded its
/// own (@see LocalHistory).

#include <vtbackend/core/Image.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <vthost/GridWire.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::client
{

/// What a full replay does with the scrollback the mirror terminal already holds.
///
/// The mirror's local history is built by SCROLLING rows through the bottom of the page, so it is
/// real scrollback the mirror owns — and it is routinely deeper than what the server can still name
/// (the client keeps `RemoteScreen::historyKeep` rows, its own profile's `history.limit`; the
/// server keeps its `maxHistoryLineCount`).
/// Discarding it is therefore a deliberate act, not a side effect of resynchronizing.
enum class LocalHistory : uint8_t
{
    /// Keep what the mirror accumulated and repaint only the viewport. Correct for
    /// every resync where the server did NOT throw history away: a resize, a screen
    /// type or DEC page flip, a `ResyncRequired` snapshot. After a large forward
    /// viewport jump the rows that passed by unreported are simply ABSENT from local
    /// history — a gap, which is strictly better than erasing the rows that are there.
    ///
    /// A RESIZE belongs here even though it destroys the server's row identity: this terminal
    /// reflowed the same rows at the same moment, so its history is the reflowed truth rather
    /// than something stale. Discarding it would shorten the user's scrollback to the daemon's
    /// depth every time the window changed size — which local mode never does.
    ///
    /// Keep preserves history; it does not decline to HAVE any. Where the target page holds no
    /// local scrollback the replay streams the snapshot's rows in regardless, because there is no
    /// second copy to make and nothing to protect — the case being a mirror primed while the
    /// session sat on the alternate screen, which owns no primary history until it returns.
    Keep,

    /// Erase local scrollback and rebuild it from the server's rows. Correct only when the
    /// mirror's history is known to be wrong: the first replay into a fresh terminal, a
    /// reset or history-limit change (a generation bump with the size unchanged), and a
    /// server-side `clear`/`CSI 3 J` (which the caller detects as its floor outrunning the
    /// viewport advance).
    Discard,
};

/// Stateful per-session populator. Feed every Delta (after the owning `RemoteScreen` applied it);
/// the mirror terminal is up to date when the call returns.
class ScreenMirror
{
  public:
    /// @param terminal The local terminal to keep in sync. Borrowed — it outlives this mirror,
    ///        which is bound to it for life (the session/pane owns both).
    explicit ScreenMirror(vtbackend::Terminal& terminal) noexcept:
        _terminal { &terminal }, _ownStatusDisplayType { terminal.statusDisplayType() }
    {
    }

    /// Brings the mirror terminal up to date with @p screen after @p delta was applied to it.
    /// Falls back to a full replay on the first call, on snapshot deltas, and on generation,
    /// size or screen-type changes.
    void apply(RemoteScreen const& screen, proto::Delta const& delta);

    /// Replays everything unconditionally — the path for a freshly constructed mirror terminal.
    ///
    /// Every piece of non-cell state is asserted here, INCLUDING state that sits at its default
    /// value. A snapshot routes such state through `SessionState` instead of a Delta's changed-flag,
    /// so a value that returned to its default inside one would otherwise never be delivered — and
    /// the server, having already recorded it as sent, never re-sends it. That is how a
    /// Kitty-keyboard reset used to get lost, leaving the mirror encoding keys the hosted app had
    /// stopped asking for.
    ///
    /// Not defaulted: erasing the user's scrollback is the kind of decision a caller should have to
    /// write down.
    /// @param screen The mirrored screen to replay.
    /// @param history Whether to keep or erase the mirror's local scrollback.
    void fullReplay(RemoteScreen const& screen, LocalHistory history);

    /// Places image @p imageId now that its pixels arrived, or releases it if the server dropped
    /// it — what the client's image handler calls so the image appears where the delta put it.
    void applyImage(RemoteScreen const& screen, uint32_t imageId);

    /// Reproduces a transient session event (bell / desktop notification / OSC 52 clipboard write)
    /// on the mirror terminal, so the frontend's own bell, notify and clipboard handling — and its
    /// permissions — apply. Stateless: the event carries everything it needs.
    void applyEvent(proto::SessionEventPdu const& event);

  private:
    /// Brings the mirror's INPUT-encoding state in line with the server's: the mirrored DEC modes
    /// (each an independent boolean) and then the resolved mouse protocol, coordinate encoding and
    /// wheel mode, which are not modes at all. The order is load-bearing — @see the definition.
    void syncInputEncoding(RemoteScreen const& screen);

    /// Asserts the non-grid state a snapshot carries: title, cursor style, colors, working
    /// directory, status display and the two input-encoding protocols.
    void applySessionState(RemoteScreen const& screen);

    /// Applies a DECSCUSR `Ps` (@see vthost::CursorStylePsTable).
    /// @param ps The style off the wire; 0 restores this terminal's configured default.
    void applyCursorStyle(uint8_t ps);

    /// Applies the status-display state, WITHOUT letting the hosted session dictate this client's
    /// own chrome.
    ///
    /// `StatusDisplayType` conflates two things: the indicator line, which is a per-profile GUI
    /// decoration the viewer configures, and the host-writable line, which an application turns on
    /// with DECSSDT. Mirroring the wire value verbatim makes a daemon that hosts with no status line
    /// switch the viewer's indicator line OFF — which not only loses the user's configuration but
    /// RESIZES the pane (the line costs a row), reporting a geometry upstream that nothing asked for.
    ///
    /// So the two are combined rather than one overwriting the other: the RICHER of what the app
    /// asked for and what this terminal was configured with, the enum being ordered
    /// None < Indicator < HostWritable. An app turning its status line on is honoured; an app that
    /// never had one cannot take the viewer's away.
    /// @param wireType The `StatusDisplayType` the session reports.
    /// @param wireActive The `ActiveStatusDisplay` the session reports.
    void applyStatusDisplay(uint8_t wireType, uint8_t wireActive);

    /// Overwrites @p target in @p page with the row @p stableId names, or clears it to the default
    /// pen when the mirror holds no such row.
    ///
    /// @p target may be NEGATIVE — a row the server changed in place in its scrollback. Such a row
    /// is written only where the mirror actually holds it, and is never cleared for missing data
    /// (the local scrollback is the mirror's own and usually deeper than the server's).
    void writeRow(vtbackend::Screen& page, RemoteScreen const& screen, int64_t stableId, int64_t target);

    /// Scrolls one row into local history and writes @p stableId into the freed bottom line —
    /// how a row that scrolled by on the server gets into the mirror's own scrollback.
    void scrollInRow(vtbackend::Screen& page, RemoteScreen const& screen, int64_t stableId);

    /// Rebuilds the host-writable status page from @p screen's status rows.
    void applyStatusLines(RemoteScreen const& screen);

    /// (Re)builds every image placement whose anchor sits in the current viewport, and drops the
    /// fragments of images the server released.
    void applyImages(RemoteScreen const& screen);

    /// Places the cursor and publishes the frame. Every entry point ends here.
    void finish(RemoteScreen const& screen);

    /// @return The page the server's `screenType` names, on the mirror terminal.
    [[nodiscard]] vtbackend::Screen& activePage() const noexcept;

    /// Registers every URI in @p screen's side table that this terminal does not already hold under
    /// the wire id naming it, so `_linkIds` can translate the wire ids the rows carry. Ids are
    /// per-terminal counters, so a wire id means nothing until translated.
    ///
    /// A wire id whose URI CHANGED — the sender's 16-bit counter wrapped and reused it — takes a
    /// fresh local id rather than rewriting the one it had, so cells written before the reuse keep
    /// pointing at the URI they were drawn with.
    void syncHyperlinks(RemoteScreen const& screen);

    /// Brings the mirror's OSC 3008 context pool and ancestry in line with the server's.
    ///
    /// Ahead of the rows, and for the same reason syncHyperlinks() is: a row carries only an id while
    /// resolving it needs the record, and the record may arrive in the SAME delta as the first row
    /// referencing it.
    void syncContexts(RemoteScreen const& screen);

    /// A pointer, not a reference: a reference member would delete copy- and move-assignment, and
    /// the binding registry holds mirrors in a map. Never null.
    vtbackend::Terminal* _terminal;
    /// What this terminal was configured with, captured before any session state is applied — the
    /// state to return to when the hosted session is not asking for its host-writable line.
    /// @see applyStatusDisplay.
    vtbackend::StatusDisplayType _ownStatusDisplayType;

    bool _primed = false;
    uint64_t _generation = 0;
    int64_t _viewportBase = 0;
    int64_t _floor = 0; ///< Last applied scrollback floor (see apply()).
    /// The oldest stable id whose row the mirror still holds at its ARITHMETIC offset
    /// (`stableId - _viewportBase`), so an in-place change to a scrollback row can be placed.
    ///
    /// Not derivable from the local history depth: a replay that keeps local scrollback (a resize,
    /// where this terminal reflowed its own rows into a different set entirely) leaves history that
    /// the server's ids no longer name. Set by every full replay to as deep as that replay made
    /// addressable, and thereafter left alone — rows scrolling in extend the addressable range
    /// downwards on their own, since they all carry ids above it.
    int64_t _alignedFloor = 0;
    uint32_t _columns = 0;
    uint32_t _lines = 0;
    uint8_t _screenType = 0;
    /// Wire hyperlink id → this terminal's own id, so a URI is registered once and every later
    /// cell referencing it resolves without a lookup by string. Re-pointed, never merely inserted:
    /// the sender's id space wraps, and @see syncHyperlinks for what a reused id must do.
    std::unordered_map<uint16_t, vtbackend::HyperlinkId> _linkIds;

    /// Wire context id -> what this mirror adopted for it. Separate id spaces for the same reason the
    /// hyperlink map is separate: the sender's ContextId is a uint16_t that wraps and reuses, and a
    /// line written before a reuse legitimately points at the OLD record. The identifier rides along
    /// so a reused wire id is recognised as new rather than silently re-pointing every line already
    /// stamped with it. @see vthost::MirroredContext.
    vthost::ContextIdMap _contextIds;
    /// Server image id → the local `vtbackend::Image` built from its pixels, so a placement decodes
    /// them once. Holding the reference is also what keeps the pool entry alive: `ImagePool`'s id
    /// index is weak, so an image nothing holds is collected.
    std::unordered_map<uint32_t, std::shared_ptr<vtbackend::Image const>> _images;
};

} // namespace vthost::client
