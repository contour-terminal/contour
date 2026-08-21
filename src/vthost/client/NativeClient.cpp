// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/NativeClient.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <net/Sockets.hpp>
#include <vthost/Logging.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/proto/PduTrace.hpp>

namespace vthost::client
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
    defaultForeground = state.defaultForeground;
    defaultBackground = state.defaultBackground;
    statusDisplayType = state.statusDisplayType;
    activeStatusDisplay = state.activeStatusDisplay;
    kittyKeyboardFlags = state.kittyKeyboardFlags;
    modifyOtherKeys = state.modifyOtherKeys;
    mouse = state.mouse;
    progressState = state.progressState;
    progressPercentage = state.progressPercentage;

    // Stated outright, and the ancestry REPLACED rather than merged: a snapshot is the whole truth
    // about the session, so an ancestry that emptied since the last one has to arrive as empty.
    for (auto const& context: state.contexts)
        contexts.insert_or_assign(context.id, context);
    contextChain = state.contextChain;
}

void RemoteScreen::apply(proto::Delta const& delta)
{
    session = delta.session;
    // A snapshot replaces everything the client held — but a snapshot too large for one frame
    // arrives as several, and only its FIRST piece may clear. The piece says which it is rather
    // than the mirror inferring it from the piece before, so a run whose tail was dropped in
    // favour of a newer snapshot still clears when that newer run's first piece lands.
    // Remembered for the reply PDUs that carry no marker of their own. @see snapshotInProgress.
    // No `snapshot != 0` conjunct: completesSnapshot() already answers true for an increment.
    snapshotInProgress = !delta.completesSnapshot();
    if (delta.startsSnapshot())
    {
        rows.clear();
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
    setAnsiModes = delta.setAnsiModes;
    if (delta.titleChanged != 0)
        title = delta.title; // keep it current so a later fullReplay re-titles correctly
    if (delta.cursorShapeChanged != 0)
        cursorShape = delta.cursorShape;
    if (delta.cwdChanged != 0)
        cwd = delta.cwd;
    if (delta.colorsChanged != 0)
    {
        defaultForeground = delta.defaultForeground;
        defaultBackground = delta.defaultBackground;
    }
    if (delta.statusChanged != 0)
    {
        statusDisplayType = delta.statusDisplayType;
        activeStatusDisplay = delta.activeStatusDisplay;
    }
    if (delta.statusLinesChanged != 0)
        statusLines = delta.statusLines;
    if (delta.kittyKeyboardChanged != 0)
        kittyKeyboardFlags = delta.kittyKeyboardFlags;
    if (delta.modifyOtherKeysChanged != 0)
        modifyOtherKeys = delta.modifyOtherKeys;
    if (delta.mouseChanged != 0)
        mouse = delta.mouse;
    if (delta.progressChanged != 0)
    {
        // Kept current for the same reason the title is: a later fullReplay must restate it.
        progressState = delta.progressState;
        progressPercentage = delta.progressPercentage;
    }

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

    for (auto const& context: delta.contexts)
        contexts.insert_or_assign(context.id, context);
    if (delta.contextChanged != 0)
    {
        // The delta carries only the ACTIVE id; the ancestry above it is rebuilt from the records'
        // parent links, which every record carries. Cheaper than resending the whole chain on every
        // command, and it cannot disagree with the records, being derived from them.
        contextChain.clear();
        // Every id is visited at most once, which is what makes the walk TOTAL. A self-reference is
        // not the only cycle a peer can present: the sender's id space is a uint16_t that wraps and
        // reuses, so a long-lived session can legitimately end up with A.parent == B and B.parent == A
        // in this accumulated table, and a bare "stop when parent == id" would then spin forever
        // appending to contextChain until the process ran out of memory.
        auto visited = std::unordered_set<uint16_t> {};
        for (auto id = delta.activeContext; id != 0 && visited.insert(id).second;)
        {
            contextChain.push_back(id);
            auto const it = contexts.find(id);
            if (it == contexts.end())
                break; // unknown parent: the chain is as much of the ancestry as this mirror has
            id = it->second.parent;
        }
        std::ranges::reverse(contextChain); // outermost first, as SessionState spells it
    }

    // Trim client-side scrollback. The server's floor is authoritative — a
    // `clear`/CSI 3 J jumps it up with no line changes, so honoring it is what
    // drops history the real terminal already discarded (otherwise the mirror
    // keeps showing ghost scrollback). The historyKeep cap bounds memory when
    // the floor sits far below the viewport; unset means the floor alone bounds it.
    auto const evictBelow = historyKeep ? std::max(stableFloor, viewportBase - *historyKeep) : stableFloor;
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
    for (auto& [stableId, columns]: imageCells)
        std::erase_if(columns, [imageId](auto const& pair) { return pair.second.imageId == imageId; });
    std::erase_if(imageCells, [](auto const& pair) { return pair.second.empty(); });
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
// NativeClient

NativeClient::NativeClient(net::EventLoop& loop,
                           std::unique_ptr<net::ISocket> connection,
                           HandshakeOptions handshake,
                           UpdateHandler onUpdate,
                           ImageHandler onImage,
                           SessionEventHandler onSessionEvent,
                           LayoutHandler onLayout):
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 1 } * 1024 * 1024),
    _historyKeep(resolveHistoryKeep(handshake)), // the parameter, before the move below
    _handshake(std::move(handshake)),
    _onUpdate(std::move(onUpdate)),
    _onImage(std::move(onImage)),
    _onSessionEvent(std::move(onSessionEvent)),
    _onLayout(std::move(onLayout))
{
}

