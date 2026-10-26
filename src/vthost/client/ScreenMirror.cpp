// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ScreenMirror.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <core/Overloaded.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/ContextWire.hpp>
#include <vthost/CursorStyle.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/ImageWire.hpp>
#include <vthost/MirroredModes.hpp>
#include <vthost/MouseWire.hpp>
#include <vthost/NotifyWire.hpp>
#include <vthost/ProgressWire.hpp>
#include <vthost/StatusWire.hpp>
#include <vthost/TimeWire.hpp>
#include <vthost/client/ViewportOrigin.hpp>
#include <vthost/control/ProgramStatusConvert.hpp>

namespace vthost::client
{

namespace
{
    /// @return The row @p stableId names, or nullptr if the mirror does not hold it.
    [[nodiscard]] proto::WireLine const* rowOrNull(RemoteScreen const& screen, int64_t stableId)
    {
        auto const it = screen.rows.find(stableId);
        return it != screen.rows.end() ? &it->second : nullptr;
    }

    /// One image's placement, accumulated from the cells it covers.
    struct Placement
    {
        int64_t anchorRow = 0;     ///< Stable id of the image's top-left cell.
        uint16_t anchorColumn = 0; ///< Column of that cell.
        uint32_t rows = 0;         ///< Cell rows the image spans.
        uint32_t columns = 0;      ///< Cell columns the image spans.
        uint8_t layer = 0;
        uint8_t alignment = 0;
        uint8_t resize = 0;
        bool anchored = false; ///< The (0,0) tile is among the cells seen.
    };

    /// Groups @p screen's image cells by image, recovering each placement's extent and anchor.
    ///
    /// The wire describes images per COVERED CELL, so the placement is not stated anywhere — it is
    /// the bounding box of the offsets, and the anchor is the cell whose offset is (0,0).
    [[nodiscard]] std::unordered_map<uint32_t, Placement> placementsOf(RemoteScreen const& screen)
    {
        auto placements = std::unordered_map<uint32_t, Placement> {};
        for (auto const& [stableId, columns]: screen.imageCells)
        {
            for (auto const& [column, entry]: columns)
            {
                auto& placement = placements[entry.imageId];
                placement.layer = entry.layer;
                placement.alignment = entry.alignment;
                placement.resize = entry.resize;
                placement.rows = std::max(placement.rows, static_cast<uint32_t>(entry.offsetLine) + 1);
                placement.columns =
                    std::max(placement.columns, static_cast<uint32_t>(entry.offsetColumn) + 1);
                if (entry.offsetLine == 0 && entry.offsetColumn == 0)
                {
                    placement.anchorRow = stableId;
                    placement.anchorColumn = column;
                    placement.anchored = true;
                }
            }
        }
        return placements;
    }

