// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ScreenMirror.hpp>

#include <vtbackend/Color.hpp>
#include <vtbackend/Hyperlink.hpp>
#include <vtbackend/Image.hpp>
#include <vtbackend/primitives.hpp>

#include <crispy/overloaded.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

#include <vthost/CursorStyle.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/ImageWire.hpp>
#include <vthost/MirroredModes.hpp>
#include <vthost/MouseWire.hpp>
#include <vthost/StatusWire.hpp>

namespace vthost::client
{

namespace
{
    /// std::ranges::contains is not yet in Apple's libc++; find() is.
    [[nodiscard]] bool containsValue(std::vector<uint32_t> const& values, uint32_t needle)
    {
        return std::ranges::find(values, needle) != values.end();
    }

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
} // namespace

vtbackend::Screen& ScreenMirror::activePage() const noexcept
{
    return _screenType == 1 ? _terminal->alternateScreen() : _terminal->primaryScreen();
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

void ScreenMirror::writeRow(vtbackend::Screen& page,
                            RemoteScreen const& screen,
                            int64_t stableId,
                            int64_t target)
{
    auto const offset = vtbackend::LineOffset::cast_from(target);
    auto const* row = rowOrNull(screen, stableId);

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
        applyWireLine(page.grid().changingLineAt(offset), *row, _linkIds);
        return;
    }
    if (offset >= vtbackend::LineOffset::cast_from(page.pageSize().lines))
        return; // the server's page is taller than ours; the extra rows have nowhere to go
    auto& line = page.grid().lineAt(offset);
    if (row != nullptr)
        applyWireLine(line, *row, _linkIds);
    else
        // A row the mirror has no data for is cleared rather than left holding whatever was
        // there before: stale content is worse than a blank line, because it looks correct.
        line.reset(vtbackend::LineFlags {}, vtbackend::GraphicsAttributes {});
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
        fullReplay(screen, LocalHistory::Discard);
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
    if (screen.screenType != _screenType)
    {
        fullReplay(screen, LocalHistory::Keep);
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
    if (floorOutranScroll || (identityLost && !sizeChanged))
    {
        fullReplay(screen, LocalHistory::Discard);
        return;
    }
    if (delta.snapshot != 0 || sizeChanged || screen.viewportBase < _viewportBase)
    {
        fullReplay(screen, LocalHistory::Keep);
        return;
    }

    auto const oldBase = _viewportBase;
    auto const newBase = screen.viewportBase;
    _viewportBase = newBase;
    _floor = screen.stableFloor;

    {
        auto const guard = std::lock_guard { *_terminal };
        syncHyperlinks(screen); // before any row: a cell's link id is unresolvable without it
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
            _terminal->keyboardProtocol().flags() =
                vtbackend::KeyboardEventFlags::from_value(delta.kittyKeyboardFlags);
        if (delta.modifyOtherKeysChanged != 0)
            _terminal->setModifyOtherKeys(delta.modifyOtherKeys);
        applyImages(screen);
        finish(screen);
    }
    // Outside the lock, exactly where Terminal::processInputOnce publishes a parsed chunk.
    _terminal->screenUpdated();
}

void ScreenMirror::fullReplay(RemoteScreen const& screen, LocalHistory history)
{
    _primed = true;
    _generation = screen.generation;
    _viewportBase = screen.viewportBase;
    _floor = screen.stableFloor;
    _columns = screen.columns;
    _lines = screen.lines;

    {
        auto const guard = std::lock_guard { *_terminal };
        syncHyperlinks(screen); // before any row: a cell's link id is unresolvable without it
        // The page has to be the right one BEFORE _screenType is adopted: activePage() reads it.
        auto const wantAlternate = screen.screenType == 1;
        if (_terminal->isAlternateScreen() != wantAlternate)
            _terminal->setMode(vtbackend::DECMode::ExtendedAltScreen, wantAlternate);
        _screenType = screen.screenType;

        auto& page = activePage();
        auto const lines = unbox<int64_t>(page.pageSize().lines);
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
            page.grid().clearHistory();
            _terminal->scrollbackBufferCleared();
        }

        // Stream history (then the viewport) through the page top-down: once the page is full,
        // each further row scrolls a REAL row into scrollback, so local history matches remote
        // history with no filler.
        auto const firstId = screen.rows.empty() ? screen.viewportBase
                                                 : std::min(screen.rows.begin()->first, screen.viewportBase);
        if (rebuildHistory && firstId < screen.viewportBase)
        {
            auto row = int64_t { 0 };
            for (auto const id: std::views::iota(firstId, screen.viewportBase + lines))
            {
                if (row < lines)
                    writeRow(page, screen, id, row++);
                else
                    scrollInRow(page, screen, id);
            }
            // Everything from firstId down was just streamed through the page, so the whole of it
            // sits at its arithmetic offset.
            _alignedFloor = firstId;
        }
        else
        {
            for (auto const row: std::views::iota(int64_t { 0 }, lines))
                writeRow(page, screen, screen.viewportBase + row, row);
            // Only the page was rewritten. Whatever local history sits above it was NOT re-streamed
            // and may no longer line up with the server's ids — a resize is the ordinary case, and
            // there this terminal reflowed its own history into a different set of rows entirely,
            // for which the server's ids have no counterpart. So nothing above the new base is
            // addressable by id; rows scrolling in from here on are, and they all sit below it.
            _alignedFloor = screen.viewportBase;
        }

        applySessionState(screen);
        applyStatusLines(screen);
        syncInputEncoding(screen);
        applyImages(screen);
        finish(screen);
    }
    _terminal->screenUpdated();
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
    _terminal->keyboardProtocol().flags() =
        vtbackend::KeyboardEventFlags::from_value(screen.kittyKeyboardFlags);
    if (_terminal->modifyOtherKeys() != screen.modifyOtherKeys)
        _terminal->setModifyOtherKeys(screen.modifyOtherKeys);
    // The working directory is the exception: an empty one is "not known", not "the root", and
    // overwriting a known cwd with it would lose information rather than restore a default.
    if (!screen.cwd.empty() && _terminal->currentWorkingDirectory() != screen.cwd)
        _terminal->setCurrentWorkingDirectory(screen.cwd);
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
    // crispy::unreachable() — undefined behaviour in this process. A value no enumerator has is
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
        auto const want = containsValue(screen.setModes, vtbackend::toDECModeNum(mode));
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
        auto const want = containsValue(screen.setAnsiModes, vtbackend::toAnsiModeNum(mode));
        if (want != _terminal->isModeEnabled(mode))
            _terminal->setMode(mode, want);
    }