std::optional<int64_t> NativeClient::resolveHistoryKeep(HandshakeOptions const& handshake)
{
    // No stated profile means no opinion, so the mirror's own ceiling applies -- @see
    // RemoteScreen::DefaultHistoryKeep.
    if (!handshake.sessionSettings)
        return RemoteScreen::DefaultHistoryKeep;

    // Read through the SAME translation the daemon applies to this field rather than restating its
    // rules here: `-1` is unlimited, `0` means the default, and a finite count is clamped to
    // MaxSessionHistoryLineCount. vthost/SessionSettings.h exists precisely so the two ends cannot
    // drift apart on what a field means, and a second reader spelling the conventions out by hand
    // is the parallel field list its file comment warns about.
    auto const settings = fromWireSessionSettings(*handshake.sessionSettings, defaultSessionSettings());
    if (auto const* finite = std::get_if<vtbackend::LineCount>(&settings.historyLimits.capacity))
        return unbox<int64_t>(*finite);
    return std::nullopt; // vtbackend::Infinite -- `history.limit: infinite`
}

RemoteScreen& NativeClient::screenFor(uint64_t session)
{
    return _screens.try_emplace(session, _historyKeep).first->second;
}

uint64_t NativeClient::send(proto::DecodedPdu const& pdu)
{
    auto const serial = _nextSerial++;
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    if (protocolTraceLog)
        protocolTraceLog()("attach {}", proto::traceLine(proto::Direction::Send, serial, pdu, bytes.size()));
    if (!_writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }))
    {
        // The queue's overflow contract: dropping a frame mid-stream (a
        // keystroke, a resize) silently desyncs the daemon — sever instead.
        errorLog()("attach: {}; severing", _writer.describeRefusal());
        _writer.close();
        _connection->close();
    }
    return serial;
}

void NativeClient::sendInput(uint64_t session, std::string_view bytes)
{
    auto input = proto::Input { .session = session, .data = {} };
    auto const* src = reinterpret_cast<std::byte const*>(bytes.data());
    input.data.assign(src, src + bytes.size());
    send(proto::DecodedPdu { input });
}

void NativeClient::requestResize(uint32_t columns, uint32_t lines)
{
    send(proto::DecodedPdu { proto::ResizeRequest { .columns = columns, .lines = lines } });
}

void NativeClient::resizePane(uint64_t session, uint32_t columns, uint32_t lines)
{
    send(proto::DecodedPdu { proto::ResizePane { .session = session, .columns = columns, .lines = lines } });
}

void NativeClient::fetchImage(uint64_t session, uint32_t imageId)
{
    auto const serial =
        send(proto::DecodedPdu { proto::FetchImage { .session = session, .imageId = imageId } });
    // The ImageData/ImageGone answer carries no session — remember which one this
    // serial belongs to so the reply lands in the right screen's cache.
    _pendingImages.insert_or_assign(serial, std::pair { session, imageId });
}

void NativeClient::createTab(uint64_t beside)
{
    send(proto::DecodedPdu { proto::CreateTab { .session = beside } });
}

void NativeClient::createWindow()
{
    send(proto::DecodedPdu { proto::NewWindow {} });
}

void NativeClient::splitPane(uint64_t session, uint8_t orientation, uint16_t ratio)
{
    send(proto::DecodedPdu {
        proto::SplitPane { .session = session, .orientation = orientation, .ratio = ratio } });
}

void NativeClient::resizeSplit(uint64_t firstSession, uint64_t secondSession, uint16_t ratio)
{
    send(proto::DecodedPdu { proto::ResizeSplit {
        .firstSession = firstSession, .secondSession = secondSession, .ratio = ratio } });
}