    /// The eviction count that numbers the mirror's rows as the server numbers them.
    ///
    /// The server's row at its floor is number `evicted + 1`, so the mirror's top row is that plus its id's
    /// distance below the floor -- or minus it, where the mirror keeps history the server has since dropped,
    /// and the top row lies above the floor. Rows the mirror kept that the server's ids no longer name
    /// continue the arithmetic, as they continue the page's offsets. Every operand
    /// is peer-announced, so the distance is taken in unsigned arithmetic, which wraps where signed
    /// arithmetic would overflow, and the count is clamped at zero: a mirror holding rows older than any
    /// the server ever numbered calls its top row 1.
    /// @param screen The mirrored screen: the server's count and its floor.
    /// @param pageTop The server's row the mirror page's first line shows (ScreenMirror::pageTop).
    /// @param topOffset The mirror grid's addressableTop(), relative to its page, which @p pageTop
    ///        names.
    /// @return The count for the mirror grid to adopt.
    [[nodiscard]] uint64_t mirroredEvictedRowCount(RemoteScreen const& screen,
                                                   int64_t pageTop,
                                                   vtbackend::LineOffset topOffset)
    {
        auto const below = static_cast<uint64_t>(pageTop) + static_cast<uint64_t>(unbox<int64_t>(topOffset))
                           - static_cast<uint64_t>(screen.stableFloor);
        if (static_cast<int64_t>(below) >= 0)
            return screen.evictedRowCount + below;
        auto const above = uint64_t { 0 } - below;
        return screen.evictedRowCount - std::min(screen.evictedRowCount, above);
    }
} // namespace

ScreenMirror::ScreenMirror(vtbackend::Terminal& terminal, DaemonMachine daemon):
    _terminal { &terminal }, _ownStatusDisplayType { terminal.statusDisplayType() }
{
    // Under the lock, as every later read of the role is: the terminal is already shown, and the GUI asks
    // it where its directory is whenever it likes.
    auto const guard = std::lock_guard { terminal };
    // The daemon sends every machine id derived, so this machine is named the same way to be compared.
    terminal.bindReplica(daemon == DaemonMachine::Same ? vtbackend::ReplicaRole::SameMachine
                                                       : vtbackend::ReplicaRole::OtherMachine,
                         appSpecificIdentity(terminal.localIdentity()));
}

void ScreenMirror::restart()
{
    _primed = false;
    _generation = 0;
    _viewportBase = 0;
    _floor = 0;
    _alignedFloor = 0;
    _columns = 0;
    _lines = 0;
    _screenType = 0;
    _origin = {};
    _linkIds.clear();
    _contextIds.clear();
    _syncedCommandBlocks = 0;
    _commandBlockIdEpoch = 0;
    _primaryAnchor.reset();
    _measuredHostTime = 0;
    _headCandidates.clear();
    _evictedHeads.clear();
    _images.clear();
}

vtbackend::Screen& ScreenMirror::activePage() const noexcept
{
    return _screenType == 1 ? _terminal->alternateScreen() : _terminal->primaryScreen();
}

vtbackend::CellLocation ScreenMirror::originFor(RemoteScreen const& screen) const
{
    auto const remote = vtbackend::PageSize { .lines = vtbackend::LineCount::cast_from(screen.lines),
                                              .columns = vtbackend::ColumnCount::cast_from(screen.columns) };
    // Clamped into the grid, as the cursor the host states always is: a peer's out-of-range value must not
    // pan the view off it.
    auto const cursor = vtbackend::CellLocation {
        .line = vtbackend::LineOffset::cast_from(
            std::clamp<int64_t>(screen.cursorLine, 0, std::max<int64_t>(int64_t { screen.lines } - 1, 0))),
        .column = vtbackend::ColumnOffset::cast_from(std::clamp<int64_t>(
            screen.cursorColumn, 0, std::max<int64_t>(int64_t { screen.columns } - 1, 0))),
    };
    return viewportOrigin(remote, activePage().pageSize(), cursor);
}

int64_t ScreenMirror::pageTop() const noexcept
{
    return _viewportBase + unbox<int64_t>(_origin.line);
}

void ScreenMirror::syncHyperlinks(RemoteScreen const& screen)
{
    // Ahead of the rows rather than lazily per cell, because a cell carries only an id while the
    // translation needs the URI — which lives in a side table that may arrive in the SAME delta as
    // the first cell referencing it. The table is bounded by the sender's own hyperlink LRU and
    // grows by at most a handful of entries per delta, so walking it costs nothing measurable.
    auto& store = _terminal->hyperlinks();
    for (auto const& [wireId, uri]: screen.hyperlinks)
    {
        if (wireId == 0)
            continue;
        if (auto const known = _linkIds.find(wireId); known != _linkIds.end())
        {
            auto const info = store.hyperlinkById(known->second);
            if (info && info->uri == uri)
                continue; // this terminal already holds exactly the mapping the server named
            // Otherwise the server is telling us something new about an id we thought we knew: its
            // HyperlinkId is a uint16_t that wraps and reuses ids, and NativeSession re-sends the
            // pair precisely so a reused id cannot pin the mirror to a stale URI (or the entry fell
            // out of this terminal's LRU and has to be registered again).
            //
            // A FRESH local id rather than rewriting the info behind the old one: cells written
            // before the reuse legitimately point at the OLD URI, and mutating it in place would
            // retroactively re-target every one of them — the links already on screen would start
            // opening somewhere else.
        }
        auto const id = store.nextHyperlinkId++;
        // No userId: that field exists so OSC 8's `id=` parameter can reuse an id across
        // sequences, and this side never sees an OSC 8 — the sender already resolved it.
        store.cache.emplace(id,
                            std::make_shared<vtbackend::HyperlinkInfo>(
                                vtbackend::HyperlinkInfo { .userId = {}, .uri = uri }));
        _linkIds.insert_or_assign(wireId, id);
    }
}

void ScreenMirror::syncContexts(RemoteScreen const& screen)
{
    auto& contexts = _terminal->contexts();
    for (auto const& [wireId, wire]: screen.contexts)
    {
        if (wire.id == 0)
            continue;

        auto localParent = vtbackend::ContextId {};
        if (auto const parent = _contextIds.find(wire.parent); parent != _contextIds.end())
            localParent = parent->second.local;

        auto const known = _contextIds.find(wire.id);
        if (known != _contextIds.end() && known->second.identifier == wire.identifier)
        {
            // The SAME context, so it keeps the local id every line already stamped with it points
            // at -- but not necessarily the same metadata: a re-`start=` reinitialises a context in
            // place under the same identifier, and `end=` records an outcome on it. Skipping on the
            // identifier alone left an attached pane resolving the cwd of the session's first prompt
            // forever. Compared without building the record, because this loop runs over the whole
            // accumulated pool on every delta. @see vthost::matchesWireContext.
            auto const* const current = contexts.find(known->second.local);
            if (current != nullptr && matchesWireContext(*current, wire, localParent))
                continue;
            _terminal->adoptContext(vthost::fromWireContext(wire, known->second.local, localParent));
            continue;
        }

        // A FRESH local id when the sender reuses a wire id for a different context, rather than
        // rewriting the record behind the old one: lines written before the reuse legitimately point
        // at the OLD context, and mutating it in place would retroactively re-attribute every one of
        // them. Exactly the rule syncHyperlinks() follows, and for exactly the same reason.
        auto const localId = contexts.nextAdoptedId();
        _terminal->adoptContext(vthost::fromWireContext(wire, localId, localParent));
        _contextIds.insert_or_assign(
            wire.id, vthost::MirroredContext { .local = localId, .identifier = wire.identifier });
    }

    // The ancestry, translated through the same map. Asserted even when EMPTY: a snapshot routes it
    // through SessionState rather than a changed-gate, so a value that returned to its default would
    // otherwise never be delivered -- which is how a Kitty-keyboard reset used to get lost.
    auto chain = std::vector<vtbackend::ContextId> {};
    chain.reserve(screen.contextChain.size());
    for (auto const wireId: screen.contextChain)
        if (auto const it = _contextIds.find(wireId); it != _contextIds.end())
            chain.push_back(it->second.local);
    _terminal->setContextChain(chain);
}

void ScreenMirror::syncCommandBlocks(RemoteScreen const& screen, CommandBlockSync sync)
{
    // First of all, as the host forgets its own rows' ids first: an id from before its ids started over names
    // nothing now, or a block minted since. The host sends again the rows it still holds, without their ids;
    // the ones it no longer holds -- this mirror's deeper scrollback -- only this forgets.
    if (screen.commandBlockIdEpoch != _commandBlockIdEpoch)
    {
        _terminal->primaryScreen().forgetBlockIds();
        _commandBlockIdEpoch = screen.commandBlockIdEpoch;
    }
    // Gated like the host's walk: nothing in the table moved since the last sync, nothing to adopt.
    // The first replay always runs — it has never synced.
    if (sync == CommandBlockSync::Follow && screen.commandBlocksRevision == _syncedCommandBlocks)
        return;
    // Before any adoption, and before `newest` is taken: that order is what follows the host's ids
    // starting over. @see the declaration.
    if (sync == CommandBlockSync::Prime || screen.commandBlocksRetiredAt > _syncedCommandBlocks)
        retireAbsentCommandBlocks(screen);

    auto& store = _terminal->commandBlocks();
    // The newest id held before this sync, advanced as records are adopted oldest first. A reported
    // finish only ever happens to the host's newest record (OSC 133;D finishes the current one), so a
    // record this store never held that is OLDER than what it holds cannot be news.
    auto newest = store.currentId().value;

    // What changed since the last sync -- everything, on the attach -- read off the revision index rather
    // than found by walking the table, and adopted in id order, oldest first, as the rule above needs.
    auto const since = sync == CommandBlockSync::Prime ? uint64_t { 0 } : _syncedCommandBlocks;
    auto changed =
        std::ranges::to<std::vector>(std::ranges::subrange(screen.commandBlocksByRevision.upper_bound(since),
                                                           screen.commandBlocksByRevision.end())
                                     | std::views::values);
    std::ranges::sort(changed);

    for (auto const id: changed)
    {
        auto const& mirrored = screen.commandBlocks.at(id); // the index names exactly what the table holds
        // A record the store already holds unchanged is adopted all the same: the store takes it for a
        // resend, and nothing moves.
        auto const* const held = store.find(vtbackend::CommandBlockId { id });
        auto const mode = (sync == CommandBlockSync::Prime || (held == nullptr && id < newest))
                              ? vtbackend::AdoptMode::Snapshot
                              : vtbackend::AdoptMode::Live;
        // A record new to the store has no head here; its rows may be written already (a capped snapshot's
        // remainder follows them) or arrive in this update.
        if (held == nullptr)
            _headCandidates.emplace_back(id);
        // Whether or not this store held it: a head this mirror never placed may have left the host since.
        if (hostHeadOf(mirrored.wire) == HostHead::Evicted)
            _evictedHeads.emplace_back(id);
        // `held` is read before the adoption, which may move the store's records.
        _terminal->adoptCommandBlock(fromWireCommandBlock(mirrored.wire, held), mode);
        newest = std::max(newest, id);
    }
    _syncedCommandBlocks = screen.commandBlocksRevision;
}

void ScreenMirror::syncHostClock(RemoteScreen const& screen)
{
    // Only a reading this mirror has not measured yet: the one a SessionState just brought. A replay later
    // driven from the same RemoteScreen would otherwise measure an old reading against a clock that moved
    // on since, as if the host's clock had fallen behind by however long ago it came.
    if (screen.hostTime == 0 || screen.hostTime == _measuredHostTime)
        return;
    _measuredHostTime = screen.hostTime;
    _terminal->adoptHostClock(fromWireTime(screen.hostTime));
}

void ScreenMirror::retireAbsentCommandBlocks(RemoteScreen const& screen)
{
    // Both sides ascend by id, so one merge walk names what the store holds and the table does not. It
    // runs on every retirement the host makes -- a prompt that ran nothing, and, once the host's store is
    // full, every new prompt, whose delta also retires the oldest -- and usually finds one record or none,
    // as the store's own cap may have dropped the oldest already.
    auto& store = _terminal->commandBlocks();
    auto held = store.records() | std::views::transform(&vtbackend::CommandBlockRecord::id);
    auto stated = screen.commandBlocks | std::views::keys
                  | std::views::transform([](uint32_t id) { return vtbackend::CommandBlockId { id }; });
    auto stale = std::vector<vtbackend::CommandBlockId> {};
    std::ranges::set_difference(held, stated, std::back_inserter(stale));
    store.retire(stale);
}

void ScreenMirror::writeRow(vtbackend::Screen& page,
                            RemoteScreen const& screen,
                            int64_t stableId,
                            int64_t target)
{
    auto const offset = vtbackend::LineOffset::cast_from(target);
    auto const* row = rowOrNull(screen, stableId);
    // A page row is shifted left by the origin, so the window into a wider grid shows the cursor's region; a
    // row of the host's history is not, as the history above the page is where the user reads back from.
    auto const firstColumn = stableId < screen.viewportBase ? vtbackend::ColumnOffset(0) : _origin.column;

    if (offset < vtbackend::LineOffset(0))
    {
        // A row ABOVE the viewport: the server reporting an IN-PLACE change to a scrollback row
        // (@see Grid::changingLineAt — a shell's OSC 133 marks land on a wrapped prompt's head,
        // which sits in history). Bounded by the local history the mirror actually holds, and —
        // unlike a page row — never blanked when the data is missing: this scrollback is the
        // mirror's own, routinely deeper than what the server can still name, so clearing a row
        // the server merely stopped naming would destroy history rather than refresh it.
        if (row == nullptr || offset < -vtbackend::LineOffset::cast_from(page.grid().historyLineCount()))
            return;
        writeLine(page, offset, row, firstColumn);
        return;
    }
    if (offset >= vtbackend::LineOffset::cast_from(page.pageSize().lines))
        return; // the server's page is taller than ours; the extra rows have nowhere to go
    writeLine(page, offset, row, firstColumn);
}

void ScreenMirror::writeLine(vtbackend::Screen& page,
                             vtbackend::LineOffset offset,
                             proto::WireLine const* row,
                             vtbackend::ColumnOffset firstColumn)
{
    // A scrollback row is changed through the accessor that has the grid's scans reach that deep; a page
    // row is always scanned.
    auto& grid = page.grid();
    auto& line = offset < vtbackend::LineOffset(0) ? grid.changingLineAt(offset) : grid.lineAt(offset);
    auto const block = line.blockId();
    auto const before = line.flags();
    if (row != nullptr)
        // Its flags whole, the user's vi mark among them: the host holds that too (see hostRowOf()).
        applyWireLine(line, *row, _linkIds, _contextIds, firstColumn);
    else
        // A row the mirror has no data for is cleared rather than left holding whatever was
        // there before: stale content is worse than a blank line, because it looks correct.
        line.reset(vtbackend::LineFlags {}, vtbackend::GraphicsAttributes {});

    page.noteRowRewritten(offset, block, before);
    // Noted, not looked up: the store is read once the update's rows are all written, by placeHeads().
    if (line.blockId() != vtbackend::CommandBlockId {} && line.flags().any(vtbackend::BlockHeadLineFlags))
        _headCandidates.push_back(line.blockId());
}

void ScreenMirror::placeHeads()
{
    // First: placement passes over a block headed in this generation, which a retired one then is.
    _terminal->retireCommandBlockHeads(_evictedHeads);
    _evictedHeads.clear();
    _terminal->placeCommandBlockHeads(_headCandidates);
    _headCandidates.clear();
}

void ScreenMirror::scrollInRow(vtbackend::Screen& page, RemoteScreen const& screen, int64_t stableId)
{
    // Through Screen::scrollUp rather than Grid::scrollUp: it is what keeps the cursor iterator,
    // the viewport, the Vi cursor and any live selection in step with the rows moving underneath.
    page.scrollUp(vtbackend::LineCount(1));
    writeRow(page, screen, stableId, unbox<int64_t>(page.pageSize().lines) - 1);
}

void ScreenMirror::apply(RemoteScreen const& screen, proto::Delta const& delta)
{
    // Nothing recorded is comparable yet.
    if (!_primed)
    {
        replay(screen, LocalHistory::Discard, ReplayCause::Attach);
        return;
    }
    // A page change ends the comparisons before they start, rather than each one below remembering
    // to opt out. `_generation`, `_floor` and `_viewportBase` describe ONE grid, and each of the 16
    // pages is a distinct grid whose stable ids and generation advance independently (@see
    // NativeSession::pushDelta) — so after a flip the incoming numbers are not in the same space as
    // the recorded ones and mean nothing at all. Reading them anyway let a primary<->alternate flip
    // land on whichever branch the two unrelated id spaces happened to suggest: a spurious Discard
    // truncating deep local scrollback to the daemon's depth where the generations differed, and —
    // where they collided — no rebuild at all for a mirror holding no history for the incoming page.
    //
    // Stated once, here, so a comparison added below inherits it instead of opting in.
    //
    // The program's doing, though the host forces a snapshot to carry it: news (@see frameOf).
    if (screen.screenType != _screenType)
    {
        replay(screen, LocalHistory::Keep, ReplayCause::ScreenSwitch);
        return;
    }
    // A floor that jumped further than the viewport advanced means the server
    // discarded history WITHOUT scrolling it through the page (a `clear`/CSI 3 J,
    // which the incremental path — no line changes, no viewport move — would
    // otherwise leave as ghost scrollback). It is one of the two cases that earn a
    // DISCARDING replay: local scrollback is erased and rebuilt from what the server
    // still holds.
    // Rearranged to avoid signed int64_t overflow: A - B > C - D  ⇔  A + D > C + B.
    // All four values are non-negative line counts, so the sums cannot overflow
    // (INT64_MAX is ~9e18 — no terminal history approaches that).
    auto const floorOutranScroll = screen.stableFloor + _viewportBase > _floor + screen.viewportBase;
    // A generation bump says row identity was destroyed wholesale, but NOT why — and the two reasons
    // want opposite treatment. A RESIZE bumps it because the server reflowed; this terminal reflowed
    // its own copy of the same rows at the same moment, so its history is not wrong, it is the
    // reflowed truth — and discarding it would truncate the user's scrollback to the daemon's depth
    // on every window resize, which fat mode never does. A reset or a history-limit change bumps it
    // because the server really threw those rows away, and then the mirror must follow.
    //
    // Telling them apart needs no wire field: a resize is the case where the announced SIZE moved.
    // (A resize that left this pane's grid alone bumps nothing — @see SessionHost::resizeLocked.)
    auto const sizeChanged = screen.columns != _columns || screen.lines != _lines;
    auto const identityLost = screen.generation != _generation;
    // Only what makes the mirror's OWN history wrong may erase it (@see LocalHistory).
    // Both are the program's doing, whether a snapshot carried them or not.
    if (floorOutranScroll || (identityLost && !sizeChanged))
    {
        replay(screen,
               LocalHistory::Discard,
               floorOutranScroll ? ReplayCause::HistoryCleared : ReplayCause::Reset);
        return;
    }
    // A window into a larger grid that has to move with the cursor rewrites the whole page: every row it
    // shows is another one now, and shifted by another amount.
    auto const origin = [&] {
        auto const guard = std::lock_guard { *_terminal };
        return originFor(screen);
    }();
    if (delta.snapshot != 0 || sizeChanged || screen.viewportBase < _viewportBase || origin != _origin)
    {
        // Named in that order: a resize or a pan riding a snapshot is still the resync.
        auto const cause = [&] {
            if (delta.snapshot != 0)
                return ReplayCause::Resync;
            if (sizeChanged)
                return ReplayCause::Resize;
            return ReplayCause::WindowMoved;
        }();
        replay(screen, LocalHistory::Keep, cause);
        return;
    }

    // Where the page started and starts now, in the host's rows: the base plus the origin, which is
    // unchanged on this path.
    auto const oldBase = pageTop();
    _viewportBase = screen.viewportBase;
    auto const newBase = pageTop();
    _floor = screen.stableFloor;

    {
        auto const guard = std::lock_guard { *_terminal };
        syncHyperlinks(screen); // before any row: a cell's link id is unresolvable without it
        syncContexts(screen);   // and before any row for the same reason: so is a line's context id
        syncHostClock(screen);  // before any record: it says how to read the record's instants
        // And before any row for the same reason: a head row needs its record.
        syncCommandBlocks(screen, CommandBlockSync::Follow);
        auto& page = activePage();
        auto const lines = unbox<int64_t>(page.pageSize().lines);

        // 1. Overwrite changed rows at their old positions — including rows about to scroll into
        //    history, so local history keeps their final content, AND rows already IN history that
        //    the server changed in place (@see Grid::changingLineAt: OSC 133 marks land on a
        //    logical line's head, which for a wrapped prompt has scrolled off the page). Bounding
        //    this at the old viewport top dropped those silently, so semantic-block selection and
        //    prompt navigation over such a command answered from stale marks in attach mode while
        //    the same session in a local window behaved correctly.
        //
        //    `_alignedFloor` is how far up the arithmetic offset `stableId - oldBase` is still the
        //    row the id names; below it the mirror's history no longer corresponds to the server's
        //    ids at all and the row is skipped rather than written somewhere wrong.
        for (auto const& row: delta.lines)
            if (row.stableId >= _alignedFloor && row.stableId < oldBase + lines)
                writeRow(page, screen, row.stableId, row.stableId - oldBase);

        // 2. Scroll the viewport advance in, row by row, so rows that pass straight through
        //    (entering and leaving within one update) still land in local history.
        //
        //    BOUNDED, because the iteration count is peer-announced: `stableViewportBase` is a wire
        //    field, and each row costs a Screen::scrollUp plus a writeRow — on the reactor thread,
        //    under the terminal lock — so a base naming a far-away row would freeze the window for
        //    as long as the arithmetic says. The bound loses nothing: a row is skipped only when it
        //    is BOTH older than the deepest row this terminal will still be holding when the loop
        //    ends (evicted by the rows behind it) AND unnamed by the server, which means it would
        //    have been written as a blank. A peer that wants rows in the mirror's history has to
        //    send them, so the work stays proportional to the bytes it paid for.
        auto const advanceTop = oldBase + lines;
        auto const named = screen.rows.lower_bound(advanceTop);
        auto const oldestNamed = named != screen.rows.end() ? named->first : newBase + lines;
        auto const retainable = newBase - unbox<int64_t>(page.grid().maxHistoryLineCount());
        auto const scrollFrom = std::max(advanceTop, std::min(oldestNamed, retainable));
        if (scrollFrom > advanceTop)
            // Everything below was pushed out of both the page and local history by the rows about
            // to stream in, so nothing older sits at its arithmetic offset any more.
            _alignedFloor = scrollFrom;
        for (auto const id: std::views::iota(scrollFrom, newBase + lines))
            scrollInRow(page, screen, id);
        placeHeads();

        syncInputEncoding(screen);
        if (delta.titleChanged != 0)
            _terminal->setWindowTitle(delta.title);
        if (delta.cursorShapeChanged != 0)
            applyCursorStyle(delta.cursorShape);
        if (delta.cwdChanged != 0)
            _terminal->setCurrentWorkingDirectory(delta.cwd);
        if (delta.colorsChanged != 0)
        {
            _terminal->colorPalette().defaultForeground = vtbackend::RGBColor { delta.defaultForeground };
            _terminal->colorPalette().defaultBackground = vtbackend::RGBColor { delta.defaultBackground };
        }
        if (delta.statusChanged != 0)
            applyStatusDisplay(delta.statusDisplayType, delta.activeStatusDisplay);
        if (delta.statusLinesChanged != 0)
            applyStatusLines(screen);
        if (delta.kittyKeyboardChanged != 0)
            _terminal->changeKeyboardProtocol([&](auto& protocol) {
                protocol.flags() = vtbackend::KeyboardEventFlags::fromValue(delta.kittyKeyboardFlags);
            });
        if (delta.modifyOtherKeysChanged != 0)
            _terminal->setModifyOtherKeys(delta.modifyOtherKeys);
        if (delta.progressChanged != 0)
            // Validated rather than cast, per the rule in vthost/ProgressWire.hpp: a state no
            // enumerator has is ignored, leaving what the mirror already shows standing.
            if (auto const progress = progressOf(delta.progressState, delta.progressPercentage))
                _terminal->setProgress(*progress);
        applyImages(screen);
        finish(screen);
        anchorPrimaryPage(screen);
    }
    // Outside the lock, exactly where Terminal::processInputOnce publishes a parsed chunk.
    _terminal->screenUpdated();
}

void ScreenMirror::fullReplay(RemoteScreen const& screen, LocalHistory history)
{
    replay(screen, history, ReplayCause::Attach);
}

void ScreenMirror::replay(RemoteScreen const& screen, LocalHistory history, ReplayCause cause)
{
    // Decided before `_primed` flips: the first replay is the attach, and nothing it shows is news.
    auto const blockSync = _primed ? CommandBlockSync::Follow : CommandBlockSync::Prime;
    _primed = true;
    _generation = screen.generation;
    _viewportBase = screen.viewportBase;
    _floor = screen.stableFloor;
    _columns = screen.columns;
    _lines = screen.lines;

    {
        auto const guard = std::lock_guard { *_terminal };
        syncHyperlinks(screen); // before any row: a cell's link id is unresolvable without it
        syncContexts(screen);   // and before any row for the same reason: so is a line's context id
        syncHostClock(screen);  // before any record: it says how to read the record's instants
        // And before any row for the same reason: a head row needs its record.
        syncCommandBlocks(screen, blockSync);
        // Not grid state, but a replay re-asserts everything: after a reconnect the set the old connection
        // left may be one the host has since emptied.
        syncProgramStatus(screen.programStatus);
        // The page has to be the right one BEFORE _screenType is adopted: activePage() reads it.
        auto const wantAlternate = screen.screenType == 1;
        if (_terminal->isAlternateScreen() != wantAlternate)
            _terminal->setMode(vtbackend::DECMode::ExtendedAltScreen, wantAlternate);
        _screenType = screen.screenType;
        // With the page picked, as the page's size is what the window into the host's grid is cut to.
        _origin = originFor(screen);

        auto& page = activePage();
        auto const lines = unbox<int64_t>(page.pageSize().lines);
        auto const top = pageTop();
        // Only the primary screen HAS local scrollback; the alternate screen keeps none, so there
        // is nothing there to rebuild and nothing to preserve.
        //
        // Rebuild whenever there is no local history to protect: every Discard, and also a Keep
        // whose mirror never had any. Keep preserves history, it does not decline to HAVE any — a
        // mirror primed while the session sat on the alternate screen owns no primary history at
        // all, so repainting only the page on the way back would drop the scrollback the snapshot
        // DID carry. Where history IS present, re-streaming would append a SECOND copy below the
        // rows already there, which is exactly what Keep exists to avoid.
        //
        // Decided BEFORE the clear below rather than by probing the grid afterwards: the emptiness
        // that a Discard produces is this function's own doing, and inferring policy from a side
        // effect it just caused would silently change meaning if anything were inserted between.
        auto const rebuildHistory = !wantAlternate
                                    && (history == LocalHistory::Discard
                                        || page.grid().historyLineCount() == vtbackend::LineCount(0));
        if (history == LocalHistory::Discard && !wantAlternate)
        {
            // The history goes as ED 3 takes it on the host, and whatever names its rows goes with it -- even
            // with no history to drop, since every row the replay keeps is rewritten below.
            page.grid().clearHistory();
            _terminal->rowsDropped(vtbackend::LineCount(0));
        }
        if (rebuildHistory)
        {
            // The rows below come back under ids this grid mints anew, while its stable-id generation stays
            // put (Grid::clearHistory() is deliberately no bump), so a head would go on naming whatever row
            // now holds its id -- of its own block, maybe, but not the row the rule picks. Every head is
            // forgotten and looked for again among the rows streamed, as after a reflow.
            auto& store = _terminal->commandBlocks();
            store.forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
                store.updateHeadPosition(record.id, 0, vtbackend::UnknownHeadGeneration);
                _headCandidates.push_back(record.id);
                // No row streamed below can head a block whose host head row is gone.
                if (auto const mirrored = screen.commandBlocks.find(record.id.value);
                    mirrored != screen.commandBlocks.end()
                    && hostHeadOf(mirrored->second.wire) == HostHead::Evicted)
                    _evictedHeads.push_back(record.id);
            });
        }

