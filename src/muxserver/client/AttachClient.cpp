// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/AttachClient.h>

#include <libunicode/convert.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>
#include <variant>
#include <vector>

#include <muxserver/PduPump.h>
#include <net/Sockets.h>

namespace muxserver::client
{

using namespace std::chrono_literals;

void appendCluster(std::string& out, proto::WireCell const& cell)
{
    unicode::convert_to<char>(std::u32string_view(&cell.codepoint, 1), std::back_inserter(out));
    for (auto const extra: cell.clusterExtras)
        unicode::convert_to<char>(std::u32string_view(&extra, 1), std::back_inserter(out));
}

// ---------------------------------------------------------------------------
// RemoteScreen

void RemoteScreen::apply(proto::SessionState const& state)
{
    session = state.session;
    columns = state.columns;
    lines = state.lines;
    screenType = state.screenType;
    cursorLine = state.cursorLine;
    cursorColumn = state.cursorColumn;
    title = state.title;
    cursorShape = state.cursorShape;
    cwd = state.cwd;
}

void RemoteScreen::apply(proto::Delta const& delta)
{
    session = delta.session;
    if (delta.snapshot != 0)
    {
        rows.clear(); // a snapshot replaces everything the client held
        imageCells.clear();
        // The pixel caches stay valid: image ids are pool-scoped, not
        // generation-scoped, so a rebuild does not invalidate a fetched image.
    }

    generation = delta.generation;
    seqno = delta.seqno;
    viewportBase = delta.stableViewportBase;
    stableFloor = delta.stableFloor;
    cursorLine = delta.cursorLine;
    cursorColumn = delta.cursorColumn;
    setModes = delta.setModes;
    if (delta.titleChanged != 0)
        title = delta.title; // keep it current so a later fullReplay re-titles correctly
    if (delta.cursorShapeChanged != 0)
        cursorShape = delta.cursorShape;
    if (delta.cwdChanged != 0)
        cwd = delta.cwd;

    for (auto const& line: delta.lines)
    {
        rows.insert_or_assign(line.stableId, line);
        // The row was redrawn: its previous image cells are stale. The server
        // re-sends any that survived in delta.imageCells (which only references
        // rows present in delta.lines), so clearing first is loss-free.
        imageCells.erase(line.stableId);
    }
    for (auto const& entry: delta.imageCells)
        imageCells[entry.stableId][entry.column] = entry;
    for (auto const& entry: delta.hyperlinks)
        hyperlinks.insert_or_assign(entry.id, entry.uri);

    // Trim client-side scrollback. The server's floor is authoritative — a
    // `clear`/CSI 3 J jumps it up with no line changes, so honoring it is what
    // drops history the real terminal already discarded (otherwise the mirror
    // keeps showing ghost scrollback). The HistoryKeep cap bounds memory when
    // the floor sits far below the viewport.
    auto const evictBelow = std::max(stableFloor, viewportBase - HistoryKeep);
    rows.erase(rows.begin(), rows.lower_bound(evictBelow));
    imageCells.erase(imageCells.begin(), imageCells.lower_bound(evictBelow));
}

proto::ImageCellEntry const* RemoteScreen::imageAt(int64_t stableId, uint16_t column) const
{
    auto const rowIt = imageCells.find(stableId);
    if (rowIt == imageCells.end())
        return nullptr;
    auto const colIt = rowIt->second.find(column);
    return colIt != rowIt->second.end() ? &colIt->second : nullptr;
}

proto::ImageData const* RemoteScreen::imageData(uint32_t imageId) const
{
    auto const it = images.find(imageId);
    return it != images.end() ? &it->second : nullptr;
}

void RemoteScreen::dropImage(uint32_t imageId)
{
    images.erase(imageId);
    requestedImages.erase(imageId);
    for (auto rowIt = imageCells.begin(); rowIt != imageCells.end();)
    {
        auto& columns = rowIt->second;
        for (auto colIt = columns.begin(); colIt != columns.end();)
        {
            if (colIt->second.imageId == imageId)
                colIt = columns.erase(colIt);
            else
                ++colIt;
        }
        if (columns.empty())
            rowIt = imageCells.erase(rowIt);
        else
            ++rowIt;
    }
}

proto::WireLine const* RemoteScreen::rowAt(int32_t line) const
{
    auto const it = rows.find(viewportBase + line);
    return it != rows.end() ? &it->second : nullptr;
}

std::string RemoteScreen::viewportText() const
{
    auto text = std::string {};
    for (auto line = int32_t { 0 }; std::cmp_less(line, lines); ++line)
    {
        auto rendered = std::string {};
        if (auto const* row = rowAt(line))
            for (auto const& cell: row->cells)
            {
                if (cell.codepoint == 0)
                {
                    rendered += ' ';
                    continue;
                }
                appendCluster(rendered, cell);
            }
        while (!rendered.empty() && rendered.back() == ' ')
            rendered.pop_back();
        text += rendered;
        text += '\n';
    }
    return text;
}

// ---------------------------------------------------------------------------
// AttachClient

AttachClient::AttachClient(net::EventLoop& loop, std::unique_ptr<net::ISocket> connection, std::string token):
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 1 } * 1024 * 1024),
    _token(std::move(token))
{
}

