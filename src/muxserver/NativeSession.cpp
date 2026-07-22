// SPDX-License-Identifier: Apache-2.0
#include <muxserver/NativeSession.h>

#include <vtbackend/Image.h>
#include <vtbackend/Line.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <muxserver/MirroredModes.h>
#include <muxserver/PduPump.h>
#include <net/Sockets.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

namespace muxserver
{

using namespace std::chrono_literals;
using vtmux::SessionId;

namespace
{
    [[nodiscard]] uint32_t rawColor(vtbackend::Color color) noexcept
    {
        static_assert(sizeof(vtbackend::Color) == sizeof(uint32_t));
        return std::bit_cast<uint32_t>(color);
    }

    /// The DECSCUSR Ps value (1 blink block … 6 steady bar) for a cursor shape and
    /// blink state — what a client re-emits as `CSI Ps SP q`. Rectangle (a Contour
    /// extension DECSCUSR cannot name) degrades to block.
    [[nodiscard]] uint8_t decscusrPs(vtbackend::CursorShape shape, vtbackend::CursorDisplay display) noexcept
    {
        auto const blink = display == vtbackend::CursorDisplay::Blink;
        switch (shape)
        {
            case vtbackend::CursorShape::Block:
            case vtbackend::CursorShape::Rectangle: return blink ? 1 : 2;
            case vtbackend::CursorShape::Underscore: return blink ? 3 : 4;
            case vtbackend::CursorShape::Bar: return blink ? 5 : 6;
        }
        return 2;
    }

    /// Serializes one split-tree node onto the wire (recurses into its children).
    [[nodiscard]] proto::WirePane serializePaneTree(vtmux::Pane const& pane)
    {
        auto wire = proto::WirePane {};
        wire.paneId = pane.id().value;
        wire.split = std::to_underlying(pane.splitState());
        if (pane.isLeaf())
            wire.session = pane.session().value;
        else
        {
            wire.ratio = static_cast<uint16_t>((pane.ratio() * 10000.0) + 0.5);
            if (pane.first() != nullptr)
                wire.children.push_back(serializePaneTree(*pane.first()));
            if (pane.second() != nullptr)
                wire.children.push_back(serializePaneTree(*pane.second()));
        }
        return wire;
    }

    /// Serializes the host window's whole tab/pane layout for the LayoutState PDU.
    [[nodiscard]] proto::LayoutState serializeLayout(SessionHost& host)
    {
        auto layout = proto::LayoutState {};
        auto* window = host.model().window(host.windowId());
        if (window == nullptr)
            return layout;
        layout.window = host.windowId().value;
        layout.activeTab = static_cast<uint32_t>(std::max(0, window->activeTabIndex()));
        for (auto const tabIndex: std::views::iota(0, window->tabCount()))
        {
            auto* tab = window->tabAt(tabIndex);
            if (tab == nullptr)
                continue;
            auto wireTab = proto::WireTab {};
            wireTab.tabId = tab->id().value;
            if (auto const* active = tab->activePane())
                wireTab.activePane = active->id().value;
            if (auto const zoomed = tab->zoomedLeafId())
                wireTab.zoomedPane = zoomed->value;
            if (auto const& title = tab->runtimeTitle())
                wireTab.title = *title;
            if (auto const color = tab->color())
            {
                wireTab.hasColor = 1;
                wireTab.color = color->value();
            }
            wireTab.root = serializePaneTree(*tab->rootPane());
            layout.tabs.push_back(std::move(wireTab));
        }
        return layout;
    }