        // Stream history (then the viewport) through the page top-down: once the page is full,
        // each further row scrolls a REAL row into scrollback, so local history matches remote
        // history with no filler.
        auto const firstId = screen.rows.empty() ? screen.viewportBase
                                                 : std::min(screen.rows.begin()->first, screen.viewportBase);
        if (rebuildHistory && firstId < screen.viewportBase)
        {
            auto row = int64_t { 0 };
            auto const stream = [&](int64_t id) {
                if (row < lines)
                    writeRow(page, screen, id, row++);
                else
                    scrollInRow(page, screen, id);
            };
            // The host's history, then the window of its page this mirror shows: the page rows above the
            // window are on the host's page, not in its history, and are not copied into this one.
            for (auto const id: std::views::iota(firstId, screen.viewportBase))
                stream(id);
            for (auto const id: std::views::iota(top, top + lines))
                stream(id);
            // Everything from firstId down was just streamed through the page, so the whole of it
            // sits at its arithmetic offset -- unless the window skipped the rows above it, which moves
            // every history row that many lines off it.
            _alignedFloor = _origin.line == vtbackend::LineOffset(0) ? firstId : top;
        }
        else
        {
            for (auto const row: std::views::iota(int64_t { 0 }, lines))
                writeRow(page, screen, top + row, row);
            // Only the page was rewritten. Whatever local history sits above it was NOT re-streamed
            // and may no longer line up with the server's ids — a resize is the ordinary case, and
            // there this terminal reflowed its own history into a different set of rows entirely,
            // for which the server's ids have no counterpart. So nothing above the new base is
            // addressable by id; rows scrolling in from here on are, and they all sit below it.
            _alignedFloor = top;
        }
        placeHeads();