void NativeClient::closePane(uint64_t session)
{
    send(proto::DecodedPdu { proto::ClosePane { .session = session } });
}

void NativeClient::detach()
{
    _detached = true;
    _writer.close();
    _connection->close();
}

void NativeClient::handlePdu(proto::DecodedFrame const& frame)
{
    auto const& pdu = frame.pdu;
    if (protocolTraceLog)
        protocolTraceLog()("attach {}",
                           proto::traceLine(proto::Direction::Recv, frame.serial, pdu, frame.consumed));

    if (auto const* hello = std::get_if<proto::ServerHello>(&pdu))
    {
        if (hello->codecVersion == proto::CodecVersion)
        {
            _connected = true;
            clientLog()("attach: connected (codec v{})", proto::CodecVersion);
        }
        else
        {
            errorLog()("attach: daemon speaks codec v{}, we speak v{}; detaching",
                       hello->codecVersion,
                       proto::CodecVersion);
            _versionMismatch = true;
        }
        return;
    }
    if (auto const* state = std::get_if<proto::SessionState>(&pdu))
    {
        screenFor(state->session).apply(*state);
        return;
    }
    if (auto const* delta = std::get_if<proto::Delta>(&pdu))
    {
        auto& screen = screenFor(delta->session);
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
        // Publish only a screen that is whole. A mid-run piece leaves the mirror holding part of
        // a grid, and handing that to a frontend would have it repaint a snapshot it has only
        // half received — the rows not yet delivered rendering as absent until the run finishes.
        if (_onUpdate && !screen.snapshotInProgress)
            _onUpdate(screen, *delta);
        return;
    }
    if (auto const* image = std::get_if<proto::ImageData>(&pdu))
    {
        // The reply carries no session; the request serial is what routes it.
        auto const it = _pendingImages.find(frame.serial);
        if (it == _pendingImages.end())
        {
            errorLog()("attach: unrouted ImageData for serial {} (no FetchImage in flight)", frame.serial);
            return;
        }
        auto const [session, imageId] = it->second;
        _pendingImages.erase(it);
        auto& screen = screenFor(session);
        screen.images.insert_or_assign(imageId, *image);
        // Withheld mid-run for the same reason a snapshot piece is: the pixels are recorded either
        // way, and the piece that completes the run places every image it holds.
        if (_onImage && !screen.snapshotInProgress)
            _onImage(screen, imageId);
        return;
    }
    if (std::holds_alternative<proto::ImageGone>(pdu))
    {
        auto const it = _pendingImages.find(frame.serial);
        if (it == _pendingImages.end())
        {
            errorLog()("attach: unrouted ImageGone for serial {} (no FetchImage in flight)", frame.serial);
            return;
        }
        auto const [session, imageId] = it->second;
        _pendingImages.erase(it);
        auto& screen = screenFor(session);
        screen.dropImage(imageId);
        if (_onImage && !screen.snapshotInProgress)
            _onImage(screen, imageId);
        return;
    }
    // Asked once, of the protocol, rather than testing each event tag here — this stays correct when
    // a fourth transient event joins the catalog.
    if (auto const event = proto::asSessionEvent(pdu))
    {
        if (_onSessionEvent)
            _onSessionEvent(screenFor(proto::sessionOf(*event)), *event);
        return;
    }
    if (auto const* layout = std::get_if<proto::LayoutState>(&pdu))
    {
        if (_onLayout)
            _onLayout(*layout);
        return;
    }
    // Unknown PDUs are ignored for forward compatibility — but recorded, so a mirror that is
    // quietly missing an update is distinguishable from one that never received it.
    errorLog()(
        "attach: ignoring unexpected {} (serial {})", proto::toString(proto::typeOf(pdu)), frame.serial);
}

void NativeClient::reportPumpOutcome(PumpResult const& outcome)
{
    if (auto const reason = describe(outcome))
    {
        if (isFailure(outcome.stop))
            errorLog()("attach: {}", *reason);
        else
            clientLog()("attach: {}", *reason);
    }
}

coro::Task<void> NativeClient::run()
{
    send(proto::DecodedPdu { proto::ClientHello { .codecVersion = proto::CodecVersion,
                                                  .token = _handshake.token,
                                                  .sessionSettings = _handshake.sessionSettings } });

    auto const outcome = co_await pumpPdus(_connection.get(), [this](proto::DecodedFrame const& frame) {
        handlePdu(frame);
        return !_detached && !_versionMismatch;
    });
    reportPumpOutcome(outcome);

    if (!_detached)
    {
        co_await _writer.flushThenClose();
        _connection->close();
    }
}

} // namespace vthost::client