    /// Converts one grid row into its wire form (caller holds the terminal lock).
    [[nodiscard]] proto::WireLine toWireLine(vtbackend::Grid const& grid,
                                             vtbackend::LineOffset offset,
                                             vtbackend::Line const& line)
    {
        auto wire = proto::WireLine {};
        wire.stableId = grid.stableLineIdOf(offset);
        wire.flags = static_cast<uint16_t>(line.flags().value());
        wire.columns = unbox<uint32_t>(line.size());

        auto const& soa = line.storage();
        wire.fillForeground = rawColor(soa.fillAttrs.foregroundColor);
        wire.fillBackground = rawColor(soa.fillAttrs.backgroundColor);
        if (line.isBlank())
            return wire; // uniformly filled: no cells on the wire

        auto const columns = unbox<std::size_t>(line.size());
        wire.cells.reserve(columns);
        for (std::size_t column = 0; column < columns; ++column)
        {
            auto cell = proto::WireCell {};
            cell.codepoint = soa.codepoints[column];
            if (soa.clusterSize[column] >= 2)
            {
                auto const extras = static_cast<std::size_t>(soa.clusterSize[column] - 1);
                auto const start = soa.clusterPoolIndex[column];
                for (std::size_t i = 0; i < extras && start + i < soa.clusterPool.size(); ++i)
                    cell.clusterExtras.push_back(soa.clusterPool[start + i]);
            }
            cell.width = soa.widths[column];
            cell.scale = soa.scales[column];
            cell.textScaleExtras = soa.textScaleExtras[column];
            cell.hyperlink = unbox<uint16_t>(soa.hyperlinks[column]);
            cell.foreground = rawColor(soa.sgr[column].foregroundColor);
            cell.background = rawColor(soa.sgr[column].backgroundColor);
            cell.underlineColor = rawColor(soa.sgr[column].underlineColor);
            cell.flags = static_cast<uint32_t>(soa.sgr[column].flags.value());
            wire.cells.push_back(std::move(cell));
        }
        return wire;
    }

    /// Collects the image-cell side-table entries of one row.
    void appendImageCells(std::vector<proto::ImageCellEntry>& out,
                          int64_t stableId,
                          vtbackend::Line const& line)
    {
        auto const& fragments = line.storage().imageFragments;
        if (!fragments)
            return;
        for (auto const& [column, fragment]: *fragments)
        {
            if (!fragment)
                continue;
            auto const& rasterized = fragment->rasterizedImage();
            out.push_back(proto::ImageCellEntry {
                .stableId = stableId,
                .column = column,
                .imageId = unbox<uint32_t>(rasterized.image().id()),
                .offsetLine = static_cast<uint16_t>(unbox<int>(fragment->offset().line)),
                .offsetColumn = static_cast<uint16_t>(unbox<int>(fragment->offset().column)),
                .layer = std::to_underlying(rasterized.layer()),
            });
        }
    }
} // namespace

NativeSession::NativeSession(net::EventLoop& loop,
                             SessionHost& host,
                             std::unique_ptr<net::ISocket> connection,
                             std::size_t maxWriteQueueBytes,
                             std::string expectedToken):
    _loop(loop),
    _host(host),
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), maxWriteQueueBytes),
    _expectedToken(std::move(expectedToken))
{
    // Every model change (the host fans them here once subscribed) re-pushes the
    // whole layout — infrequent, so a full resend beats a granular diff.
    _layoutObserver.onChange = [this] {
        pushLayout();
    };
}

void NativeSession::send(uint64_t serial, proto::DecodedPdu const& pdu)
{
    if (_closed)
        return;
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    if (!_writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }))
    {
        // The queue's overflow contract: a client too slow to drain the byte bound
        // is disconnected, not silently under-served — the delta cursor has already
        // advanced past this frame, so dropping it would leave the mirror holey
        // with no resync trigger. Closing the connection unparks the PDU pump.
        _closed = true;
        _writer.close();
        _connection->close();
    }
}

void NativeSession::sessionScreenUpdated(SessionId session)
{
    if (!_handshaken || _closed)
        return;
    _pendingSessions.insert(session.value);
    if (_flushScheduled)
        return;
    _flushScheduled = true;
    _loop.spawn(flushSoon());
}

void NativeSession::sessionClosed(SessionId session)
{
    _followed.erase(session.value);
    _pendingSessions.erase(session.value);
}

void NativeSession::sessionBell(SessionId session)
{
    emitSessionEvent(session, proto::SessionEventKind::Bell, {}, {});
}