        applySessionState(screen);
        applyStatusLines(screen);
        syncInputEncoding(screen);
        applyImages(screen);
        finish(screen);
        anchorPrimaryPage(screen);
    }
    // A repaint of what the host showed already -- the attach, a resync, a resize -- is not output; what the
    // program did to its screen is.
    switch (frameOf(cause))
    {
        case MirrorFrame::Replay: _terminal->screenReplayed(); break;
        case MirrorFrame::Output: _terminal->screenUpdated(); break;
    }
}

std::optional<proto::RowAddress> ScreenMirror::hostRowOf(int64_t stableId, uint64_t generation) const
{
    auto const guard = std::lock_guard { *_terminal };
    auto const& grid = _terminal->primaryScreen().grid();
    // A reflow of this grid since the last update renamed every row: until the host's reflowed page comes,
    // no id here lines up with one there.
    if (_screenType != 0 || !_primaryAnchor || grid.stableIdGeneration() != generation
        || _primaryAnchor->mirrorIdGeneration != generation || !grid.lineOffsetOf(stableId))
        return std::nullopt;
    // The anchor's line held the host row at the page's top (pageTop()), and every row down to the aligned
    // floor holds the one its distance from it names.
    auto const hostRow = pageTop() + (stableId - _primaryAnchor->mirrorBase);
    if (hostRow < _alignedFloor)
        return std::nullopt;
    return proto::RowAddress { .stableId = hostRow, .generation = _primaryAnchor->hostIdGeneration };
}

