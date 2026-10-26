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
///  - cells through `applyWireLine`/`writeCellToSoA` (the line's shape the fast paths read, the
///    grapheme-cluster pool, image-fragment replacement),
///  - scrolling through `Screen::scrollUp` (cursor iterator, plus `onBufferScrolled` for the
///    viewport, the Vi cursor and any live selection),
///  - command-block heads through `Screen::noteBlockRowChange` and `Terminal::placeCommandBlockHeads`
///    (the parser's erases and prompts keep them on the host, by a rule the mirror applies to its own rows),
///  - the cursor through `Screen::moveCursorTo`, and the frame through `Terminal::screenUpdated` -- or
///    `Terminal::screenReplayed` for a full replay the host's plumbing caused, which repaints rather than
///    reports output. A primed mirror's screen switch, clear or reset is the program's doing and reports
///    output (@see frameOf).
///
/// The mirror OWNS its terminal's screen, and its scrollback is real scrollback built by scrolling
/// rows through the page — so a resync must not erase it unless the server actually discarded its
/// own (@see LocalHistory).

#include <vtbackend/core/Image.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <vthost/GridWire.hpp>
#include <vthost/HostIdentity.hpp>
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

/// Why a primed mirror, or an attaching one, rewrote its whole page rather than applying an update row by
/// row: the branch of ScreenMirror::apply() that took the update, or the explicit ScreenMirror::fullReplay().
/// Its frame is announced by frameOf().
enum class ReplayCause : uint8_t
{
    /// The first update into a fresh or restarted mirror (nothing recorded to compare with), or an explicit
    /// fullReplay(): what the host showed already, whatever state the session is found in.
    Attach,
    /// A snapshot delta to a primed mirror: the server resynchronized -- a ResyncRequired, or output that
    /// overran its scrollback floor between two pushes, which an increment can no longer name.
    Resync,
    /// The announced size moved: the window manager's doing, not the program's.
    Resize,
    /// The window into a larger host grid panned, or the host's page base moved back.
    WindowMoved,
    /// The program entered or left the alternate screen (or another DEC page).
    ScreenSwitch,
    /// The server's floor outran its viewport advance: the program cleared the history (`clear`, CSI 3 J).
    /// A history limit lowered on the host can read as this rather than as a Reset; the frame is the same.
    HistoryCleared,
    /// Row identity lost at an unchanged size: the program reset the terminal (RIS).
    ///
    /// A false positive the wire cannot rule out: sizes that changed and changed back while the host's
    /// snapshot was queued (80 -> 81 -> 80 columns) bump the generation at the size the mirror already has,
    /// so the server's reflow reads as a Reset and dots a background tab. Telling them apart needs a cause
    /// or size epoch in the snapshot, which is a protocol change.
    Reset,
};

/// How a frame of the mirror terminal is announced to its listener.
enum class MirrorFrame : uint8_t
{
    Replay, ///< Terminal::screenReplayed(): a repaint of what the host showed already, not news.
    Output, ///< Terminal::screenUpdated(): what the program did, news to whatever tracks unseen output.
};

/// The one place that decides whether a full replay is news (the GUI's tab dot): it is when the program
/// caused it, and only a primed mirror can tell that (an unprimed one's replay is the Attach).
///
/// A missed dot, never a false one: output that overran the server's scrollback floor between two pushes
/// arrives as a snapshot and reads as a Resync. Rarer still, a history-limit change on the host bumps the
/// generation as a reset does, and is a Reset (or a HistoryCleared, if it lowered the floor first). The one
/// false dot is a server-coalesced resize that ended at the mirror's size (@see ReplayCause::Reset).
/// @param cause Why the page was replayed.
/// @return How its frame is announced.
[[nodiscard]] constexpr MirrorFrame frameOf(ReplayCause cause) noexcept
{
    switch (cause)
    {
        case ReplayCause::Attach:
        case ReplayCause::Resync:
        case ReplayCause::Resize:
        case ReplayCause::WindowMoved: return MirrorFrame::Replay;
        case ReplayCause::ScreenSwitch:
        case ReplayCause::HistoryCleared:
        case ReplayCause::Reset: return MirrorFrame::Output;
    }
    return MirrorFrame::Replay;
}

