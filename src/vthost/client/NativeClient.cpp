// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/NativeClient.hpp>

#include <core/net/Sockets.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/GracefulClose.hpp>
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

namespace
{
    /// Whether @p wire's strings fit what a record keeps, which every record a sane host sends does.
    [[nodiscard]] bool fitsRecordBounds(proto::WireCommandBlock const& wire) noexcept
    {
        return wire.commandLine.size() <= vtbackend::MaxRecordedCommandLineBytes
               && wire.workingDirectory.size() <= vtbackend::MaxRecordedWorkingDirectoryBytes
               && wire.endWorkingDirectory.size() <= vtbackend::MaxRecordedWorkingDirectoryBytes;
    }

    /// Drops @p directory whole when it is too long for a record -- a cut path names a different directory --
    /// and with it @p locality.
    void boundDirectory(std::string& directory, uint8_t& locality)
    {
        if (directory.size() <= vtbackend::MaxRecordedWorkingDirectoryBytes)
            return;
        directory.clear();
        directory.shrink_to_fit(); // what bounds the cost is the bytes kept, not the length
        locality = std::to_underlying(vtbackend::ContextLocality::Unknown);
    }

    /// @p wire held to what a record keeps, exactly as vtbackend::CommandBlockStore::adopt() holds one: its
    /// command line cut at a code point and ended in an ellipsis, a working directory too long dropped
    /// whole -- a cut path names a different directory -- and with it its locality.
    [[nodiscard]] proto::WireCommandBlock boundedToRecord(proto::WireCommandBlock wire)
    {
        if (wire.commandLine.size() > vtbackend::MaxRecordedCommandLineBytes)
            wire.commandLine = vtbackend::truncatedCommandLine(wire.commandLine);
        boundDirectory(wire.workingDirectory, wire.locality);
        boundDirectory(wire.endWorkingDirectory, wire.endLocality);
        return wire;
    }

    /// Forgets the record @p id names in @p screen's command-block table.
    /// @param screen The mirror whose table to update.
    /// @param id The host's id of the record.
    /// @return How many records it forgot: one, or none when the table held no such id.
    std::size_t dropCommandBlock(RemoteScreen& screen, uint32_t id)
    {
        auto const held = screen.commandBlocks.find(id);
        if (held == screen.commandBlocks.end())
            return 0;
        screen.commandBlocksByRevision.erase(held->second.revision);
        screen.commandBlocks.erase(held);
        return 1;
    }

    /// Holds @p sent in @p screen's command-block table as the record its id names, within the table's
    /// bounds (RemoteScreen::commandBlocks). One identical to the record held keeps its revision -- a
    /// resend, or a resync stating what is held, is not news -- and any other is stamped with the next.
    /// @param screen The mirror whose table to update.
    /// @param sent The record as the host sent it.
    /// @return How many records the cap dropped to make room for it: the oldest, or none.
    std::size_t holdCommandBlock(RemoteScreen& screen, proto::WireCommandBlock const& sent)
    {
        // Held to the bounds before it is compared, so resending an over-long record is still a resend;
        // copied only to cut it, which no sane host's record needs.
        auto const bounded = fitsRecordBounds(sent) ? std::nullopt : std::optional { boundedToRecord(sent) };
        auto const& wire = bounded ? *bounded : sent;

        auto& table = screen.commandBlocks;
        auto const maxRecords = screen.commandBlockLimits.maxRecords;
        auto const at = table.lower_bound(wire.id);
        auto const held = at != table.end() && at->first == wire.id;
        if (!held && at == table.begin() && table.size() >= maxRecords)
            return 0; // older than all a full table keeps: the cap would drop it on arrival
        if (held && at->second.wire == wire)
            return 0;

        auto const entry = held ? at : table.emplace_hint(at, wire.id, MirroredCommandBlock {});
        if (held)
            screen.commandBlocksByRevision.erase(entry->second.revision);
        entry->second = MirroredCommandBlock { .wire = wire, .revision = ++screen.commandBlocksRevision };
        screen.commandBlocksByRevision.emplace(entry->second.revision, wire.id);
        return table.size() > maxRecords ? dropCommandBlock(screen, table.begin()->first) : 0;
    }
} // namespace