void ScreenMirror::anchorPrimaryPage(RemoteScreen const& screen) noexcept
{
    if (_screenType != 0)
        return;
    auto const& grid = _terminal->primaryScreen().grid();
    _primaryAnchor = PageAnchor { .mirrorBase = grid.stableLineIdOf(vtbackend::LineOffset(0)),
                                  .mirrorIdGeneration = grid.stableIdGeneration(),
                                  .hostIdGeneration = screen.stableIdGeneration };
}

void ScreenMirror::applySessionState(RemoteScreen const& screen)
{
    // Every value here is asserted, defaults included — see the class doc for why a value that
    // returned to its default inside a snapshot is never announced again. The defaults are all
    // reachable states an app arrives at by giving something up: DECSCUSR 0, DECSSDT/DECSASD 0, a
    // popped CSI-u stack, modifyOtherKeys level 0.
    //
    // Asserted, but only ever WRITTEN when the terminal does not already hold it. That is not an
    // optimization: these setters have side effects far beyond the field they name, and a replay
    // runs on every resize and every screen flip. `setStatusDisplay` RESIZES the screen when the
    // status line appears or goes, which fires the pty's resize sink straight back into the
    // controller; `setWindowTitle` posts a tab-strip refresh onto the GUI thread. Re-asserting a
    // value the terminal already holds fires all of that for no change at all. Comparing against
    // the terminal is exact — if it already equals the wire's value, writing it is a no-op by
    // definition — so nothing about what a snapshot can deliver is lost.
    if (_terminal->windowTitle() != screen.title)
        _terminal->setWindowTitle(screen.title);
    if (decscusrPs(_terminal->cursorShape(), _terminal->cursorDisplay()) != screen.cursorShape)
        applyCursorStyle(screen.cursorShape);
    _terminal->colorPalette().defaultForeground = vtbackend::RGBColor { screen.defaultForeground };
    _terminal->colorPalette().defaultBackground = vtbackend::RGBColor { screen.defaultBackground };
    applyStatusDisplay(screen.statusDisplayType, screen.activeStatusDisplay);
    _terminal->changeKeyboardProtocol([&](auto& protocol) {
        protocol.flags() = vtbackend::KeyboardEventFlags::fromValue(screen.kittyKeyboardFlags);
    });
    if (_terminal->modifyOtherKeys() != screen.modifyOtherKeys)
        _terminal->setModifyOtherKeys(screen.modifyOtherKeys);
    // The working directory is the exception: an empty one is "not known", not "the root", and
    // overwriting a known cwd with it would lose information rather than restore a default.
    if (!screen.cwd.empty() && _terminal->currentWorkingDirectory() != screen.cwd)
        _terminal->setCurrentWorkingDirectory(screen.cwd);
    // The re-attach replay: a client joining mid-operation adopts the bar already in flight. Compared
    // against the terminal first for the same reason the title is — setProgress announces to the
    // frontend, and re-announcing an unchanged value is work for no change at all.
    if (auto const progress = progressOf(screen.progressState, screen.progressPercentage);
        progress.has_value() && _terminal->progress() != *progress)
        _terminal->setProgress(*progress);
    // The rows carry their birth stamps as seconds since the HOST's session began, so they name the
    // right instants only when counted from the host's epoch. Zero is "never stated" (a bare test
    // screen), not 1970. Compared first, like everything above: a replay runs on every resize.
    if (screen.sessionEpoch != 0)
        if (auto const epoch = fromWireTime(screen.sessionEpoch); _terminal->sessionEpoch() != epoch)
            _terminal->adoptSessionEpoch(epoch);
}