uint64_t AttachClient::send(proto::DecodedPdu const& pdu)
{
    auto const serial = _nextSerial++;
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    if (!_writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }))
    {
        // The queue's overflow contract: dropping a frame mid-stream (a
        // keystroke, a resize) silently desyncs the daemon — sever instead.
        _writer.close();
        _connection->close();
    }
    return serial;
}

void AttachClient::sendInput(uint64_t session, std::string_view bytes)
{
    auto input = proto::Input { .session = session, .data = {} };
    input.data.reserve(bytes.size());
    for (auto const ch: bytes)
        input.data.push_back(static_cast<std::byte>(ch));
    send(proto::DecodedPdu { input });
}

void AttachClient::requestResize(uint32_t columns, uint32_t lines)
{
    send(proto::DecodedPdu { proto::ResizeRequest { .columns = columns, .lines = lines } });
}

void AttachClient::fetchImage(uint64_t session, uint32_t imageId)
{
    auto const serial =
        send(proto::DecodedPdu { proto::FetchImage { .session = session, .imageId = imageId } });
    // The ImageData/ImageGone answer carries no session — remember which one this
    // serial belongs to so the reply lands in the right screen's cache.
    _pendingImages.insert_or_assign(serial, std::pair { session, imageId });
}

void AttachClient::detach()
{
    _detached = true;
    _writer.close();
    _connection->close();
}

void AttachClient::handlePdu(proto::DecodedFrame const& frame)
{
    auto const& pdu = frame.pdu;
    if (auto const* hello = std::get_if<proto::ServerHello>(&pdu))
    {
        if (hello->codecVersion == proto::CodecVersion)
            _connected = true;
        else
            _versionMismatch = true;
        return;
    }
    if (auto const* state = std::get_if<proto::SessionState>(&pdu))
    {
        _screens[state->session].apply(*state);
        return;
    }
    if (auto const* delta = std::get_if<proto::Delta>(&pdu))
    {
        auto& screen = _screens[delta->session];
        screen.apply(*delta);
        // Pull pixels for any image this delta newly referenced that we neither
        // hold nor have a request in flight for. The cells render blank until the
        // ImageData answer lands and fires the image handler.
        for (auto const& entry: delta->imageCells)
        {
            if (screen.images.contains(entry.imageId) || screen.requestedImages.contains(entry.imageId))
                continue;
            screen.requestedImages.insert(entry.imageId);
            fetchImage(delta->session, entry.imageId);
        }
        if (_onUpdate)
            _onUpdate(screen, *delta);
        return;
    }
    if (auto const* image = std::get_if<proto::ImageData>(&pdu))
    {
        // The reply carries no session; the request serial is what routes it.
        auto const it = _pendingImages.find(frame.serial);
        if (it == _pendingImages.end())
            return;
        auto const [session, imageId] = it->second;
        _pendingImages.erase(it);
        auto& screen = _screens[session];
        screen.images.insert_or_assign(imageId, *image);
        if (_onImage)
            _onImage(screen, imageId);
        return;
    }
    if (std::holds_alternative<proto::ImageGone>(pdu))
    {
        auto const it = _pendingImages.find(frame.serial);
        if (it == _pendingImages.end())
            return;
        auto const [session, imageId] = it->second;
        _pendingImages.erase(it);
        auto& screen = _screens[session];
        screen.dropImage(imageId);
        if (_onImage)
            _onImage(screen, imageId);
        return;
    }
    if (auto const* event = std::get_if<proto::SessionEvent>(&pdu))
    {
        if (_onSessionEvent)
            _onSessionEvent(_screens[event->session], *event);
        return;
    }
    // Unknown PDUs are ignored for forward compatibility.
}

coro::Task<void> AttachClient::run()
{
    send(proto::DecodedPdu { proto::ClientHello { .codecVersion = proto::CodecVersion, .token = _token } });

    co_await pumpPdus(_connection.get(), [this](proto::DecodedFrame const& frame) {
        handlePdu(frame);
        return !_detached && !_versionMismatch;
    });

    if (!_detached)
    {
        co_await _writer.flushThenClose();
        _connection->close();
    }
}

} // namespace muxserver::client
