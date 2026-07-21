// SPDX-License-Identifier: Apache-2.0
#include <muxserver/NativeSession.h>

#include <vtbackend/Image.h>
#include <vtbackend/Line.h>

#include <bit>
#include <chrono>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

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
                             std::unique_ptr<net::ISocket> connection):
    _loop(loop),
    _host(host),
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 4 } * 1024 * 1024)
{
}

void NativeSession::send(uint64_t serial, proto::DecodedPdu const& pdu)
{
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    std::ignore = _writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() });
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

    auto hyperlinkIds = std::vector<uint16_t> {};
    {
        // The same lock discipline as refreshRenderBuffer: all grid queries
        // happen under the terminal's state lock.
        auto const guard = std::lock_guard { *terminal };
        auto& grid = terminal->currentScreen().grid();

        auto const collect = [&](vtbackend::LineOffset offset, vtbackend::Line const& line) {
            delta.lines.push_back(toWireLine(grid, offset, line));
            appendImageCells(delta.imageCells, delta.lines.back().stableId, line);
            for (auto const& cell: delta.lines.back().cells)
                if (cell.hyperlink != 0 && !follow.sentHyperlinks.contains(cell.hyperlink))
                    hyperlinkIds.push_back(cell.hyperlink);
        };

        auto snapshot = forceSnapshot;
        if (!snapshot
            && grid.forEachLineChangedSince(follow.cursor, collect)
                   == vtbackend::GridDeltaResult::ResyncRequired)
            snapshot = true;
        if (snapshot)
        {
            delta.lines.clear();
            delta.imageCells.clear();
            hyperlinkIds.clear();
            grid.forEachValidLine(collect);
            // Re-anchor the cursor past the snapshot (forEachValidLine does not).
            std::ignore = grid.forEachLineChangedSince(follow.cursor,
                                                       [](vtbackend::LineOffset, vtbackend::Line const&) {});
        }
        delta.snapshot = snapshot ? 1 : 0;
        delta.generation = grid.generation();
        delta.seqno = grid.seqno();
        delta.stableViewportBase = grid.stableLineIdOf(vtbackend::LineOffset(0));

        auto const cursor = terminal->currentScreen().cursor().position;
        delta.cursorLine = unbox<int32_t>(cursor.line);
        delta.cursorColumn = unbox<int32_t>(cursor.column);

        for (auto const id: hyperlinkIds)
        {
            auto const info = terminal->hyperlinks().hyperlinkById(vtbackend::HyperlinkId { id });
            if (!info)
                continue;
            delta.hyperlinks.push_back(proto::HyperlinkEntry { .id = id, .uri = info->uri });
            follow.sentHyperlinks.insert(id);
        }
    }

    if (delta.snapshot != 0)
    {
        auto state = proto::SessionState {};
        state.session = session.value;
        auto const size = terminal->pageSize();
        state.columns = unbox<uint32_t>(size.columns);
        state.lines = unbox<uint32_t>(size.lines);
        state.screenType = std::to_underlying(terminal->screenType());
        state.cursorLine = delta.cursorLine;
        state.cursorColumn = delta.cursorColumn;
        state.title = terminal->windowTitle();
        send(0, proto::DecodedPdu { state });
    }
    if (delta.snapshot != 0 || !delta.lines.empty())
        send(0, proto::DecodedPdu { delta });
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
        for (auto& [session, follow]: _followed)
        {
            auto* terminal = _host.terminal(SessionId { session });
            if (terminal == nullptr)
                continue;
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
    while (_writer.queuedBytes() > 0 || _writer.draining())
        co_await _loop.delay(1ms);
    _writer.close();
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