void ScreenMirror::applyStatusDisplay(uint8_t wireType, uint8_t wireActive)
{
    // The richer of what the application asked for and what this terminal was configured with. The
    // enum is ordered None < Indicator < HostWritable, so this reads as: a status line is shown if
    // EITHER the viewer configured one or the app asked for one, and the app's richer request wins.
    // Taking the wire value alone would let a daemon hosting without a status line switch the
    // viewer's own indicator line off — losing the user's configuration AND resizing the pane, since
    // the line costs a row. @see the declaration.
    // Both values are validated rather than cast: they are bytes a peer chose, and an out-of-range
    // StatusDisplayType reaches Terminal::statusLineHeight(), whose switch ends in
    // core::unreachable() — undefined behaviour in this process. A value no enumerator has is
    // treated as "the session says nothing", which leaves this terminal's own configuration
    // standing. @see vthost/StatusWire.h.
    auto const asked = statusDisplayTypeOf(wireType).value_or(vtbackend::StatusDisplayType::None);
    auto const want = std::max(asked, _ownStatusDisplayType);
    if (_terminal->statusDisplayType() != want)
        _terminal->setStatusDisplay(want);
    if (auto const active = activeStatusDisplayOf(wireActive);
        active.has_value() && _terminal->activeStatusDisplay() != *active)
        _terminal->setActiveStatusDisplay(*active);
}