void NativeSession::sessionNotify(SessionId session, std::string const& title, std::string const& body)
{
    emitSessionEvent(session, proto::SessionEventKind::Notify, title, body);
}

void NativeSession::sessionCopyToClipboard(SessionId session, std::string const& data)
{
    // "c" = the CLIPBOARD selection; the client applies its own write permission.
    emitSessionEvent(session, proto::SessionEventKind::ClipboardSet, "c", data);
}

void NativeSession::emitSessionEvent(SessionId session,
                                     proto::SessionEventKind kind,
                                     std::string a,
                                     std::string b)
{
    if (!_handshaken || _closed)
        return;
    send(0,
         proto::DecodedPdu { proto::SessionEvent { .session = session.value,
                                                   .kind = std::to_underlying(kind),
                                                   .a = std::move(a),
                                                   .b = std::move(b) } });
}

void NativeSession::pushLayout()
{
    if (!_handshaken || _closed)
        return;
    send(0, proto::DecodedPdu { serializeLayout(_host) });
}

coro::Task<void> NativeSession::flushSoon()
{
    // Debounce: a busy PTY produces many screenUpdated signals per frame-worth
    // of output; one Delta per ~20ms window is what the client can use anyway.
    co_await _loop.delay(20ms);
    _flushScheduled = false;
    if (_closed)
        co_return;
    auto pending = std::exchange(_pendingSessions, {});
    for (auto const session: pending)
        pushDelta(SessionId { session }, /*forceSnapshot=*/false);
}

