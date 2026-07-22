// SPDX-License-Identifier: Apache-2.0
#include <muxserver/NativeSession.h>

#include <vtbackend/Image.h>
#include <vtbackend/Line.h>

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
                             std::size_t maxWriteQueueBytes):
    _loop(loop),
    _host(host),
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), maxWriteQueueBytes)
{
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
        auto& grid = terminal->currentScreen().grid();

        // The primary and alternate screens are distinct grids whose generations
        // advance independently (and can collide), so one cursor cannot span a
        // flip: treat it as a wholesale identity change and resync. This also
        // gives a session's very first push its SessionState (nullopt != type).
        auto const screenType = terminal->screenType();
        auto snapshot = forceSnapshot || follow.lastScreenType != screenType;
        follow.lastScreenType = screenType;

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

        auto const cursor = terminal->currentScreen().cursor().position;
        delta.cursorLine = unbox<int32_t>(cursor.line);
        delta.cursorColumn = unbox<int32_t>(cursor.column);

        for (auto const mode: MirroredModes)
            if (terminal->isModeEnabled(mode))
                delta.setModes.push_back(vtbackend::toDECModeNum(mode));

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
    if (delta.snapshot != 0 || !delta.lines.empty() || modesChanged || cursorMoved || delta.titleChanged != 0)
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
    send(frame.serial, proto::DecodedPdu { proto::ServerHello {} });
    _handshaken = true;

    // Attaching to an empty daemon spawns the first session.
    if (_host.model().window(_host.windowId())->tabCount() == 0)
        std::ignore = _host.createTab();

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
                                       std::unique_ptr<net::ISocket> connection)
    {
        auto session = std::make_unique<NativeSession>(*loop, *host, std::move(connection));
        auto const subscription = ScopedStreamSubscription { *host, *session };
        co_await session->run();
    }
} // namespace

std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeNativeHandler(net::EventLoop& loop,
                                                                                 SessionHost& host)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host](std::unique_ptr<net::ISocket> connection) {
        return serveNativeClient(&loop, &host, std::move(connection));
    };
}

} // namespace muxserver