/// Stateful per-session populator. Feed every Delta (after the owning `RemoteScreen` applied it);
/// the mirror terminal is up to date when the call returns.
class ScreenMirror
{
  public:
    /// @param terminal The local terminal to keep in sync. Borrowed — it outlives this mirror,
    ///        which is bound to it for life (the session/pane owns both).
    /// @param daemon Whether the daemon runs on this terminal's machine, as the connection's ServerHello
    ///        decided it (vthost::daemonMachineOf). Bound into @p terminal as its replica role
    ///        (vtbackend::Terminal::bindReplica), which every locality it judges follows from then on, with
    ///        this machine's identity as the daemon names machines (vthost::appSpecificIdentity).
    ///        Fixed for the mirror's life, as for the connection's: every attach builds a new mirror.
    ScreenMirror(vtbackend::Terminal& terminal, DaemonMachine daemon);

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
    ///
    /// Its frame is announced as Terminal::screenReplayed(), not screenUpdated(): it is the Attach
    /// (@see frameOf), so nothing that tracks unseen output (the GUI's tab badge) takes it for news.
    /// @param screen The mirrored screen to replay.
    /// @param history Whether to keep or erase the mirror's local scrollback.
    void fullReplay(RemoteScreen const& screen, LocalHistory history);

    /// Forgets everything learned from the connection that fed this mirror -- every stable-id mapping,
    /// generation, hyperlink, context and image -- so that the next update, from a new connection, is applied
    /// as the first one is: a full replay. Called when a lost connection was replaced, before the new one's
    /// snapshots arrive; the terminal and its replica role stay.
    void restart();

    /// Places image @p imageId now that its pixels arrived, or releases it if the server dropped
    /// it — what the client's image handler calls so the image appears where the delta put it.
    void applyImage(RemoteScreen const& screen, uint32_t imageId);

    /// The row of the host's primary grid that a row of this mirror's primary page holds: what the host is
    /// asked to act on when the user acts on the mirror's row -- a vi mark, which is the host's.
    ///
    /// Only rows the mirror holds where the host's numbering puts them can be named: the page and history
    /// down to where a resize left the two numberings apart. Not while the alternate page is shown, whose
    /// numbers are the only ones the mirror follows then.
    ///
    /// Takes the terminal lock; call it where the mirror is applied, on the thread its updates arrive on.
    /// @param stableId The row, by this mirror's own stable id.
    /// @param generation The stable-id generation of this mirror's grid that @p stableId was taken in
    ///        (vtbackend::Grid::stableIdGeneration); a row named in another one is not named at all.
    /// @return The host's row, or std::nullopt when this mirror cannot name it.
    [[nodiscard]] std::optional<proto::RowAddress> hostRowOf(int64_t stableId, uint64_t generation) const;

    /// Reproduces a transient session event (bell / desktop notification / OSC 52 clipboard write)
    /// on the mirror terminal, so the frontend's own bell, notify and clipboard handling — and its
    /// permissions — apply; and replaces the program-status records with a ProgramStatus set, under the
    /// terminal lock. Stateless: the event carries everything it needs.
    void applyEvent(proto::SessionEventPdu const& event);

  private:
    /// The full replay itself, announced as frameOf(@p cause) says: what fullReplay() and every replaying
    /// branch of apply() run.
    /// @param screen The mirrored screen to replay.
    /// @param history Whether to keep or erase the mirror's local scrollback.
    /// @param cause Why the page is replayed.
    void replay(RemoteScreen const& screen, LocalHistory history, ReplayCause cause);

    /// Replaces the mirror terminal's program-status records with the host's
    /// (Terminal::replaceProgramStatus), dropping each record whose state this build does not know. The
    /// caller holds the terminal lock.
    /// @param records The host's whole set, least recently updated first.
    void syncProgramStatus(std::span<proto::WireProgramStatusRecord const> records);

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