void NativeSession::pushDelta(SessionId session, bool forceSnapshot)
{
    auto* terminal = _host.terminal(session);
    if (terminal == nullptr)
        return;
    auto& follow = _followed[session.value];

    auto delta = proto::Delta {};
    delta.session = session.value;
    auto state = std::optional<proto::SessionState> {};

    auto hyperlinkIds = std::vector<uint16_t> {};
    auto referencedLinks = std::unordered_set<uint16_t> {}; ///< Deduplicates ids within THIS delta.
    {
        // The same lock discipline as refreshRenderBuffer: all grid queries
        // happen under the terminal's state lock — and so does every terminal
        // read below (pageSize/screenType/windowTitle mutate on the session's
        // pump thread; windowTitle() in particular is an unlocked reference).
        auto const guard = std::lock_guard { *terminal };

        // Mirror exactly what the fat GUI paints: the DISPLAYED page, which is what
        // the user sees. It coincides with the cursor page while DECPCCM couples
        // them, but a decoupled display — or any DEC page 1..14 reached via NPP/PP/
        // PPA — shows a different page than VT output currently targets. Serializing
        // currentScreen() (the cursor page, and during an active status display the
        // status screen) would mirror the wrong grid.
        auto const displayedPage = terminal->displayedPageIndex();
        auto& grid = terminal->pageAt(displayedPage).grid();

        // Each of the 16 pages is a distinct grid whose generation advances
        // independently (and can collide across pages), so one cursor cannot span a
        // page flip: treat it as a wholesale identity change and resync. Keying on
        // the page INDEX (not screenType, which collapses DEC pages 1..14 to
        // "Alternate") is what makes page<->page switches mirror correctly. This
        // also gives a session's very first push its SessionState (nullopt != page).
        // screenTypeFromPage then maps the displayed page to the wire's primary(0)/
        // alt-like(1) discriminator the mirror uses to toggle ?1049 and scrollback.
        auto const screenType = vtbackend::screenTypeFromPage(displayedPage);
        auto snapshot = forceSnapshot || follow.lastDisplayedPage != displayedPage;
        follow.lastDisplayedPage = displayedPage;

        auto const collect = [&](vtbackend::LineOffset offset, vtbackend::Line const& line) {
            delta.lines.push_back(toWireLine(grid, offset, line));
            appendImageCells(delta.imageCells, delta.lines.back().stableId, line);
            // Collect every referenced id (deduped within this delta), NOT only
            // never-sent ones: the send loop below decides per id whether its URI
            // actually needs (re)sending, which is what catches an id reused for a
            // different URI after the 16-bit counter wrapped.
            for (auto const& cell: delta.lines.back().cells)
                if (cell.hyperlink != 0 && referencedLinks.insert(cell.hyperlink).second)
                    hyperlinkIds.push_back(cell.hyperlink);
        };

        if (!snapshot
            && grid.forEachLineChangedSince(follow.cursor, collect)
                   == vtbackend::GridDeltaResult::ResyncRequired)
            snapshot = true;
        if (snapshot)
        {
            delta.lines.clear();
            delta.imageCells.clear();
            hyperlinkIds.clear();
            referencedLinks.clear();
            grid.forEachValidLine(collect);
            // The snapshot delivered the whole grid, so re-anchor the cursor to the
            // stream head directly -- forEachValidLine leaves it untouched, and a
            // second forEachLineChangedSince purely to advance it would rescan.
            grid.anchorCursorToHead(follow.cursor);
        }
        delta.snapshot = snapshot ? 1 : 0;
        delta.generation = grid.generation();
        delta.seqno = grid.seqno();
        delta.stableViewportBase = grid.stableLineIdOf(vtbackend::LineOffset(0));
        // The scrollback floor: a `clear`/CSI 3 J evicts history without a
        // generation bump or any line change, so this is the ONLY signal that
        // tells the mirror to drop the history the real terminal discarded.
        delta.stableFloor = grid.stableRangeFloor();

        auto const cursor = terminal->pageAt(displayedPage).cursor().position;
        delta.cursorLine = unbox<int32_t>(cursor.line);
        delta.cursorColumn = unbox<int32_t>(cursor.column);

        // When DECPCCM is reset and the cursor sits on a page other than the one
        // displayed, the fat GUI hides the cursor (it belongs to a page the user is
        // not looking at). Mirror that by withholding VisibleCursor (DECTCEM/mode
        // 25) from the mode set, exactly as if the app had hidden it.
        auto const cursorOnDisplayedPage = terminal->cursorPageIndex() == displayedPage;
        for (auto const mode: MirroredModes)
        {
            if (mode == vtbackend::DECMode::VisibleCursor && !cursorOnDisplayedPage)
                continue;
            if (terminal->isModeEnabled(mode))
                delta.setModes.push_back(vtbackend::toDECModeNum(mode));
        }

        for (auto const id: hyperlinkIds)
        {
            auto const info = terminal->hyperlinks().hyperlinkById(vtbackend::HyperlinkId { id });
            if (!info)
                continue;
            // Send only when this id is new, or its URI changed since we last sent
            // it. The terminal's HyperlinkId is a uint16_t that wraps and reuses
            // ids, so an id keyed once and never revisited would pin the mirror to
            // a stale URI after wraparound.
            auto const [it, inserted] = follow.sentHyperlinks.try_emplace(id, info->uri);
            if (!inserted && it->second == info->uri)
                continue; // already sent this exact id->URI mapping
            it->second = info->uri;
            delta.hyperlinks.push_back(proto::HyperlinkEntry { .id = id, .uri = info->uri });
        }

        // Live window title (OSC 0/2): the snapshot carries it in SessionState
        // below; an incremental delta carries it only when it changed since last
        // sent, so a title-only batch still re-titles the mirror.
        if (auto title = std::string { terminal->windowTitle() }; title != follow.lastTitle)
        {
            if (!snapshot)
            {
                delta.titleChanged = 1;
                delta.title = title;
            }
            follow.lastTitle = std::move(title);
        }

        // Live cursor shape (DECSCUSR): pull+diff like the title; the snapshot
        // carries it in SessionState below.
        auto const cursorPs = decscusrPs(terminal->cursorShape(), terminal->cursorDisplay());
        if (cursorPs != follow.lastCursorShape)
        {
            if (!snapshot)
            {
                delta.cursorShapeChanged = 1;
                delta.cursorShape = cursorPs;
            }
            follow.lastCursorShape = cursorPs;
        }

        // Live working directory (OSC 7): pull+diff; the snapshot carries it in
        // SessionState below. Only propagate once one was actually set.
        auto const& cwd = terminal->currentWorkingDirectory();
        if (!cwd.empty() && (!follow.cwdKnown || cwd != follow.lastCwd))
        {
            if (!snapshot)
            {
                delta.cwdChanged = 1;
                delta.cwd = cwd;
            }
            follow.lastCwd = cwd;
            follow.cwdKnown = true;
        }

        // Live default fg/bg (OSC 10/11): pull+diff; the snapshot carries them in
        // SessionState below.
        auto const& colors = terminal->colorPalette();
        auto const fg = static_cast<int>(colors.defaultForeground.value());
        auto const bg = static_cast<int>(colors.defaultBackground.value());
        if (fg != follow.lastDefaultForeground || bg != follow.lastDefaultBackground)
        {
            if (!snapshot)
            {
                delta.colorsChanged = 1;
                delta.defaultForeground = static_cast<uint32_t>(fg);
                delta.defaultBackground = static_cast<uint32_t>(bg);
            }
            follow.lastDefaultForeground = fg;
            follow.lastDefaultBackground = bg;
        }

        // Live status-display state (DECSSDT/DECSASD) — the first slice of
        // multi-page support: which status line is shown and where the app writes.
        auto const statusType = static_cast<int>(std::to_underlying(terminal->statusDisplayType()));
        auto const activeStatus = static_cast<int>(std::to_underlying(terminal->activeStatusDisplay()));
        if (statusType != follow.lastStatusDisplayType || activeStatus != follow.lastActiveStatusDisplay)
        {
            if (!snapshot)
            {
                delta.statusChanged = 1;
                delta.statusDisplayType = static_cast<uint8_t>(statusType);
                delta.activeStatusDisplay = static_cast<uint8_t>(activeStatus);
            }
            follow.lastStatusDisplayType = statusType;
            follow.lastActiveStatusDisplay = activeStatus;
        }

        // Host-writable status-line CONTENT (a separate page): when that page is
        // shown, carry its whole (tiny) grid so the client paints the app's custom
        // status line. Full resend on change — no stable-id delta for one row.
        if (terminal->statusDisplayType() == vtbackend::StatusDisplayType::HostWritable)
        {
            auto const& statusGrid = terminal->hostWritableStatusLineDisplay().grid();
            auto rows = std::vector<proto::WireLine> {};
            for (auto const i: std::views::iota(0, unbox<int>(statusGrid.pageSize().lines)))
                rows.push_back(toWireLine(
                    statusGrid, vtbackend::LineOffset(i), statusGrid.lineAt(vtbackend::LineOffset(i))));
            if (snapshot || rows != follow.lastStatusLines)
            {
                delta.statusLinesChanged = 1;
                delta.statusLines = rows;
            }
            follow.lastStatusLines = std::move(rows);
        }

        // Live Kitty keyboard protocol flags (pull+diff): the top of the app's
        // CSI-u flag stack governs how KEYS are encoded, so the client must track
        // it to send input the way the app negotiated. Only the effective (current)
        // flags matter to a mirror — the stack itself stays server-side.
        auto const kittyFlags = static_cast<int>(terminal->keyboardProtocol().flags().value());
        if (kittyFlags != follow.lastKittyKeyboardFlags)
        {
            if (!snapshot)
            {
                delta.kittyKeyboardChanged = 1;
                delta.kittyKeyboardFlags = static_cast<uint8_t>(kittyFlags);
            }
            follow.lastKittyKeyboardFlags = kittyFlags;
        }

        if (snapshot)
        {
            auto& snap = state.emplace();
            snap.session = session.value;
            auto const size = terminal->pageSize();
            snap.columns = unbox<uint32_t>(size.columns);
            snap.lines = unbox<uint32_t>(size.lines);
            snap.screenType = std::to_underlying(screenType);
            snap.cursorLine = delta.cursorLine;
            snap.cursorColumn = delta.cursorColumn;
            snap.title = terminal->windowTitle();
            snap.cursorShape = cursorPs;
            snap.cwd = terminal->currentWorkingDirectory();
            snap.defaultForeground = colors.defaultForeground.value();
            snap.defaultBackground = colors.defaultBackground.value();
            snap.statusDisplayType = static_cast<uint8_t>(statusType);
            snap.activeStatusDisplay = static_cast<uint8_t>(activeStatus);
            snap.kittyKeyboardFlags = static_cast<uint8_t>(kittyFlags);
        }
    }

    if (state)
        send(0, proto::DecodedPdu { *state });
    // A pure mode flip (an app enabling mouse tracking, say) changes no cell,
    // yet clients must hear about it to encode input correctly. A pure cursor move
    // (a full-screen app repositioning with no visible cell change) likewise
    // carries only the new cursor position, but the mirror must still get it or its
    // cursor lags until the next cell write.
    auto const modesChanged = delta.setModes != follow.lastModes;
    auto const cursorMoved =
        delta.cursorLine != follow.lastCursorLine || delta.cursorColumn != follow.lastCursorColumn;
    if (delta.snapshot != 0 || !delta.lines.empty() || modesChanged || cursorMoved || delta.titleChanged != 0
        || delta.cursorShapeChanged != 0 || delta.cwdChanged != 0 || delta.colorsChanged != 0
        || delta.statusChanged != 0 || delta.statusLinesChanged != 0 || delta.kittyKeyboardChanged != 0)
    {
        follow.lastModes = delta.setModes;
        follow.lastCursorLine = delta.cursorLine;
        follow.lastCursorColumn = delta.cursorColumn;
        send(0, proto::DecodedPdu { delta });
    }
}