void RemoteScreen::apply(proto::ProgramStatus const& status)
{
    auto const& records = status.records;
    auto const kept = std::min(records.size(), vtbackend::MaxProgramStatusRecords);
    programStatus.assign(records.end() - static_cast<std::ptrdiff_t>(kept), records.end());
}

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
    sessionEpoch = state.sessionEpoch;
    hostTime = state.hostTime;
    commandBlockIdEpoch = state.commandBlockIdEpoch;

    // REPLACED from the floor on, not merged: a snapshot states every record the host holds at or above
    // its floor, and leaves the ones below — a capped snapshot's floor is the oldest record it states —
    // until a delta retires them; whatever this client lacks there follows as increments. Updated in
    // place: an unchanged record keeps its entry and its stamp, so a resync costs no copy and is not
    // mistaken for news; anything from the floor on that the snapshot no longer names is gone on the host
    // too.
    auto stated = std::vector<uint32_t> {};
    stated.reserve(state.commandBlocks.size());
    auto dropped = std::size_t { 0 };
    for (auto const& wire: state.commandBlocks)
    {
        stated.push_back(wire.id);
        dropped += holdCommandBlock(*this, wire);
    }
    std::ranges::sort(stated);
    auto gone = std::vector<uint32_t> {};
    for (auto const id:
         std::ranges::subrange(commandBlocks.lower_bound(state.commandBlocksFloor), commandBlocks.end())
             | std::views::keys)
        if (!std::ranges::binary_search(stated, id))
            gone.push_back(id);
    for (auto const id: gone)
        dropped += dropCommandBlock(*this, id);
    if (dropped != 0)
        commandBlocksRetiredAt = ++commandBlocksRevision;

    // After the table: the baseline is read off it. A newest record still at its prompt is the shell's own
    // and belongs to what comes next, so the line stays standing until it, or one after it, has run.
    interruptedCommand = vtbackend::truncatedCommandLine(state.interruptedCommand);
    interruptedAfterBlock = 0;
    if (!commandBlocks.empty())
    {
        auto const& [newestId, newest] = *commandBlocks.rbegin();
        interruptedAfterBlock =
            newest.wire.state == std::to_underlying(vtbackend::CommandBlockState::Prompting) ? newestId - 1
                                                                                             : newestId;
    }
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
    stableIdGeneration = delta.stableIdGeneration;
    seqno = delta.seqno;
    viewportBase = delta.stableViewportBase;
    stableFloor = delta.stableFloor;
    evictedRowCount = delta.evictedRowCount;
    commandBlockIdEpoch = delta.commandBlockIdEpoch;
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
    {
        statusLines = delta.statusLines;
        statusLinesDisplay = delta.statusLinesDisplay;
    }
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

    auto dropped = std::size_t { 0 };
    for (auto const& wire: delta.commandBlocks)
        dropped += holdCommandBlock(*this, wire);
    for (auto const id: delta.retiredCommandBlocks)
        dropped += dropCommandBlock(*this, id);
    if (dropped != 0)
        commandBlocksRetiredAt = ++commandBlocksRevision;

    if (!interruptedCommand.empty()
        && std::ranges::any_of(
            std::ranges::subrange(commandBlocks.upper_bound(interruptedAfterBlock), commandBlocks.end())
                | std::views::values,
            [](MirroredCommandBlock const& block) {
                return block.wire.state != std::to_underlying(vtbackend::CommandBlockState::Prompting);
            }))
        interruptedCommand.clear();

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