    /// Writes @p row over the line at @p offset, or clears that line when the mirror holds no data for it,
    /// then has the command blocks' heads and the user-mark index follow what the write did.
    ///
    /// The host's cached head never crosses the wire (proto::WireCommandBlock leaves it out), so the mirror
    /// heads each block by the host's own rule, among its own rows. A write that took the row out of its
    /// block, or changed the marks a head is found by, is the host's erase or new prompt arriving: it is
    /// counted as the host counted it, so the next read re-heads that block as the host did. A row carrying
    /// such a mark may head a block that has no head here yet: its block is noted for placeHeads().
    ///
    /// A vi mark is the host's, as the row's flags say (hostRowOf() names the row it is asked on): the write
    /// takes the row's, and a mark it placed or took away is reported as a block's change is, since it never
    /// passes the funnel a local one is written through. Both go through vtbackend::Screen::noteRowRewritten,
    /// as an erase's do.
    /// @see vtbackend::Terminal::placeCommandBlockHeads
    /// @param page The page the line belongs to.
    /// @param offset Where the line is in @p page; negative for a scrollback row.
    /// @param row The wire row to write, or nullptr to clear the line.
    /// @param firstColumn The column of @p row that lands in the line's first one (@see writeRow).
    void writeLine(vtbackend::Screen& page,
                   vtbackend::LineOffset offset,
                   proto::WireLine const* row,
                   vtbackend::ColumnOffset firstColumn);

    /// Heads every block noted since the last call that has no head in the mirror's grid yet, by the rule
    /// the host heads by — once all of an update's rows are written, since a lowest mark can only be told
    /// with every row in place. A block whose host head row is gone is recorded as evicted instead, as the
    /// host holds it.
    void placeHeads();

    /// Records where the primary page stands, when it is the page an update just wrote.
    /// @see hostRowOf
    void anchorPrimaryPage(RemoteScreen const& screen) noexcept;

    /// Scrolls one row into local history and writes @p stableId into the freed bottom line —
    /// how a row that scrolled by on the server gets into the mirror's own scrollback.
    void scrollInRow(vtbackend::Screen& page, RemoteScreen const& screen, int64_t stableId);

    /// Paints @p screen's status rows on the display they are for: the host-writable status page, or the
    /// indicator row the host's status-line extensions drew, which the mirror then shows in place of its own
    /// template -- until an indicator page with no rows gives it its own back.
    void applyStatusLines(RemoteScreen const& screen);

    /// (Re)builds every image placement whose anchor sits in the current viewport, and drops the
    /// fragments of images the server released.
    void applyImages(RemoteScreen const& screen);

    /// Places the cursor and publishes the frame. Every entry point ends here.
    void finish(RemoteScreen const& screen);

    /// @return The page the server's `screenType` names, on the mirror terminal.
    [[nodiscard]] vtbackend::Screen& activePage() const noexcept;

    /// Which cell of @p screen's grid the mirror page's top-left cell shows: the origin of the window a
    /// page smaller than the host's grid shows into it, following the host's cursor (viewportOrigin), and
    /// (0,0) for a page at least as large. Under the terminal lock: it reads the page's size.
    [[nodiscard]] vtbackend::CellLocation originFor(RemoteScreen const& screen) const;

    /// @return The host's row the mirror page's first line shows: the viewport base plus the origin's line.
    [[nodiscard]] int64_t pageTop() const noexcept;

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

    /// Which rule a command-block sync adopts records under.
    enum class CommandBlockSync : uint8_t
    {
        Follow = 0, ///< The mirror has been following the session: a change it sees is news.
        Prime = 1,  ///< The mirror's first replay — the attach: everything it sees is history.
    };