void NativeSession::handlePdu(proto::DecodedFrame const& frame)
{
    if (auto const* input = std::get_if<proto::Input>(&frame.pdu))
    {
        if (auto* terminal = _host.terminal(SessionId { input->session }))
            std::ignore = terminal->device().write(
                std::string_view { reinterpret_cast<char const*>(input->data.data()), input->data.size() });
        return;
    }
    if (auto const* resize = std::get_if<proto::ResizeRequest>(&frame.pdu))
    {
        if (resize->columns >= 1 && resize->columns <= 10000 && resize->lines >= 1 && resize->lines <= 10000)
        {
            _host.applyClientSize(
                vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(resize->lines)),
                                  .columns = vtpty::ColumnCount(static_cast<int>(resize->columns)) });
            for (auto& [session, follow]: _followed)
                pushDelta(SessionId { session }, /*forceSnapshot=*/true);
        }
        return;
    }
    if (auto const* fetch = std::get_if<proto::FetchImage>(&frame.pdu))
    {
        // Image ids are per-session ImagePool counters (each starts at 1), so the
        // SAME numeric id names different images in different sessions. The lookup
        // must be scoped to the session the requested cell belongs to — scanning
        // every followed session and returning the first match paints the wrong
        // picture whenever two sessions minted the same id.
        auto* terminal = _host.terminal(SessionId { fetch->session });
        if (terminal != nullptr)
            if (auto const image = terminal->imagePool().findImageById(vtbackend::ImageId { fetch->imageId }))
            {
                auto data = proto::ImageData {};
                data.imageId = fetch->imageId;
                data.format = std::to_underlying(image->format());
                data.width = unbox<uint32_t>(image->width());
                data.height = unbox<uint32_t>(image->height());
                data.data.assign(
                    reinterpret_cast<std::byte const*>(image->data().data()),
                    reinterpret_cast<std::byte const*>(image->data().data() + image->data().size()));
                send(frame.serial, proto::DecodedPdu { data });
                return;
            }
        send(frame.serial, proto::DecodedPdu { proto::ImageGone { .imageId = fetch->imageId } });
        return;
    }
    // Layout-authoring verbs (F2): route to the model. The resulting ModelEvents
    // fan out through every client's LayoutObserver, so the change mirrors to all
    // attached clients (including this one) as a fresh LayoutState.
    if (std::holds_alternative<proto::CreateTab>(frame.pdu))
    {
        std::ignore = _host.createTab();
        return;
    }
    if (auto const* split = std::get_if<proto::SplitPane>(&frame.pdu))
    {
        _host.splitActivePane(vtmux::TabId { split->tab },
                              static_cast<vtmux::SplitState>(split->orientation),
                              static_cast<double>(split->ratio) / 10000.0);
        return;
    }
    if (auto const* close = std::get_if<proto::ClosePane>(&frame.pdu))
    {
        _host.handleSessionExit(vtmux::SessionId { close->session });
        return;
    }
    // Unknown/unexpected PDUs are ignored: forward compatibility.
}