    // The mouse state AFTER the modes, and never through setMode. Two mode handlers write into it:
    // DECCKM rewrites the wheel mode while the alternate screen is up, and enabling a protocol
    // resets the wheel mode to Default. Applying the resolved values last makes the server's answer
    // the one that survives.
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
    if (screen.statusLines.empty())
        return;
    auto& page = _terminal->hostWritableStatusLineDisplay();
    auto const lines = unbox<std::size_t>(page.pageSize().lines);
    for (auto const [row, line]: crispy::views::enumerate(screen.statusLines))
    {
        if (static_cast<std::size_t>(row) >= lines)
            break;
        applyWireLine(page.grid().lineAt(vtbackend::LineOffset::cast_from(row)), line, _linkIds);
    }
}

void ScreenMirror::applyImages(RemoteScreen const& screen)
{
    if (screen.imageCells.empty() && _images.empty())
        return;

    auto& page = activePage();
    auto const base = _viewportBase;
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
                if (entry.imageId != imageId
                    || vtbackend::ColumnOffset::cast_from(column)
                           >= vtbackend::ColumnOffset::cast_from(line.size()))
                    continue;
                line.useCellAt(vtbackend::ColumnOffset::cast_from(column))
                    .setImageFragment(rasterized,
                                      vtbackend::CellLocation {
                                          .line = vtbackend::LineOffset::cast_from(entry.offsetLine),
                                          .column = vtbackend::ColumnOffset::cast_from(entry.offsetColumn) });
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

void ScreenMirror::applyEvent(proto::SessionEventPdu const& event)
{
    // One overload per event shape, so each reads only the fields it has. The visit is exhaustive by
    // construction: a fourth event alternative fails to compile until it gets its own arm.
    std::visit(
        overloaded {
            [this](proto::SessionBell const&) { _terminal->bell(); },
            [this](proto::SessionNotify const& notify) { _terminal->notify(notify.title, notify.body); },
            [this](proto::SessionClipboard const& clipboard) {
                // Under the CLIENT's own write permission, which is the point of routing it
                // through the terminal rather than to the clipboard directly.
                _terminal->copyToClipboard(clipboard.data);
            },
        },
        event);
}

void ScreenMirror::finish(RemoteScreen const& screen)
{
    activePage().moveCursorTo(vtbackend::LineOffset::cast_from(screen.cursorLine),
                              vtbackend::ColumnOffset::cast_from(screen.cursorColumn));
    _terminal->markScreenDirty();
}

} // namespace vthost::client