    /// Brings the mirror terminal's command-block store in line with @p screen's table, ahead of the
    /// rows for the reason syncContexts() is.
    ///
    /// Adopts, oldest first, each record that changed since the last sync -- RemoteScreen's revision index
    /// names them, so a sync costs what changed, not the table -- and one identical to the store's copy is
    /// a resend the store passes over. On @p sync Prime every record is AdoptMode::Snapshot:
    /// reattaching must never replay a finish that happened while detached. Otherwise a record is
    /// AdoptMode::Live — so a finish raises Events::commandBlockFinished once — unless the store never held
    /// it and it is older than the newest it holds, which is history this mirror was never shown.
    ///
    /// Retired records leave the store BEFORE anything is adopted. That is also what follows the host
    /// when its ids start over: it forgets every record, so the walk that sends the new epoch's first
    /// id retires all the old ones in the same delta, and the new id 1 lands in an empty store — not
    /// below the old ids, where it would read as history or a full store would refuse it. And before
    /// that, when the epoch @p screen states moved, every row of the mirror forgets its block id
    /// (vtbackend::Screen::forgetBlockIds), as the host's rows did: the rows the host still holds come
    /// again without theirs, but a row only the mirror holds would go on naming the new epoch's block
    /// of the same number.
    ///
    /// A record's locality stands only when the daemon runs on this machine; otherwise the terminal holds
    /// it as Foreign (vtbackend::Terminal::adoptCommandBlock, by the replica role the constructor bound).
    /// @param screen The mirrored screen whose table to follow.
    /// @param sync Whether this is the attach (Prime) or a later update (Follow).
    void syncCommandBlocks(RemoteScreen const& screen, CommandBlockSync sync);

    /// Drops from the store every record @p screen's table no longer holds (the host retired it), in
    /// place (vtbackend::CommandBlockStore::retire): the records that stay are not touched.
    ///
    /// One merge walk over the store and the table, run only when a sync finds a removal it has not seen.
    /// @param screen The mirrored screen whose table names the records the host still holds.
    void retireAbsentCommandBlocks(RemoteScreen const& screen);

    /// Has the terminal measure its clock against the host's newest reading in @p screen
    /// (vtbackend::Terminal::adoptHostClock), once per reading, before any record is adopted.
    /// @param screen The mirrored screen carrying the reading of the last SessionState.
    void syncHostClock(RemoteScreen const& screen);

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
    /// The cell of the host's grid the mirror page's top-left cell shows, as the last full replay placed it.
    /// A change rewrites the page (@see apply()); every row written onto the page and the cursor are offset
    /// by it, and the terminal adds it to the mouse reports it sends.
    vtbackend::CellLocation _origin {};
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
    /// The RemoteScreen::commandBlocksRevision this mirror last synced at.
    uint64_t _syncedCommandBlocks = 0;
    /// The epoch the block ids on this mirror's rows count in (proto::SessionState::commandBlockIdEpoch).
    uint64_t _commandBlockIdEpoch = 0;

    /// Where the primary page stood, in this mirror's own row ids, when an update last wrote it: its first
    /// line held the host row at `_viewportBase`. Kept in ids rather than offsets because this terminal's
    /// page may change its line count meanwhile -- a status line the user turns on -- which keeps every row's
    /// id but not its offset.
    struct PageAnchor
    {
        int64_t mirrorBase = 0;          ///< The stable id of the page's first line, in this mirror's grid.
        uint64_t mirrorIdGeneration = 0; ///< This grid's stable-id generation then.
        uint64_t hostIdGeneration = 0;   ///< The host grid's stable-id generation then.
    };
    /// Unset until the primary page was first written.
    /// @see hostRowOf
    std::optional<PageAnchor> _primaryAnchor;
    /// The RemoteScreen::hostTime this mirror last measured its clock against; 0 for none.
    int64_t _measuredHostTime = 0;
    /// The blocks the update being applied may give a head: each record it adopted anew, each whose head a
    /// replay forgot, and each a row it wrote carries a head mark of. Gathered as the rows are written and
    /// looked up only afterwards, in placeHeads(): a read of the store between two row writes would re-head
    /// blocks among half-written rows.
    std::vector<vtbackend::CommandBlockId> _headCandidates;
    /// The blocks the update being applied may give a head whose host says its head row is gone
    /// (vthost::HostHead::Evicted): recorded as evicted in placeHeads(), before any is looked for.
    std::vector<vtbackend::CommandBlockId> _evictedHeads;
    /// Server image id → the local `vtbackend::Image` built from its pixels, so a placement decodes
    /// them once. Holding the reference is also what keeps the pool entry alive: `ImagePool`'s id
    /// index is weak, so an image nothing holds is collected.
    std::unordered_map<uint32_t, std::shared_ptr<vtbackend::Image const>> _images;
};

} // namespace vthost::client