bool NativeSession::completeHandshake(proto::DecodedFrame const& frame)
{
    auto const* hello = std::get_if<proto::ClientHello>(&frame.pdu);
    if (hello == nullptr || hello->codecVersion != proto::CodecVersion)
    {
        // Answer with our version so the peer can report the mismatch, then close.
        send(frame.serial, proto::DecodedPdu { proto::ServerHello {} });
        return false;
    }
    // Preshared-token auth (empty _expectedToken accepts any — the AF_UNIX default,
    // where the socket's permissions are the gate). A mismatch answers the version
    // handshake and drops, exactly as a version mismatch does, revealing nothing.
    if (!_expectedToken.empty() && hello->token != _expectedToken)
    {
        send(frame.serial, proto::DecodedPdu { proto::ServerHello {} });
        return false;
    }
    send(frame.serial, proto::DecodedPdu { proto::ServerHello {} });
    _handshaken = true;

    // Attaching to an empty daemon spawns the first session.
    if (_host.model().window(_host.windowId())->tabCount() == 0)
        std::ignore = _host.createTab();

    // The window/tab/pane layout first, so the client builds its tabs and split
    // trees before the per-session content streams into them.
    pushLayout();

    // The attach snapshot: every hosted session, full state.
    auto* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
        window->tabAt(tabIndex)->rootPane()->walkTree([&](vtmux::Pane& pane) {
            if (pane.isLeaf())
                pushDelta(pane.session(), /*forceSnapshot=*/true);
        });
    return true;
}