NativeClient::NativeClient(core::net::EventLoop& loop,
                           std::unique_ptr<core::net::ISocket> connection,
                           HandshakeOptions handshake,
                           vtbackend::CommandBlockStoreLimits commandBlockLimits,
                           UpdateHandler onUpdate,
                           ImageHandler onImage,
                           SessionEventHandler onSessionEvent,
                           LayoutHandler onLayout,
                           PermissionHandler onPermissionRequest,
                           LivenessPolicy liveness,
                           TabMergeRefusedHandler onTabMergeRefused):
    _loop(loop),
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 1 } * 1024 * 1024),
    _historyKeep(resolveHistoryKeep(handshake)), // the parameter, before the move below
    _handshake(std::move(handshake)),
    _blockLimits(commandBlockLimits),
    _onUpdate(std::move(onUpdate)),
    _onImage(std::move(onImage)),
    _onSessionEvent(std::move(onSessionEvent)),
    _onLayout(std::move(onLayout)),
    _onPermissionRequest(std::move(onPermissionRequest)),
    _onTabMergeRefused(std::move(onTabMergeRefused)),
    _liveness(liveness)
{
}

void NativeClient::armLivenessTick()
{
    _livenessTimer.reset();
    _livenessTimer.emplace(
        _loop, _loop.clock().now() + std::chrono::seconds { 1 }, &NativeClient::livenessTick, this);
}

void NativeClient::handshakeExpired(void* self)
{
    auto& client = *static_cast<NativeClient*>(self);
    if (client._closing || client._detached || client._negotiated || client._refusal)
        return;
    client._lossReason = std::format(
        "the daemon did not answer the handshake within {} s",
        std::chrono::duration_cast<std::chrono::seconds>(client._liveness.policy().handshakeDeadline)
            .count());
    errorLog()("attach: {}; closing the connection", *client._lossReason);
    // As the liveness tick gives up: the parked read is retired and run() closes the connection in order.
    client._connection->cancelRead();
}

void NativeClient::livenessTick(void* self)
{
    auto& client = *static_cast<NativeClient*>(self);
    if (client._closing || client._detached)
        return;
    auto const now = client._loop.clock().now();
    switch (client._liveness.decide(now))
    {
        case LivenessAction::None: break;
        case LivenessAction::SendHeartbeat:
            client.send(proto::DecodedPdu { proto::Heartbeat { .nonce = client._nextHeartbeatNonce++ } });
            client._liveness.noteProbeSent(now);
            break;
        case LivenessAction::DeclareLost:
            client._lossReason = std::format(
                "no answer from the daemon for {} s",
                std::chrono::duration_cast<std::chrono::seconds>(client._liveness.policy().deadAfter)
                    .count());
            errorLog()("attach: {}; closing the connection", *client._lossReason);
            // As detach() does: the parked read is retired and run() closes the connection in order. Nothing
            // comes after it, for the read's resumption can end the flow that owns this object.
            client._connection->cancelRead();
            return;
    }
    // Re-armed from inside its own callback, which DeadlineTimer allows: it is settled before it is called.
    client.armLivenessTick();
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
    return _screens.try_emplace(session, _historyKeep, _blockLimits).first->second;
}

uint64_t NativeClient::send(proto::DecodedPdu const& pdu)
{
    // A detached client is ending its connection in order: what it writes now would race the close.
    if (_closing || _detached)
        return 0;
    if (_unsupported.peerRefused(proto::typeOf(pdu)))
    {
        if (protocolTraceLog)
            protocolTraceLog()("attach not sending {}: the daemon does not know it", proto::describe(pdu));
        return 0;
    }
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
        // The queue closes _connection too. A close resumes a parked read at once, which can end
        // the flow that owns this object, so it comes last and is not repeated.
        _writer.close();
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
    // A fetch that was not sent awaits nothing -- and 0 is the serial of every frame the daemon pushes
    // unasked, so recording it would route one of those to this screen.
    if (serial == 0)
        return;
    // The ImageData/ImageGone answer carries no session — remember which one this
    // serial belongs to so the reply lands in the right screen's cache.
    _pendingImages.insert_or_assign(serial, std::pair { session, imageId });
}