void ScreenMirror::applyCursorStyle(uint8_t ps)
{
    // Ps 0 means "whatever this terminal was configured with", which only the receiving side knows.
    if (auto const* style = cursorStyleOf(ps))
        _terminal->setCursorStyle(style->display, style->shape);
    else
        _terminal->setCursorStyle(_terminal->factorySettings().cursorDisplay,
                                  _terminal->factorySettings().cursorShape);
}

void ScreenMirror::syncInputEncoding(RemoteScreen const& screen)
{
    // The DEC modes first. Every one of them is an INDEPENDENT boolean (@see vthost/MirroredModes.h
    // for why that is a rule and not an observation), so applying the whole set is well-defined
    // whatever order it runs in, and no memory of the previous set is needed to get it right.
    //
    // The comparison is against the TERMINAL, not against the last set seen on the wire. That is
    // what keeps this correct on a first sync: a mirror built while an application was already
    // running has no previous set to diff against, and the old code answered that by applying every
    // row — including the disabled ones, which is precisely what a mode that is not independent
    // cannot survive.
    for (auto const mode: MirroredModes)
    {
        auto const want = std::ranges::contains(screen.setModes, vtbackend::toDECModeNum(mode));
        if (want != _terminal->isModeEnabled(mode))
            _terminal->setMode(mode, want);
    }

    // The ANSI modes, off their own field for the reason MirroredModes.h gives (ANSI 20 and DEC 20
    // are different modes). LNM is the whole table today, and it matters here specifically because
    // in attach mode the CLIENT encodes the keystroke: without this, Return sends a bare CR to an
    // application that asked for CR LF, so its input lines are never submitted and it looks hung —
    // while the same application in a local tab works.
    for (auto const mode: MirroredAnsiModes)
    {
        auto const want = std::ranges::contains(screen.setAnsiModes, vtbackend::toAnsiModeNum(mode));
        if (want != _terminal->isModeEnabled(mode))
            _terminal->setMode(mode, want);
    }

    // The mouse state AFTER the modes, and never through setMode. Mode handlers write into it:
    // DECCKM picks the form of the wheel's cursor keys, and ?1007 and every protocol change derive
    // the wheel mode afresh -- as does setMouseProtocol() below. Applying the resolved values last
    // makes the server's answer the one that survives.
    //
    // Asserted unconditionally, defaults included, for the reason applySessionState gives: the
    // server states this outright on every snapshot and then stays quiet, so a mirror that only
    // wrote non-default values would lose a return to the default permanently. The setters are
    // plain field writes, so re-asserting costs nothing.
    auto const protocol = mouseProtocolOf(screen.mouse.protocol);
    // When disabling, the protocol argument is ignored — InputGenerator clears whichever protocol
    // is active — so its value matters only in the enabling case.
    _terminal->setMouseProtocol(protocol.value_or(vtbackend::MouseProtocol::NormalTracking),
                                protocol.has_value());
    _terminal->setMouseTransport(mouseTransportOf(screen.mouse.transport));
    _terminal->setMouseWheelMode(mouseWheelModeOf(screen.mouse.wheelMode));
}

void ScreenMirror::applyStatusLines(RemoteScreen const& screen)
{
    // Validated rather than cast, as the status display's own bytes are (@see vthost/StatusWire.hpp): rows
    // for a display no enumerator names are not painted anywhere.
    auto const display = statusDisplayTypeOf(screen.statusLinesDisplay);
    if (display == vtbackend::StatusDisplayType::Indicator)
    {
        if (screen.statusLines.empty())
        {
            _terminal->setMirroredIndicatorStatusLine(std::nullopt);
            return;
        }
        auto row = vtbackend::Line(_terminal->indicatorStatusLineDisplay().pageSize().columns);
        applyWireLine(row, screen.statusLines.front(), _linkIds, _contextIds);
        _terminal->setMirroredIndicatorStatusLine(std::move(row));
        return;
    }
    // The host's indicator row is not what the host shows any more: the application's status line is.
    _terminal->setMirroredIndicatorStatusLine(std::nullopt);
    if (display != vtbackend::StatusDisplayType::HostWritable || screen.statusLines.empty())
        return;
    auto& page = _terminal->hostWritableStatusLineDisplay();
    auto const lines = unbox<std::size_t>(page.pageSize().lines);
    for (auto const [row, line]: core::views::enumerate(screen.statusLines))
    {
        if (static_cast<std::size_t>(row) >= lines)
            break;
        applyWireLine(page.grid().lineAt(vtbackend::LineOffset::cast_from(row)), line, _linkIds, _contextIds);
    }
}