coro::Task<void> NativeSession::run()
{
    // Nothing is valid before a version-matching ClientHello; afterwards the
    // pump serves request PDUs until the peer disconnects.
    co_await pumpPdus(_connection.get(), [this](proto::DecodedFrame const& frame) {
        if (!_handshaken)
            return completeHandshake(frame);
        handlePdu(frame);
        return true;
    });

    _closed = true;
    // serveNativeClient destroys this session the moment run() returns, but a
    // debounce flush spawned before the disconnect may still be parked in its
    // 20ms delay with `this` in its frame. Wait for it to resume (it observes
    // _closed and backs out) before letting the frame die.
    co_await net::pollUntil(&_loop, [this] { return !_flushScheduled; });
    co_await _writer.flushThenClose();
    _connection->close();
}

namespace
{
    /// One native client's whole lifetime, as a free coroutine (a capturing
    /// lambda coroutine would dangle its closure; pointers live in the frame).
    coro::Task<void> serveNativeClient(net::EventLoop* loop,
                                       SessionHost* host,
                                       std::unique_ptr<net::ISocket> connection,
                                       std::string expectedToken)
    {
        auto session = std::make_unique<NativeSession>(*loop,
                                                       *host,
                                                       std::move(connection),
                                                       NativeSession::DefaultWriteQueueBytes,
                                                       std::move(expectedToken));
        auto const subscription = ScopedStreamSubscription { *host, *session };
        auto const layoutSubscription = ScopedModelSubscription { *host, session->layoutObserver() };
        co_await session->run();
    }
} // namespace

std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeNativeHandler(net::EventLoop& loop,
                                                                                 SessionHost& host,
                                                                                 std::string expectedToken)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return
        [&loop, &host, expectedToken = std::move(expectedToken)](std::unique_ptr<net::ISocket> connection) {
            return serveNativeClient(&loop, &host, std::move(connection), expectedToken);
        };
}

} // namespace muxserver