void NativeClient::createTab(uint64_t beside, std::string profileName)
{
    send(proto::DecodedPdu {
        proto::CreateTab { .session = beside, .profileName = profileForWire(std::move(profileName)) } });
}

Delivery NativeClient::editTab(proto::EditTab edit)
{
    // A daemon that did not negotiate it reads it as a PDU it does not know: it is not sent at all.
    if (!shares(proto::Capability::TabEdits) || send(proto::DecodedPdu { std::move(edit) }) == 0)
        return Delivery::Unsupported;
    return Delivery::Sent;
}

Delivery NativeClient::mergeTabIntoSplit(std::span<uint64_t const> tabSessions,
                                         uint64_t targetSession,
                                         vtworkspace::SplitSide side)
{
    // A daemon that did not negotiate it reads it as a PDU it does not know: it is not sent at all.
    auto const request = proto::MergeTabIntoSplit { .tabSessions = { tabSessions.begin(), tabSessions.end() },
                                                    .targetSession = targetSession,
                                                    .side = std::to_underlying(side) };
    if (!shares(proto::Capability::TabMerge) || send(proto::DecodedPdu { request }) == 0)
        return Delivery::Unsupported;
    return Delivery::Sent;
}

void NativeClient::restateSessionSettings(std::optional<proto::WireSessionSettings> settings)
{
    // A daemon resolving profiles owns emulation: what this client would restate it ignores anyway.
    if (namesProfiles() || settings == _handshake.sessionSettings)
        return;
    _handshake.sessionSettings = std::move(settings);
    send(proto::DecodedPdu {
        proto::SessionSettingsChanged { .sessionSettings = _handshake.sessionSettings } });
}

void NativeClient::createWindow(std::string profileName)
{
    send(proto::DecodedPdu { proto::NewWindow { .profileName = profileForWire(std::move(profileName)) } });
}

void NativeClient::splitPane(uint64_t session, uint8_t orientation, uint16_t ratio, std::string profileName)
{
    send(proto::DecodedPdu { proto::SplitPane { .session = session,
                                                .orientation = orientation,
                                                .ratio = ratio,
                                                .profileName = profileForWire(std::move(profileName)) } });
}

void NativeClient::resizeSplit(uint64_t firstSession, uint64_t secondSession, uint16_t ratio)
{
    send(proto::DecodedPdu { proto::ResizeSplit {
        .firstSession = firstSession, .secondSession = secondSession, .ratio = ratio } });
}

Delivery NativeClient::closePane(uint64_t session)
{
    return send(proto::DecodedPdu { proto::ClosePane { .session = session } }) != 0 ? Delivery::Sent
                                                                                    : Delivery::Unsupported;
}

void NativeClient::performOperation(proto::PerformOperation request)
{
    send(proto::DecodedPdu { std::move(request) });
}

void NativeClient::answerPermission(std::string hostId, std::string digest, GrantState answer)
{
    // Only a daemon that offered to ask reads an answer; to any other it is a PDU it did not expect.
    if (!_negotiated || !_negotiated->shared.contains(proto::Capability::ExtensionPermissions))
    {
        clientLog()("attach: not answering {}: the daemon did not offer to ask", hostId);
        return;
    }
    send(proto::DecodedPdu { proto::PermissionReply { .hostId = std::move(hostId),
                                                      .digest = std::move(digest),
                                                      .grantState = std::to_underlying(answer) } });
}

void NativeClient::detach()
{
    _detached = true;
    // Retires the parked read so run() leaves its pump and ends the connection in order, with
    // close_notify over TLS. Closing here instead reads as truncation on the daemon's side.
    // Retiring the read can resume run() at once, so nothing comes after it.
    _connection->cancelRead();
}