void ScreenMirror::applyImages(RemoteScreen const& screen)
{
    if (screen.imageCells.empty() && _images.empty())
        return;

    auto& page = activePage();
    auto const base = pageTop();
    auto const lines = unbox<int64_t>(page.pageSize().lines);

    for (auto const& [imageId, placement]: placementsOf(screen))
    {
        if (!placement.anchored)
            continue; // the image's top scrolled out of what the mirror holds; a resync re-places it
        auto const* data = screen.imageData(imageId);
        if (data == nullptr)
            continue; // pixels not fetched yet; the image handler places it once they land

        auto local = _images.find(imageId);
        if (local == _images.end())
        {
            // The wire types pixels as std::byte, the image pool as unsigned char; same bits,
            // different spelling, so the copy is explicit rather than a reinterpret_cast.
            auto pixels = vtbackend::Image::Data(data->data.size());
            std::ranges::transform(
                data->data, pixels.begin(), [](std::byte raw) { return std::to_integer<uint8_t>(raw); });
            auto image = _terminal->imagePool().create(
                imageFormatOf(data->format),
                vtbackend::ImageSize { vtbackend::Width::cast_from(data->width),
                                       vtbackend::Height::cast_from(data->height) },
                std::move(pixels));
            if (!image)
                continue;
            local = _images.emplace(imageId, std::move(image)).first;
        }

        // Rasterized against OUR cell size, not the sender's: the pixels are the shared truth, the
        // grid they are cut into is per-display. The alignment and resize policies DO come off the
        // wire — they decide how the source is cropped and stretched into the cells, so deriving
        // them locally would show a differently framed image.
        //
        // Through vthost/ImageWire.h rather than a cast: these bytes are unvalidated wire input (the
        // codec carries them unjudged, exactly as it does the mouse state), and the switches they
        // reach in Image.cpp end in std::unreachable() — once per frame, on the render thread, for
        // every attached pane. A peer's byte becomes an enumerator in one place, or not at all.
        auto rasterized =
            vtbackend::rasterize(local->second,
                                 imageAlignmentOf(placement.alignment),
                                 imageResizeOf(placement.resize),
                                 _terminal->colorPalette().defaultBackground,
                                 vtbackend::GridSize { vtbackend::LineCount::cast_from(placement.rows),
                                                       vtbackend::ColumnCount::cast_from(placement.columns) },
                                 _terminal->cellPixelSize(),
                                 imageLayerOf(placement.layer));

        for (auto const& [stableId, columns]: screen.imageCells)
        {
            auto const target = stableId - base;
            if (target < 0 || target >= lines)
                continue;
            auto& line = page.grid().lineAt(vtbackend::LineOffset::cast_from(target));
            for (auto const& [column, entry]: columns)
            {
                // Shifted as the page row it lies on is (@see writeRow).
                auto const at = vtbackend::ColumnOffset::cast_from(column) - _origin.column;
                if (entry.imageId != imageId || at < vtbackend::ColumnOffset(0)
                    || at >= vtbackend::ColumnOffset::cast_from(line.size()))
                    continue;
                line.useCellAt(at).setImageFragment(
                    rasterized,
                    vtbackend::CellLocation { .line = vtbackend::LineOffset::cast_from(entry.offsetLine),
                                              .column =
                                                  vtbackend::ColumnOffset::cast_from(entry.offsetColumn) });
            }
        }
    }
}

void ScreenMirror::applyImage(RemoteScreen const& screen, uint32_t imageId)
{
    {
        auto const guard = std::lock_guard { *_terminal };
        if (screen.imageData(imageId) == nullptr)
            // Dropped server-side: forget the local copy so a reused id can never show stale
            // pixels. The cells referencing it were already cleared by RemoteScreen::dropImage
            // and repainted by the delta that carried it.
            _images.erase(imageId);
        applyImages(screen);
        finish(screen);
    }
    _terminal->screenUpdated();
}

void ScreenMirror::syncProgramStatus(std::span<proto::WireProgramStatusRecord const> records)
{
    auto converted = std::vector<vtbackend::ProgramStatusRecord> {};
    converted.reserve(records.size());
    for (auto const& wire: records)
        if (auto record = control::fromWire(wire))
            converted.push_back(std::move(*record));
    _terminal->replaceProgramStatus(std::move(converted));
}

void ScreenMirror::applyEvent(proto::SessionEventPdu const& event)
{
    // One overload per event shape, so each reads only the fields it has. The visit is exhaustive by
    // construction: a fourth event alternative fails to compile until it gets its own arm.
    std::visit(core::Overloaded {
                   [this](proto::SessionBell const&) { _terminal->bell(); },
                   [this](proto::SessionNotify const& notify) {
                       // An OSC 99 notification is raised as a local one is, so the frontend decides its
                       // occasion by the same rule; one that stated none takes the OSC 9 / OSC 777 path and
                       // that rule.
                       auto const occasion = displayOccasionOf(notify.occasion);
                       if (!occasion)
                       {
                           _terminal->notify(notify.title, notify.body);
                           return;
                       }
                       auto notification = vtbackend::DesktopNotification {};
                       notification.title = notify.title;
                       notification.body = notify.body;
                       notification.occasion = *occasion;
                       _terminal->showDesktopNotification(notification);
                   },
                   [this](proto::SessionClipboard const& clipboard) {
                       // Under the CLIENT's own write permission, which is the point of routing it
                       // through the terminal rather than to the clipboard directly.
                       _terminal->copyToClipboard(clipboard.data);
                   },
                   [this](proto::ProgramStatus const& status) {
                       auto const guard = std::lock_guard { *_terminal };
                       syncProgramStatus(status.records);
                   },
               },
               event);
}

void ScreenMirror::finish(RemoteScreen const& screen)
{
    // After every replay and every delta, since either may have moved this grid's floor: its absolute line
    // numbers are the server's, whatever its own floor saw.
    auto& grid = activePage().grid();
    grid.adoptEvictedRowCount(mirroredEvictedRowCount(screen, pageTop(), grid.addressableTop()));
    // Where the host's cursor is in the window this mirror shows -- inside it, as viewportOrigin keeps it.
    activePage().moveCursorTo(vtbackend::LineOffset::cast_from(screen.cursorLine) - _origin.line,
                              vtbackend::ColumnOffset::cast_from(screen.cursorColumn) - _origin.column);
    // And the application hears of a click on the cell it drew there, not on this terminal's own cell.
    _terminal->setReportedCellOrigin(_origin);
    // A view scrolled back follows the remote screen's output as it would a local one's: under the lock,
    // before the update is published.
    _terminal->followOutput();
    _terminal->markScreenDirty();
}

} // namespace vthost::client