void NativeClient::handlePdu(proto::DecodedFrame const& frame)
{
    auto const& pdu = frame.pdu;
    if (protocolTraceLog)
        protocolTraceLog()("attach {}",
                           proto::traceLine(proto::Direction::Recv, frame.serial, pdu, frame.consumed));

    // Every frame counts as life, an Unsupported or an echo as much as a Delta (C8).
    _liveness.noteInbound(_loop.clock().now());
    if (auto const* hello = std::get_if<proto::ServerHello>(&pdu))
    {
        handleServerHello(*hello);
        return;
    }
    // Before a ServerHello negotiated, nothing is applied and nothing is answered: the handshake has not
    // said what either end speaks, so a session PDU might be read in a protocol the daemon does not speak.
    if (!_negotiated)
    {
        errorLog()("attach: ignoring {} before the handshake (serial {})",
                   proto::toString(proto::typeOf(pdu)),
                   frame.serial);
        return;
    }
    // An unknown PDU from a newer daemon is answered rather than dropped, and an Unsupported the daemon
    // sent is taken note of -- neither is dispatched further.
    auto const handling = _unsupported.handle(frame);
    switch (handling.verdict)
    {
        case proto::UnsupportedVerdict::NotConcerned: break;
        case proto::UnsupportedVerdict::Answer: send(proto::DecodedPdu { handling.pdu }); return;
        case proto::UnsupportedVerdict::Refused:
            clientLog()("attach: the daemon does not know PDU {}; not sending it again",
                        proto::nameOfIdent(handling.pdu.ident));
            return;
        case proto::UnsupportedVerdict::AlreadyRefused: return;
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
        auto& screen = screenFor(proto::sessionOf(*event));
        // The program-status set is state, not an event: the screen keeps it for a mirror bound later.
        if (auto const* status = std::get_if<proto::ProgramStatus>(&*event))
            screen.apply(*status);
        if (_onSessionEvent)
            _onSessionEvent(screen, *event);
        return;
    }
    if (auto const* layout = std::get_if<proto::LayoutState>(&pdu))
    {
        // A daemon sends the layouts of the windows a connection follows, so one it pushes later -- a window
        // this connection created -- is followed too, and a reconnect resumes it with the rest.
        if (shares(proto::Capability::AttachIntent))
            if (auto const at = std::ranges::lower_bound(_followedWindows, layout->window);
                at == _followedWindows.end() || *at != layout->window)
                _followedWindows.insert(at, layout->window);
        if (_onLayout)
            _onLayout(*layout);
        return;
    }
    if (auto const* request = std::get_if<proto::PermissionRequest>(&pdu))
    {
        if (_onPermissionRequest)
            _onPermissionRequest(*request);
        return;
    }
    if (auto const* refused = std::get_if<proto::TabMergeRefused>(&pdu))
    {
        if (_onTabMergeRefused)
            _onTabMergeRefused(*refused);
        return;
    }
    // A probe's echo: noted as life above, which is all it is for.
    if (std::holds_alternative<proto::Heartbeat>(pdu))
        return;
    // Unknown PDUs are ignored for forward compatibility — but recorded, so a mirror that is
    // quietly missing an update is distinguishable from one that never received it.
    errorLog()(
        "attach: ignoring unexpected {} (serial {})", proto::toString(proto::typeOf(pdu)), frame.serial);
}

std::expected<proto::Negotiated, proto::HandshakeRefusal> judgeServerHello(
    proto::VersionRange const ours, proto::CapabilitySet const& offered, proto::ServerHello const& hello)
{
    // A pre-negotiation daemon states its codec version alone, and with it refuses this client.
    if (hello.layout == proto::HelloLayout::VersionAlone)
        return std::unexpected { proto::HandshakeRefusal {
            .ours = ours, .theirs = { .min = hello.codecVersion, .max = hello.codecVersion } } };
    auto const refused =
        std::unexpected { proto::HandshakeRefusal { .ours = ours, .theirs = hello.versions } };
    if (hello.agreedVersion == 0)
        return refused;
    // Which version both ends speak the daemon picks, by a policy of its own; what this end holds it to is
    // that both do. A pick outside either range is refused as what it is: a daemon of that version alone.
    auto const agreed = hello.agreedVersion;
    auto const within = [agreed](proto::VersionRange range) {
        return range.min <= agreed && agreed <= range.max;
    };
    if (!within(ours) || !within(hello.versions))
    {
        errorLog()("attach: the daemon agreed on protocol v{}, which not both ends speak", agreed);
        return std::unexpected { proto::HandshakeRefusal { .ours = ours,
                                                           .theirs = { .min = agreed, .max = agreed } } };
    }
    return proto::Negotiated { .version = hello.agreedVersion,
                               .peerCapabilities = hello.capabilities,
                               .shared = proto::intersect(offered, hello.capabilities) };
}

std::expected<proto::Negotiated, proto::HandshakeRefusal> NativeClient::judge(
    proto::ServerHello const& hello) const
{
    return judgeServerHello(_handshake.versions, _handshake.capabilities, hello);
}

void NativeClient::handleServerHello(proto::ServerHello const& hello)
{
    // From the first hello only: the handshake, and the identity with it, is the connection's, and a mirror
    // already judged by that identity must not see it change under its feet.
    if (_negotiated || _refusal)
    {
        errorLog()("attach: ignoring a second ServerHello");
        return;
    }
    auto outcome = judge(hello);
    if (!outcome)
    {
        _refusal = outcome.error();
        errorLog()("attach: {}; detaching", proto::describe(*_refusal, proto::PeerRole::Daemon));
        return;
    }
    _daemonIdentity = vtbackend::HostIdentity { .machineId = hello.machineId, .hostname = hello.hostname };
    _daemonRelease = hello.releaseVersion;
    _daemonInstance = hello.daemonInstance;
    _restoredCounts = hello.restoredWindows == 0 && hello.restoredTabs == 0
                          ? std::nullopt
                          : std::optional { RestoredCounts { .windows = hello.restoredWindows,
                                                             .tabs = hello.restoredTabs } };
    _followedWindows = hello.followedWindows;
    _negotiated = std::move(*outcome);
    _handshakeTimer.reset();
    clientLog()("attach: connected (protocol v{})", _negotiated->version);
    // Only a daemon that echoes is probed: an older one would answer every Heartbeat Unsupported.
    if (shares(proto::Capability::Heartbeat))
        armLivenessTick();
    if (_onHandshake)
        _onHandshake();
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

core::async::Task<void> NativeClient::run()
{
    // Silence is measured from the hello, once the daemon answered it; until then the handshake deadline is
    // what gives up on a daemon that never does.
    auto const now = _loop.clock().now();
    _liveness.noteInbound(now);
    _handshakeTimer.emplace(
        _loop, now + _liveness.policy().handshakeDeadline, &NativeClient::handshakeExpired, this);
    send(proto::DecodedPdu { proto::ClientHello { .versions = _handshake.versions,
                                                  .capabilities = _handshake.capabilities,
                                                  .token = _handshake.token,
                                                  .sessionSettings = _handshake.sessionSettings,
                                                  .clientArea = _handshake.clientArea,
                                                  .attachIntent = _handshake.attachIntent,
                                                  .profileName = _handshake.profileName } });

    auto outcome = co_await pumpPdus(
        _connection.get(),
        [this](proto::DecodedFrame const& frame) {
            handlePdu(frame);
            return !_detached && !_refusal;
        },
        [this] { return _detached; });
    // detach() retires the read it parked on, which the pump reports as a cancelled read. That is
    // the detach itself, not a failure.
    // So does a connection given up on, whose loss was logged as it was declared.
    if ((_detached || _lossReason) && outcome.stop == PumpStop::TransportError
        && outcome.transportError->code == core::net::NetErrorCode::Cancelled)
        outcome = PumpResult::stopped(PumpStop::HandlerStopped);
    reportPumpOutcome(outcome);

    _closing = true;
    _handshakeTimer.reset();
    _livenessTimer.reset();
    co_await closeGracefully(&_loop, &_writer, _connection.get());
}

} // namespace vthost::client
