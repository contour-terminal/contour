// SPDX-License-Identifier: Apache-2.0
#include <vthost/NativeSession.hpp>

#include <vtbackend/core/Image.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <core/Utils.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/ContextWire.hpp>
#include <vthost/CursorStyle.hpp>
#include <vthost/DaemonControlWire.hpp>
#include <vthost/GracefulClose.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/Logging.hpp>
#include <vthost/MirroredModes.hpp>
#include <vthost/MouseWire.hpp>
#include <vthost/NotifyWire.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/TimeWire.hpp>
#include <vthost/proto/PduTrace.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

using namespace std::chrono_literals;
using vtworkspace::SessionId;

namespace
{
    /// Validates a client-proposed grid at the protocol boundary.
    ///
    /// The bound is `proto::MaxGridExtent`, shared with the decoder that bounds the grid the server
    /// ANNOUNCES: the same nonsense must be refused whichever direction it travels in.
    /// @param columns Proposed column count.
    /// @param lines Proposed line count.
    /// @return The page size, or nullopt when either axis is out of range (the proposal is dropped).
    [[nodiscard]] std::optional<vtpty::PageSize> sanePageSize(uint32_t columns, uint32_t lines) noexcept
    {
        if (columns < 1 || columns > proto::MaxGridExtent || lines < 1 || lines > proto::MaxGridExtent)
            return std::nullopt;
        return vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(lines)),
                                 .columns = vtpty::ColumnCount(static_cast<int>(columns)) };
    }

    /// Serializes one split-tree node onto the wire (recurses into its children).
    [[nodiscard]] proto::WirePane serializePaneTree(vtworkspace::Pane const& pane)
    {
        auto wire = proto::WirePane {};
        wire.paneId = pane.id().value;
        wire.split = std::to_underlying(pane.splitState());
        if (pane.isLeaf())
            wire.session = pane.session().value;
        else
        {
            wire.ratio = proto::toWireRatio(pane.ratio());
            if (pane.first() != nullptr)
                wire.children.push_back(serializePaneTree(*pane.first()));
            if (pane.second() != nullptr)
                wire.children.push_back(serializePaneTree(*pane.second()));
        }
        return wire;
    }

    /// Serializes the host window's whole tab/pane layout for the LayoutState PDU.
    [[nodiscard]] proto::LayoutState serializeLayout(vtworkspace::Window& window)
    {
        auto layout = proto::LayoutState {};
        layout.window = window.id().value;
        layout.activeTab = static_cast<uint32_t>(std::max(0, window.activeTabIndex()));
        for (auto const tabIndex: std::views::iota(0, window.tabCount()))
        {
            auto* tab = window.tabAt(tabIndex);
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

    /// Whether a record went into a frame or would have overspent its budget.
    enum class RecordFit : uint8_t
    {
        OverBudget = 0, ///< It did not fit; nothing was appended.
        Taken = 1,      ///< Its wire form was appended.
    };

    /// The command-block record bytes one frame may spend, by proto::estimatedEncodedSize, which never
    /// under-counts. The first record always fits, however large, so a lone oversized one still travels
    /// rather than holding everything behind it forever.
    class RecordBudget
    {
      public:
        /// @param bytes The most the frame's records may cost.
        /// @param grid The grid the records' rows are stamped on, which says whether a head row is gone.
        RecordBudget(std::size_t bytes, vtbackend::Grid const& grid) noexcept:
            _bytes { bytes }, _grid { grid }
        {
        }

        /// Converts @p record and appends it to @p out, unless that would overspend the budget.
        /// @return Taken, or OverBudget when it did not fit.
        [[nodiscard]] RecordFit take(vtbackend::CommandBlockRecord const& record,
                                     std::vector<proto::WireCommandBlock>& out)
        {
            auto wire = toWireCommandBlock(record, hostHeadOf(_grid, record));
            auto const cost = proto::estimatedEncodedSize(wire);
            if (_spent != 0 && _spent + cost > _bytes)
                return RecordFit::OverBudget;
            _spent += cost;
            out.push_back(std::move(wire));
            return RecordFit::Taken;
        }

      private:
        std::size_t _bytes;
        vtbackend::Grid const& _grid;
        std::size_t _spent = 0;
    };

    /// Adds the ids of @p sent -- ascending, as a walk takes them -- to @p known, ascending, keeping it a
    /// set.
    void addSentIds(std::vector<uint32_t>& known, std::span<proto::WireCommandBlock const> sent)
    {
        auto const seam = static_cast<std::ptrdiff_t>(known.size());
        for (auto const& block: sent)
            known.push_back(block.id);
        // A walk that met only records newer than every one the peer holds -- the usual case, a new
        // prompt -- appended them in order already.
        auto const middle = std::next(known.begin(), seam);
        if (middle == known.begin() || middle == known.end() || *std::prev(middle) < *middle)
            return;
        std::ranges::inplace_merge(known, middle);
        known.erase(std::ranges::unique(known).begin(), known.end());
    }

    /// Moves every id of @p known that @p store no longer holds to @p retired, by a merge-join: both are
    /// ascending, so one pass over each.
    void retireReleasedIds(vtbackend::CommandBlockStore const& store,
                           std::vector<uint32_t>& known,
                           std::vector<uint32_t>& retired)
    {
        auto const records = store.records();
        auto held = records.begin();
        auto kept = known.begin();
        for (auto const id: known)
        {
            held =
                std::ranges::find_if(held, records.end(), [id](vtbackend::CommandBlockRecord const& record) {
                    return record.id.value >= id;
                });
            if (held != records.end() && held->id.value == id)
                *kept++ = id;
            else
                retired.push_back(id);
        }
        known.erase(kept, known.end());
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
                .alignment = std::to_underlying(rasterized.alignmentPolicy()),
                .resize = std::to_underlying(rasterized.resizePolicy()),
            });
        }
    }

    /// The answer to a hello refused for its version or its token -- the same for both, so a peer
    /// probing the daemon cannot tell which it got wrong.
    /// @param hello The refused hello.
    /// @param ours The versions this endpoint speaks.
    /// @return The version alone for a pre-negotiation peer, which reads nothing more; otherwise the
    ///         agreed version 0 and @p ours, which name no capability and no machine.
    [[nodiscard]] proto::ServerHello refusalFor(proto::ClientHello const& hello, proto::VersionRange ours)
    {
        if (proto::isPreNegotiation(hello.versions))
            return proto::ServerHello { .layout = proto::HelloLayout::VersionAlone };
        return proto::ServerHello { .layout = proto::HelloLayout::Negotiating,
                                    .agreedVersion = 0,
                                    .versions = ours,
                                    .capabilities = {} };
    }

} // namespace

NativeSession::NativeSession(core::net::EventLoop& loop,
                             SessionHost& host,
                             ConnectionId id,
                             std::unique_ptr<core::net::ISocket> connection,
                             std::size_t maxWriteQueueBytes,
                             NativeSessionServices services):
    _loop(loop),
    _host(host),
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), maxWriteQueueBytes),
    _snapshotPieceBytes(snapshotPieceBytes(maxWriteQueueBytes)),
    _id(std::move(id)),
    _services(std::move(services))
{
    // Every model change (the host fans them here once subscribed) re-pushes the
    // whole layout — infrequent, so a full resend beats a granular diff.
    _layoutObserver.onChange = [this] {
        pushLayout();
    };
}

void NativeSession::send(uint64_t serial, proto::DecodedPdu const& pdu, uint64_t sessionTag)
{
    if (_closed)
        return;
    if (_unsupported.peerRefused(proto::typeOf(pdu)))
    {
        if (protocolTraceLog)
            protocolTraceLog()("{} not sending {}: the client does not know it", _id, proto::describe(pdu));
        return;
    }
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    if (protocolTraceLog)
        protocolTraceLog()("{} {}", _id, proto::traceLine(proto::Direction::Send, serial, pdu, bytes.size()));
    if (!_writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() },
                         sessionTag))
    {
        // The queue's overflow contract: a client too slow to drain the byte bound
        // is disconnected, not silently under-served — the delta cursor has already
        // advanced past this frame, so dropping it would leave the mirror holey
        // with no resync trigger. Closing the connection unparks the PDU pump.
        //
        // Reaching this now really does mean a client that stopped reading. It did not always:
        // the queue never refuses a frame for its own size, but that rule only ever protected
        // the FIRST frame of a burst, because a producer that enqueues without suspending never
        // lets the drain — which is spawned onto the loop, not resumed inline — empty the queue
        // between frames. An attach that walked every pane pushing whole grids therefore had to
        // fit its entire payload under the bound at once, and two scrollback-heavy panes did not.
        // Snapshots now go through @ref streamSnapshots, which waits for room before every piece,
        // so the only producer left on this path is the incremental one, whose frames are small
        // by construction.
        errorLog()("{}: {}; disconnecting", _id, _writer.describeRefusal());
        _closed = true;
        // The queue closes _connection too. A close resumes a parked read at once, which can end
        // the flow that owns this object, so it comes last and is not repeated.
        _writer.close();
    }
}

void NativeSession::sessionScreenUpdated(SessionId session)
{
    if (!_negotiated || _closed)
        return;
    queueIncrement(session);
}

void NativeSession::queueIncrement(SessionId session)
{
    _pendingSessions.insert(session.value);
    scheduleFlush();
}

void NativeSession::scheduleFlush()
{
    if (_flushScheduled || _closed)
        return;
    _flushScheduled = true;
    _loop.spawn(flushSoon());
}

void NativeSession::sessionClosed(SessionId session)
{
    _followed.erase(session.value);
    _pendingSessions.erase(session.value);
    // The snapshot bookkeeping forgets it too. The streamer would survive without this (buildPush
    // answers NoSuchSession and it moves on), but every per-session set this connection keeps must
    // be dropped in ONE place, or the next one added is dropped in none.
    std::erase(_snapshotQueue, session.value);
    _recordsAwaitingRoom.erase(session.value);
}

void NativeSession::sessionResized(SessionId session)
{
    // A resize destroys the grid's row identity (a rebuild bumps the generation), so the mirror
    // cannot be brought forward by a delta — it needs the whole grid. Queued rather than pushed
    // inline, and not through the 20ms debounce either: one client's resize re-projects every
    // pane of every window, so this arrives once PER PANE within a single callback. Pushing each
    // one inline is the same burst the attach walk used to be, with the same outcome.
    if (!_negotiated || _closed || !_followed.contains(session.value))
        return;
    requestSnapshot(session);
}

void NativeSession::sessionBell(SessionId session)
{
    emitSessionEvent(proto::SessionBell { .session = session.value });
}

void NativeSession::sessionNotify(SessionId session,
                                  std::string const& title,
                                  std::string const& body,
                                  std::optional<vtbackend::DisplayOccasion> occasion)
{
    emitSessionEvent(proto::SessionNotify {
        .session = session.value, .title = title, .body = body, .occasion = wireOccasionOf(occasion) });
}

void NativeSession::sessionCopyToClipboard(SessionId session, std::string const& data)
{
    // "c" = the CLIPBOARD selection; the client applies its own write permission.
    emitSessionEvent(proto::SessionClipboard { .session = session.value, .selection = "c", .data = data });
}

void NativeSession::sessionPermissionRequested(SessionId /*session*/, proto::PermissionRequest const& request)
{
    if (!asksPermissions() || _closed)
        return;
    send(0, proto::DecodedPdu { request });
}

void NativeSession::emitSessionEvent(proto::SessionEventPdu const& event)
{
    if (!_negotiated || _closed)
        return;
    // Widening the narrow event set into the catalog variant the encoder speaks.
    std::visit([this](auto const& alternative) { send(0, proto::DecodedPdu { alternative }); }, event);
}

void NativeSession::pushLayout()
{
    if (!_negotiated || _closed)
        return;
    // One LayoutState per daemon window (B4) — the client opens a GUI window for
    // each and reconciles its tabs/splits independently.
    auto& model = _host.model();
    for (auto const i: std::views::iota(0, model.windowCount()))
        if (auto* window = model.windowAt(i))
            send(0, proto::DecodedPdu { serializeLayout(*window) });
}

core::async::Task<void> NativeSession::flushSoon()
{
    // Debounce: a busy PTY produces many screenUpdated signals per frame-worth
    // of output; one Delta per ~20ms window is what the client can use anyway.
    co_await _loop.delay(20ms);
    _flushScheduled = false;
    if (_closed)
        co_return;
    // A session whose whole grid is about to be (or is being) re-described gets no increment: the
    // snapshot says everything this would, and one landing BETWEEN a snapshot's pieces would have
    // the mirror apply and repaint a grid it has only half received. Put it back rather than drop
    // it — rows changed after the snapshot was captured are still news. The reinsert targets the
    // member, which `exchange` just emptied, not the range being walked.
    for (auto const session: std::exchange(_pendingSessions, {}))
        if (snapshotPending(SessionId { session }))
            _pendingSessions.insert(session);
        else
            pushDelta(SessionId { session });
    // Something was held back and nothing else will re-arm the debounce: a session whose output
    // has stopped raises no further screen update, so without this its last increment would wait
    // for one that never comes.
    if (!_pendingSessions.empty())
        scheduleFlush();
}

void NativeSession::collectContextState(vtbackend::Terminal& terminal,
                                        FollowState& follow,
                                        proto::Delta& delta,
                                        SnapshotMode mode)
{
    // The ACTIVE id is pull+diff like the cwd is, and the records it and its ancestors need travel as
    // a side table in the same "sent once per connection on first reference" shape the hyperlink pool
    // uses -- so a client already told about a context is not told again on every command.
    auto const& contexts = terminal.contexts();
    auto const active = static_cast<int>(contexts.activeId().value);
    if (active != follow.lastActiveContext)
    {
        if (mode == SnapshotMode::Delta)
        {
            delta.contextChanged = 1;
            delta.activeContext = contexts.activeId().value;
        }
        follow.lastActiveContext = active;
    }

    // Every record the ancestry needs, plus any whose metadata moved. Compared as the WHOLE wire
    // record, not by identifier alone: a re-`start=` reinitialises a context IN PLACE under the same
    // id and the same identifier, and `end=` records an outcome on one, so an identifier-keyed gate
    // would replicate a shell context's first `cwd=` and then never again -- leaving an attached pane
    // resolving a directory the session left long ago, and never learning how a command ended.
    //
    // Gated on the stack's revision, which is what that counter is for: this runs per flush (20ms, per
    // session) and the pool holds up to maxRetained records, so walking it unconditionally spent the
    // common case -- a session where nothing about the ancestry moved -- on a scan that can have
    // nothing to say. @see ContextStack::revision.
    if (follow.contextRevisionKnown && contexts.revision() == follow.lastContextRevision)
        return;

    follow.lastContextRevision = contexts.revision();
    follow.contextRevisionKnown = true;
    // Rebuilt rather than merged into, so the table tracks the sender's own retention bound instead of
    // accumulating one entry per context the session ever created.
    auto refreshed = decltype(follow.sentContexts) {};
    contexts.forEachRecord([&](vtbackend::TerminalContext const& record) {
        auto wire = toWireContext(record);
        auto const id = record.id.value;
        auto const known = follow.sentContexts.find(id);
        auto const unchanged = known != follow.sentContexts.end() && known->second == wire;
        if (!unchanged && mode == SnapshotMode::Delta)
            delta.contexts.push_back(wire);
        refreshed.insert_or_assign(id, std::move(wire));
    });
    follow.sentContexts = std::move(refreshed);
}

NativeSession::CommandBlockBacklog NativeSession::collectCommandBlockSnapshot(
    vtbackend::Terminal& terminal,
    FollowState& follow,
    proto::SessionState& state,
    SupersededFrames superseded) const
{
    auto const& store = terminal.commandBlocks();
    auto const revision = store.contentRevision();

    // Stated outright, like the context pool — but capped, because SessionState is one unchunked frame
    // and a store of hostile 4 KB command lines would otherwise make it megabytes. Capped at this
    // connection's piece rather than at SnapshotChunkBytes, for the reason a snapshot's rows are cut to
    // it (snapshotPieceBytes). The NEWEST records are the ones a client wants first (the current prompt,
    // the last finish), so they are taken newest first until the cap is spent; the older remainder is
    // left to a walk, which the increments carry.
    auto& stated = state.commandBlocks;
    auto budget = RecordBudget { _snapshotPieceBytes, terminal.primaryScreen().grid() };
    for (auto const& record: store.records() | std::views::reverse)
        if (budget.take(record, stated) == RecordFit::OverBudget)
            break;
    std::ranges::reverse(stated);

    // Uncapped, the snapshot speaks for every id, and the client's table becomes the store as of
    // `revision`. Capped, it speaks only from the oldest record it states on: the client keeps what it
    // holds below that, so a resize does not have it drop the history it has and receive it all again.
    // Unless the supersede dropped frames this peer counted as sent: what they carried or retired never
    // arrives, so nothing the client holds below the cap can be trusted, and it speaks for every id.
    auto const capped = stated.size() < store.size();
    auto const floor = capped && superseded == SupersededFrames::None ? stated.front().id : uint32_t { 0 };
    state.commandBlocksFloor = floor;
    std::erase_if(follow.sentBlockIds, [floor](uint32_t id) { return id >= floor; });
    for (auto const& wire: stated)
        follow.sentBlockIds.push_back(wire.id);
    if (floor == 0)
    {
        // It holds exactly the stated set: nothing to retire, and — capped — nothing older counts as sent.
        follow.blockIdsJoinedAt = revision;
        follow.sentBlockStamp = capped ? 0 : revision;
    }
    if (!capped)
    {
        follow.blockWalk.reset();
        return CommandBlockBacklog::Drained;
    }

    // The older remainder goes as increments. A walk already open resumes where it stood, having brought
    // what it passed up to its own target; the records from the floor on are current as of `revision`.
    auto const until = vtbackend::CommandBlockId { stated.front().id };
    follow.blockWalk = floor != 0 && follow.blockWalk
                           ? CommandBlockWalk { .from = follow.blockWalk->from,
                                                .until = until,
                                                .target = follow.blockWalk->target }
                           : CommandBlockWalk { .until = until, .target = revision };
    return CommandBlockBacklog::Pending;
}

NativeSession::CommandBlockBacklog NativeSession::collectCommandBlockDelta(vtbackend::Terminal& terminal,
                                                                           FollowState& follow,
                                                                           proto::Delta& delta) const
{
    auto const& store = terminal.commandBlocks();
    auto const revision = store.contentRevision();

    // The gate, as for contexts: a debounce flush runs every 20 ms per session, and the walk below can
    // have nothing to say while the store's content has not moved past what this peer was sent. Head
    // placement leaves the content revision alone, so a reflow or an erase does not open it.
    if (!follow.blockWalk && follow.sentBlockStamp == revision)
        return CommandBlockBacklog::Drained;

    // Rows must flow and records need not: they are state the next delta carries just as well. So
    // under a slow reader the records lag, rather than riding every window onto a backlog until the
    // queue refuses a frame and the connection goes. Nothing is lost by waiting — the walk stays where
    // it stood, and the stamps still say what changed since.
    if (!hasSendRoom())
        return CommandBlockBacklog::Pending;

    // Everything this peer was told about that the host let go of: a prompt the next one discarded,
    // the oldest record the bound evicted, a reset. The mirror removes them; without this an attached
    // pane keeps a phantom block per empty Enter. Only a store that moved can have let any go.
    if (follow.blockIdsJoinedAt != revision)
    {
        retireReleasedIds(store, follow.sentBlockIds, delta.retiredCommandBlocks);
        follow.blockIdsJoinedAt = revision;
    }

    auto& walk = follow.blockWalk ? *follow.blockWalk
                                  : follow.blockWalk.emplace(CommandBlockWalk { .target = revision });
    auto budget = RecordBudget { std::min(CommandBlockDeltaBytes, _snapshotPieceBytes),
                                 terminal.primaryScreen().grid() };
    auto const stamped = store.stampedRecords();
    auto const resume = std::ranges::lower_bound(
        stamped, walk.from, std::less {}, [](vtbackend::StampedCommandBlock const& entry) {
            return entry.record.id;
        });
    for (auto const entry: std::ranges::subrange(resume, stamped.end()))
    {
        if (walk.until && entry.record.id >= *walk.until)
            break;
        if (entry.changeStamp <= follow.sentBlockStamp)
            continue;
        // Once over budget, everything newer waits too, so the peer still learns of records oldest
        // first — which is what lets its mirror tell a finish it witnessed from history it was never
        // shown. The next increment resumes right here.
        if (budget.take(entry.record, delta.commandBlocks) == RecordFit::OverBudget)
        {
            walk.from = entry.record.id;
            addSentIds(follow.sentBlockIds, delta.commandBlocks);
            return CommandBlockBacklog::Pending;
        }
    }
    addSentIds(follow.sentBlockIds, delta.commandBlocks);

    // The walk reached its end: everything stamped up to its target travelled. A record that changed
    // after the walk began, behind where it stood, is still owed — so a store that moved meanwhile asks
    // for one more.
    follow.sentBlockStamp = walk.target;
    follow.blockWalk.reset();
    return follow.sentBlockStamp == revision ? CommandBlockBacklog::Drained : CommandBlockBacklog::Pending;
}

void NativeSession::collectLiveState(vtbackend::Terminal& terminal,
                                     FollowState& follow,
                                     proto::Delta& delta,
                                     std::optional<proto::SessionState>& state,
                                     SessionId session,
                                     uint8_t screenTypeValue,
                                     SnapshotMode mode)
{
    auto const snapshot = mode == SnapshotMode::Forced;
    // Live window title (OSC 0/2): the snapshot carries it in SessionState
    // below; an incremental delta carries it only when it changed since last
    // sent, so a title-only batch still re-titles the mirror. Compare the
    // string_view windowTitle() returns directly and allocate only when it
    // actually changed — the title almost never differs frame-to-frame, so a
    // per-delta heap copy just to compare it was pure waste under a busy PTY.
    if (auto const title = terminal.windowTitle(); title != follow.lastTitle)
    {
        if (!snapshot)
        {
            delta.titleChanged = 1;
            delta.title = std::string { title };
        }
        follow.lastTitle = title;
    }

    // Live cursor shape (DECSCUSR): pull+diff like the title; the snapshot
    // carries it in SessionState below.
    auto const cursorPs = decscusrPs(terminal.cursorShape(), terminal.cursorDisplay());
    auto const cursorShapePs =
        static_cast<int>(cursorPs); // widened to diff against the -1 "unknown" sentinel
    if (cursorShapePs != follow.lastCursorShape)
    {
        if (!snapshot)
        {
            delta.cursorShapeChanged = 1;
            delta.cursorShape = cursorPs;
        }
        follow.lastCursorShape = cursorShapePs;
    }

    // Live working directory (OSC 7): pull+diff; the snapshot carries it in
    // SessionState below. Only propagate once one was actually set.
    auto const& cwd = terminal.currentWorkingDirectory();
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

    collectContextState(terminal, follow, delta, mode);

    // Live default fg/bg (OSC 10/11): pull+diff; the snapshot carries them in
    // SessionState below.
    auto const& colors = terminal.colorPalette();
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
    auto const statusType = static_cast<int>(std::to_underlying(terminal.statusDisplayType()));
    auto const activeStatus = static_cast<int>(std::to_underlying(terminal.activeStatusDisplay()));
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

    // Status-line CONTENT the client cannot draw itself (a separate page), its whole (tiny) grid so the
    // client paints it. Full resend on change — no stable-id delta for one row.
    //  - The host-writable status line, when shown: the application's custom status line.
    //  - The indicator row, when the session's status-line extensions draw it: they run here, and the
    //    client has no frame of theirs. The host repaints that row as their frames arrive and output does.
    //    Once they stop drawing it, the client is told so by an empty indicator page, and draws its own.
    //    Whether the host's terminal shows its indicator or not: a client decides that for itself.
    auto const statusRows =
        [&]() -> std::optional<std::pair<vtbackend::StatusDisplayType, vtbackend::Screen const*>> {
        if (terminal.statusDisplayType() == vtbackend::StatusDisplayType::HostWritable)
            return std::pair { vtbackend::StatusDisplayType::HostWritable,
                               &terminal.hostWritableStatusLineDisplay() };
        if (terminal.indicatorStatusLineDrivenByExtensions())
            return std::pair { vtbackend::StatusDisplayType::Indicator,
                               &terminal.indicatorStatusLineDisplay() };
        return std::nullopt;
    }();
    if (statusRows)
    {
        auto const [type, statusScreen] = *statusRows;
        auto const display = static_cast<uint8_t>(std::to_underlying(type));
        auto const& statusGrid = statusScreen->grid();
        auto rows = std::vector<proto::WireLine> {};
        for (auto const i: std::views::iota(0, unbox<int>(statusGrid.pageSize().lines)))
            rows.push_back(toWireLine(
                statusGrid, vtbackend::LineOffset(i), statusGrid.lineAt(vtbackend::LineOffset(i))));
        if (snapshot || display != follow.lastStatusLinesDisplay || rows != follow.lastStatusLines)
        {
            delta.statusLinesChanged = 1;
            delta.statusLinesDisplay = display;
            delta.statusLines = rows;
        }
        follow.lastStatusLines = std::move(rows);
        follow.lastStatusLinesDisplay = display;
    }
    else if (follow.lastStatusLinesDisplay == std::to_underlying(vtbackend::StatusDisplayType::Indicator))
    {
        delta.statusLinesChanged = 1;
        delta.statusLinesDisplay = follow.lastStatusLinesDisplay;
        follow.lastStatusLines.clear();
        follow.lastStatusLinesDisplay = 0;
    }

    // Live Kitty keyboard protocol flags (pull+diff): the top of the app's
    // CSI-u flag stack governs how KEYS are encoded, so the client must track
    // it to send input the way the app negotiated. Only the effective (current)
    // flags matter to a mirror — the stack itself stays server-side.
    auto const kittyFlags = static_cast<int>(terminal.keyboardProtocol().flags().value());
    if (kittyFlags != follow.lastKittyKeyboardFlags)
    {
        if (!snapshot)
        {
            delta.kittyKeyboardChanged = 1;
            delta.kittyKeyboardFlags = static_cast<uint8_t>(kittyFlags);
        }
        follow.lastKittyKeyboardFlags = kittyFlags;
    }

    // xterm's modifyOtherKeys level (XTMODKEYS resource 4) — the other protocol an app
    // uses to ask for modified keys as escape sequences. Mirrored for the same reason as
    // the Kitty flags: the client's InputGenerator, not the server's, encodes the keys.
    auto const modifyOtherKeys = terminal.modifyOtherKeys();
    if (modifyOtherKeys != follow.lastModifyOtherKeys)
    {
        if (!snapshot)
        {
            delta.modifyOtherKeysChanged = 1;
            delta.modifyOtherKeys = static_cast<uint8_t>(modifyOtherKeys);
        }
        follow.lastModifyOtherKeys = modifyOtherKeys;
    }

    // The RESOLVED mouse-reporting state (pull+diff). It deliberately does not ride the
    // mirrored-mode table: nine DEC modes write only these three values, so a client replaying the
    // mode SET as a sequence of toggles cannot land on the state the application actually chose.
    // @see vthost/MirroredModes.h.
    auto const mouse = proto::MouseState {
        .protocol = mouseProtocolNumber(terminal.mouseProtocol()),
        .transport = static_cast<uint8_t>(terminal.mouseTransport()),
        .wheelMode = static_cast<uint8_t>(terminal.mouseWheelMode()),
    };
    if (mouse != follow.lastMouse)
    {
        if (!snapshot)
        {
            delta.mouseChanged = 1;
            delta.mouse = mouse;
        }
        follow.lastMouse = mouse;
    }

    // The progress indicator (OSC 9;4), pull+diff as one value: its two fields change together, and
    // the snapshot carries it in SessionState below so a re-attaching client sees a bar still in flight.
    auto const progress = terminal.progress();
    if (progress != follow.lastProgress)
    {
        if (!snapshot)
        {
            delta.progressChanged = 1;
            delta.progressState = static_cast<uint8_t>(std::to_underlying(progress.state));
            delta.progressPercentage = progress.percentage;
        }
        follow.lastProgress = progress;
    }

    if (snapshot)
    {
        auto& snap = state.emplace();
        snap.session = session.value;
        auto const size = terminal.pageSize();
        snap.columns = unbox<uint32_t>(size.columns);
        snap.lines = unbox<uint32_t>(size.lines);
        snap.screenType = screenTypeValue;
        snap.cursorLine = delta.cursorLine;
        snap.cursorColumn = delta.cursorColumn;
        snap.title = terminal.windowTitle();
        snap.cursorShape = cursorPs;
        snap.cwd = terminal.currentWorkingDirectory();
        // Stated outright, including when EMPTY: a snapshot routes this through SessionState rather
        // than through a delta's changed-gate, so an ancestry that emptied would otherwise never be
        // delivered -- and the server, having recorded it as sent, would never re-send it.
        terminal.contexts().forEachRecord([&](vtbackend::TerminalContext const& record) {
            snap.contexts.push_back(toWireContext(record));
        });
        for (auto const& entry: terminal.contexts().chain())
            snap.contextChain.push_back(entry.record->id.value);
        snap.sessionEpoch = toWireTime(terminal.sessionEpoch());
        snap.hostTime = toWireTime(terminal.wallTimeNow());
        snap.defaultForeground = colors.defaultForeground.value();
        snap.defaultBackground = colors.defaultBackground.value();
        snap.statusDisplayType = static_cast<uint8_t>(statusType);
        snap.activeStatusDisplay = static_cast<uint8_t>(activeStatus);
        snap.kittyKeyboardFlags = static_cast<uint8_t>(kittyFlags);
        snap.modifyOtherKeys = static_cast<uint8_t>(modifyOtherKeys);
        snap.mouse = mouse;
        snap.progressState = static_cast<uint8_t>(std::to_underlying(progress.state));
        snap.progressPercentage = progress.percentage;
    }
}

std::expected<NativeSession::BuiltPush, NativeSession::BuildFailure> NativeSession::buildPush(
    SessionId session, SnapshotMode mode)
{
    auto* terminal = _host.terminal(session);
    if (terminal == nullptr)
        return std::unexpected(BuildFailure::NoSuchSession);
    auto& follow = _followed[session.value];

    auto delta = proto::Delta {};
    delta.session = session.value;
    auto state = std::optional<proto::SessionState> {};

    auto hyperlinkIds = std::vector<uint16_t> {};
    auto referencedLinks = std::unordered_set<uint16_t> {}; ///< Deduplicates ids within THIS delta.
    auto blockBacklog = CommandBlockBacklog::Drained;
    auto superseded = SupersededFrames::None;
    {
        // The same lock discipline as refreshRenderBuffer: all grid queries
        // happen under the terminal's state lock — and so does every terminal
        // read below (pageSize/screenType/windowTitle mutate on the session's
        // pump thread; windowTitle() in particular is an unlocked reference). A
        // reader's hold, which sends nothing as it is released.
        auto const guard = std::lock_guard { std::as_const(*terminal) };

        // Mirror exactly what the fat GUI paints: the DISPLAYED page, which is what
        // the user sees. It coincides with the cursor page while DECPCCM couples
        // them, but a decoupled display — or any DEC page 1..14 reached via NPP/PP/
        // PPA — shows a different page than VT output currently targets. Serializing
        // currentScreen() (the cursor page, and during an active status display the
        // status screen) would mirror the wrong grid.
        auto const displayedPage = terminal->displayedPageIndex();
        auto& grid = terminal->displayedPage().grid();

        // Each of the 16 pages is a distinct grid whose generation advances
        // independently (and can collide across pages), so one cursor cannot span a
        // page flip: treat it as a wholesale identity change and resync. Keying on
        // the page INDEX (not screenType, which collapses DEC pages 1..14 to
        // "Alternate") is what makes page<->page switches mirror correctly. This
        // also gives a session's very first push its SessionState (nullopt != page).
        // screenTypeFromPage then maps the displayed page to the wire's primary(0)/
        // alt-like(1) discriminator the mirror uses to toggle ?1049 and scrollback.
        auto const screenType = vtbackend::screenTypeFromPage(displayedPage);
        auto const pageFlipped = follow.lastDisplayedPage != displayedPage;
        follow.lastDisplayedPage = displayedPage;
        // A page flip needs the whole grid, and the whole grid belongs on the paced path. Bail
        // before collecting anything: only lastDisplayedPage has moved, and the Forced rebuild
        // that follows re-derives it, so nothing this connection believes about the peer is wrong.
        if (mode == SnapshotMode::Delta && pageFlipped)
            return std::unexpected(BuildFailure::ResyncRequired);
        // Past the two bails, this is exactly the mode asked for: Delta mode reached here only by
        // not needing a snapshot, and Forced never needs anything else.
        auto const snapshot = mode == SnapshotMode::Forced;

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

        // Both conditions mean the same thing: an incremental delta cannot describe this
        // batch. forEachLineChangedSince scans back only as far as scrolledOutDepthSince,
        // which clamps at the scrollback floor — so rows that scrolled past the floor since
        // this connection last looked are unnameable, and a client scrolls every unreported
        // id through its page as a BLANK row. A snapshot leaves them honestly absent.
        auto const cursorBelowFloor = follow.cursor.generation == grid.generation()
                                      && follow.cursor.stableBase < grid.stableRangeFloor();
        if (!snapshot
            && (cursorBelowFloor
                || grid.forEachLineChangedSince(follow.cursor, collect)
                       == vtbackend::GridDeltaResult::ResyncRequired))
            // Same as the page flip above, and bailing here is equally clean: `collect` has only
            // filled locals, and the follow state's cursor, hyperlinks and last-sent values are
            // all still untouched. A caller told this asks for a snapshot instead.
            return std::unexpected(BuildFailure::ResyncRequired);
        if (snapshot)
        {
            // Nothing to discard first: `collect` runs only on the incremental scan above, which
            // Forced mode short-circuits past and which Delta mode returns from rather than
            // upgrading. Clearing here would imply otherwise.
            grid.forEachValidLine(collect);
            // The snapshot delivered the whole grid, so re-anchor the cursor to the
            // stream head directly -- forEachValidLine leaves it untouched, and a
            // second forEachLineChangedSince purely to advance it would rescan.
            grid.anchorCursorToHead(follow.cursor);

            // Everything still queued for this session is about to be re-described in full,
            // so send the snapshot INSTEAD of them rather than behind them. Without this a
            // burst of resyncs (a window drag, or an attach immediately followed by the
            // client asserting its area) stacks whole grids in the queue until it overflows
            // and the client is dropped — for frames the mirror would have thrown away.
            // A dropped frame may have carried hyperlink URIs this connection had recorded as
            // sent; the loop below skips ids it believes the client already has, so those URIs
            // would never arrive. Forgetting them makes the snapshot self-contained again.
            // Conditional because clearing costs a full URI re-send, and with an empty backlog
            // — the overwhelmingly common case — nothing was dropped and nothing is stale.
            if (_writer.dropTagged(session.value) != 0)
            {
                follow.sentHyperlinks.clear();
                // Same reasoning, same fix: a context record whose only mention was in a dropped
                // delta would leave the client with lines pointing at a record it never received.
                follow.sentContexts.clear();
                // And the revision gate with it: the pool has not moved, so without this the re-send
                // the clear exists to force would be skipped and the records never sent again.
                follow.contextRevisionKnown = false;
                // And the command-block records: what a dropped delta carried or retired never arrives,
                // so the snapshot states its records as the client's whole table.
                superseded = SupersededFrames::Dropped;
            }
        }
        delta.snapshot = snapshot ? 1 : 0;
        delta.generation = grid.generation();
        delta.stableIdGeneration = grid.stableIdGeneration();
        delta.seqno = grid.seqno();
        delta.stableViewportBase = grid.stableLineIdOf(vtbackend::LineOffset(0));
        // The scrollback floor: a `clear`/CSI 3 J evicts history without a
        // generation bump or any line change, so this is the ONLY signal that
        // tells the mirror to drop the history the real terminal discarded.
        delta.stableFloor = grid.stableRangeFloor();
        // Its row number: the client numbers absolute lines from the host's count, not from the floor
        // moves its own grid happens to see. It moves only with the floor or a generation, so the
        // floor gate below covers it.
        delta.evictedRowCount = grid.evictedRowCount();

        auto const cursor = terminal->displayedPage().cursor().position;
        delta.cursorLine = unbox<int32_t>(cursor.line);
        delta.cursorColumn = unbox<int32_t>(cursor.column);

        // When DECPCCM is reset and the cursor sits on a page other than the one
        // displayed, the fat GUI hides the cursor (it belongs to a page the user is
        // not looking at). Mirror that by withholding VisibleCursor (DECTCEM/mode
        // 25) from the mode set, exactly as if the app had hidden it.
        //
        // The same while DECSASD selects the host-writable status line, whose cursor the fat GUI then shows
        // on the status line's row: its position does not travel -- the cursor sent above is the page on
        // display's -- so the mirror could draw it only at its own status line's column 0.
        auto const cursorShownWhereKnown =
            terminal->cursorPageIndex() == displayedPage
            && terminal->activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main;
        for (auto const mode: MirroredModes)
        {
            if (mode == vtbackend::DECMode::VisibleCursor && !cursorShownWhereKnown)
                continue;
            if (terminal->isModeEnabled(mode))
                delta.setModes.push_back(vtbackend::toDECModeNum(mode));
        }
        // The ANSI modes ride their own field: DEC 20 and ANSI 20 are different modes, so one list
        // could not say which it meant. @see vthost/MirroredModes.h for the table and for why only
        // LNM is in it.
        for (auto const mode: MirroredAnsiModes)
            if (terminal->isModeEnabled(mode))
                delta.setAnsiModes.push_back(vtbackend::toAnsiModeNum(mode));

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

        collectLiveState(*terminal, follow, delta, state, session, std::to_underlying(screenType), mode);
        // What the ids on the rows and records count in, stated on every push -- a snapshot's pieces
        // included -- so a receiver learns that they started over in the update that starts them over.
        auto const idEpoch = terminal->commandBlocks().idEpoch();
        delta.commandBlockIdEpoch = idEpoch;
        // After collectLiveState, which engages `state` exactly on a snapshot: a snapshot states its
        // records there, an increment carries them in the delta.
        if (state)
        {
            state->commandBlockIdEpoch = idEpoch;
            blockBacklog = collectCommandBlockSnapshot(*terminal, follow, *state, superseded);
        }
        else
            blockBacklog = collectCommandBlockDelta(*terminal, follow, delta);
    }

    // Records left owed — a budget or a cap cut the walk short, or the send queue had no room — follow
    // whether or not the session writes anything meanwhile: an idle shell after a flood raises no further
    // screen update, and a snapshot's capped remainder is owed however quiet the session is.
    if (blockBacklog == CommandBlockBacklog::Pending)
        followUpRecords(session);

    // A pure mode flip (an app enabling mouse tracking, say) changes no cell,
    // yet clients must hear about it to encode input correctly. A pure cursor move
    // (a full-screen app repositioning with no visible cell change) likewise
    // carries only the new cursor position, but the mirror must still get it or its
    // cursor lags until the next cell write.
    // Everything the delta itself knows is asked of the delta (proto::Delta::hasChanges), so a new
    // gated field cannot be forgotten here; only the two facts about THIS peer are added.
    auto const modesChanged =
        delta.setModes != follow.lastModes || delta.setAnsiModes != follow.lastAnsiModes;
    auto const cursorMoved =
        delta.cursorLine != follow.lastCursorLine || delta.cursorColumn != follow.lastCursorColumn;
    // A scrollback eviction (`clear`/CSI 3 J, tmux clear-history) reaches Grid::clearHistory, which
    // deliberately bumps no generation and dirties no line — so the floor is the ONLY thing that
    // moves, and the delta itself cannot tell that it carries news (hasChanges knows nothing about
    // whom it is being sent to). Without this gate the PDU was dropped and the mirror kept rendering
    // history the terminal had thrown away, with `floorOutranScroll` — its detector for exactly this
    // case — waiting on a delta that never arrived.
    auto const floorMoved = delta.stableFloor != follow.lastStableFloor;
    if (!(modesChanged || cursorMoved || floorMoved || delta.hasChanges()))
        return std::unexpected(BuildFailure::NothingToSay);

    follow.lastModes = delta.setModes;
    follow.lastAnsiModes = delta.setAnsiModes;
    follow.lastCursorLine = delta.cursorLine;
    follow.lastCursorColumn = delta.cursorColumn;
    follow.lastStableFloor = delta.stableFloor;
    return BuiltPush { .delta = std::move(delta), .state = std::move(state) };
}

void NativeSession::pushDelta(SessionId session)
{
    auto built = buildPush(session, SnapshotMode::Delta);
    if (!built)
    {
        switch (built.error())
        {
            case BuildFailure::ResyncRequired:
                // The increment could not describe the batch. The whole grid can, and the whole grid
                // is exactly what must not go out inline.
                requestSnapshot(session);
                break;
            case BuildFailure::NoSuchSession:
            case BuildFailure::NothingToSay: break;
        }
        return;
    }
    // No SessionState arm: it accompanies a snapshot only, and an increment that wanted to become
    // one reported ResyncRequired rather than building it.
    send(0, proto::DecodedPdu { std::move(built->delta) }, session.value);
}

bool NativeSession::snapshotPending(SessionId session) const noexcept
{
    return _streamingSession == session.value || std::ranges::contains(_snapshotQueue, session.value);
}

void NativeSession::requestSnapshot(SessionId session)
{
    if (!_negotiated || _closed)
        return;
    // Coalesced against the QUEUE only, not against the session being streamed: a request that
    // arrives mid-run describes state the run in flight was captured before, so it earns a fresh
    // snapshot once that one finishes.
    if (std::ranges::contains(_snapshotQueue, session.value))
        return;
    // Claimed BEFORE the spawn, not by the coroutine once it runs: `spawn` only queues the handle,
    // so between here and its first resumption run()'s teardown poll could observe no streamer and
    // let `this` be destroyed under a coroutine that still holds it.
    // The queue test is belt-and-braces: past the _closed check above, a non-empty queue always
    // has a live streamer, so !_streamingSession alone would do.
    auto const idle = _snapshotQueue.empty() && !_streamingSession;
    _snapshotQueue.push_back(session.value);
    if (!idle)
        return;
    _streamingSession = session.value;
    _loop.spawn(streamSnapshots());
}

core::async::Task<bool> NativeSession::awaitSendRoom()
{
    co_await _writer.waitUntilBacklogBelow(_snapshotPieceBytes, [this] { return _closed; });
    co_return !_closed;
}

core::async::Task<void> NativeSession::awaitInputRoom()
{
    if (!_inputBackedUp)
        co_return;
    // Polled: the session drains on its pump thread, and only a connection that filled it waits here, for as
    // long as the application leaves its input unread. An ended session holds none.
    auto const session = *_inputBackedUp;
    co_await core::net::pollUntil(
        &_loop,
        [this, session] { return _host.inputBacklog(session) <= _host.inputWatermarks().low; },
        std::chrono::milliseconds { 5 });
    _inputBackedUp.reset();
}

void NativeSession::pauseReadingIfBackedUp(SessionId session)
{
    if (_host.inputBacklog(session) >= _host.inputWatermarks().high)
        _inputBackedUp = session;
}

bool NativeSession::hasSendRoom() const noexcept
{
    // The comparison waitUntilBacklogBelow makes against the same watermark, so "room" means one
    // thing on this connection whether the asker can wait for it or not.
    return _writer.backlogBytes() < _snapshotPieceBytes;
}

void NativeSession::followUpRecords(SessionId session)
{
    // With room, the next debounce window carries what is owed. Without it, every window would only
    // rebuild the session — the lock, the row scan, the live state — to find no room again, for as long
    // as the client reads nothing: so the records wait for room instead, on one waiter for the whole
    // connection, as the snapshot streamer does.
    if (hasSendRoom())
    {
        queueIncrement(session);
        return;
    }
    _recordsAwaitingRoom.insert(session.value);
    if (_roomWaiter == RoomWaiter::Parked || _closed)
        return;
    // Claimed before the spawn, for the reason requestSnapshot claims the streamer before its own.
    _roomWaiter = RoomWaiter::Parked;
    _loop.spawn(requeueOnSendRoom());
}

core::async::Task<void> NativeSession::requeueOnSendRoom()
{
    // A scope guard, for the reason streamSnapshots has one: run()'s teardown waits on it.
    auto const idle = core::Finally { [this] { _roomWaiter = RoomWaiter::Idle; } };
    if (!co_await awaitSendRoom())
        co_return;
    for (auto const session: std::exchange(_recordsAwaitingRoom, {}))
        queueIncrement(SessionId { session });
}

core::async::Task<void> NativeSession::streamSnapshots()
{
    // A scope guard, not an assignment at the end: run()'s teardown polls this for the streamer
    // having let go of `this`, and a cancellation unwinding past a trailing assignment would
    // leave it set — deadlocking the very poll that keeps `this` alive.
    auto const finished = core::Finally { [this] { _streamingSession.reset(); } };

    while (!_snapshotQueue.empty() && !_closed)
    {
        auto const session = SessionId { _snapshotQueue.front() };
        _snapshotQueue.pop_front();
        _streamingSession = session.value;

        // Wait BEFORE building, not after: a snapshot captured and then parked behind a backlog
        // is a snapshot of the past, and this is where a peer that fell behind gets to catch up.
        if (!co_await awaitSendRoom())
            co_return;

        auto built = buildPush(session, SnapshotMode::Forced);
        if (!built)
            continue; // the host stopped hosting it while this waited its turn
        // Always engaged on this path — a snapshot carries its session's whole state — but read as
        // the optional it is, so the invariant lives in ONE place rather than in every consumer.
        if (built->state)
            send(0, proto::DecodedPdu { *std::move(built->state) }, session.value);
        // A snapshot that stated only the newest records that fit had buildPush queue the older
        // remainder as increments, which the debounce holds back until this run is off the wire.
        co_await sendSnapshotPieces(std::move(built->delta), session);

        // The session is let go once the backlog is back under a piece, not the moment its last
        // piece joined it. Letting go releases the increment flushSoon held back for the whole run —
        // the one increment that is not small by construction — and it should meet at most one
        // piece of backlog, not two.
        if (!co_await awaitSendRoom())
            co_return;
    }
}

core::async::Task<void> NativeSession::sendSnapshotPieces(proto::Delta delta, SessionId session)
{
    // The rows are the only part of a snapshot that grows without bound; everything else is one
    // session's state. So the rows are what gets split, and the rest rides along.
    //
    // Non-const: the pieces move their rows out of it. Safe because each row is visited exactly
    // once, and because the partition below is computed before anything is moved.
    auto lines = std::exchange(delta.lines, {});

    // The two side tables that are neither per-row nor scalar, held aside for the ONE piece that
    // carries each. Copying them onto every piece and clearing them again is what it would cost
    // to leave them in `delta`: `hyperlinks` is every URI referenced anywhere in the grid, so a
    // 25-piece run would allocate all of them 25 times and throw 24 copies away.
    //
    // Hyperlinks go with the first piece: the mirror merges them into a map that outlives the
    // run, and a row cannot resolve a link id it has not seen. Status lines go with the last,
    // alongside the rest of the state the run must land on.
    auto hyperlinks = std::exchange(delta.hyperlinks, {});
    auto statusLines = std::exchange(delta.statusLines, {});
    auto const statusLinesChanged = std::exchange(delta.statusLinesChanged, uint8_t { 0 });

    // Index the image cells by the row they sit on. The mirror clears a row's image cells when it
    // takes the row and refills them from the same PDU's side table, so an entry that travelled in
    // a different piece than its row would be dropped on arrival.
    // ImageCellEntry is trivially copyable, so these are copies however they are spelled; only the
    // grouping matters.
    auto imagesByRow = std::unordered_map<int64_t, std::vector<proto::ImageCellEntry>> {};
    for (auto const& entry: std::exchange(delta.imageCells, {}))
        imagesByRow[entry.stableId].push_back(entry);

    for (auto const& part: proto::partitionSnapshotRows(lines, _snapshotPieceBytes))
    {
        // The scalar state rides on every piece. Repeating it costs a few dozen bytes and buys
        // the receiver statelessness: each piece stands on its own, and applying the same title
        // or cursor position twice is applying it once.
        auto piece = delta;
        piece.snapshotPart = std::to_underlying(part.part);

        // Moved, not copied: a WireLine owns a heap-allocated cell vector, so copying a 4000-row
        // grid is 4000 allocations and megabytes of element-wise copy construction.
        auto const rows = std::span { lines }.subspan(part.begin, part.count);
        piece.lines.assign(std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        for (auto const& line: piece.lines)
            if (auto const images = imagesByRow.find(line.stableId); images != imagesByRow.end())
                piece.imageCells.insert(piece.imageCells.end(), images->second.begin(), images->second.end());

        // Handed over with `exchange` rather than `move`: the piece that takes each is decided by
        // the SAME predicates the receiver uses, and leaving an emptied source behind means a
        // later piece can only ever get an empty table — the correctness does not rest on the
        // condition firing exactly once.
        if (piece.startsSnapshot())
            piece.hyperlinks = std::exchange(hyperlinks, {});
        if (piece.completesSnapshot())
        {
            piece.statusLines = std::exchange(statusLines, {});
            piece.statusLinesChanged = statusLinesChanged;
        }

        // Before EVERY piece, the first included: the SessionState that precedes a snapshot is
        // already in the backlog by now, so "the streamer waited before building" does not leave
        // the first piece the empty backlog it needs. Waiting here is what makes the enqueue
        // below unrefusable, which is the whole point of pacing.
        if (!co_await awaitSendRoom())
            co_return;
        send(0, proto::DecodedPdu { std::move(piece) }, session.value);
    }
}

void NativeSession::handlePdu(proto::DecodedFrame const& frame)
{
    // An unknown PDU from a newer client is answered rather than dropped, and an Unsupported it sent is
    // taken note of -- neither is dispatched further.
    auto const handling = _unsupported.handle(frame);
    switch (handling.verdict)
    {
        case proto::UnsupportedVerdict::NotConcerned: break;
        case proto::UnsupportedVerdict::Answer:
            send(frame.serial, proto::DecodedPdu { handling.pdu });
            return;
        case proto::UnsupportedVerdict::Refused:
            connectionLog()("{}: the client does not know PDU {}; not sending it again",
                            _id,
                            proto::nameOfIdent(handling.pdu.ident));
            return;
        case proto::UnsupportedVerdict::AlreadyRefused: return;
    }
    if (serveDaemonRequest(frame) == DaemonRequest::Served)
        return;
    switch (_role)
    {
        case proto::ConnectionRole::Attach: break;
        case proto::ConnectionRole::Control:
            // Not a client: nothing that reads or changes the sessions is a Control connection's to send.
            refuse(frame);
            return;
    }
    if (auto const* input = std::get_if<proto::Input>(&frame.pdu))
    {
        if (_host.terminal(SessionId { input->session }) == nullptr)
        {
            errorLog()("{}: Input for unknown session {} ({} bytes dropped)",
                       _id,
                       input->session,
                       input->data.size());
            return;
        }
        // Through the host, whose session pump writes it: this is the loop thread, which must not park on a
        // PTY write while the application leaves its input unread.
        _host.writeInput(
            SessionId { input->session },
            std::string_view { reinterpret_cast<char const*>(input->data.data()), input->data.size() });
        pauseReadingIfBackedUp(SessionId { input->session });
        return;
    }
    if (auto const* resize = std::get_if<proto::ResizeRequest>(&frame.pdu))
    {
        auto const size = sanePageSize(resize->columns, resize->lines);
        if (!size)
        {
            errorLog()("{}: rejecting client area {}x{} (out of range)", _id, resize->columns, resize->lines);
            return;
        }
        // The resync is not issued here. A client area re-projects every pane of every window, and
        // the resulting grids belong to every attached client — not just this one. SessionHost
        // announces each moved grid through sessionResized(), which lands on all of them.
        std::ignore = _host.applyClientSize(this, *size);
        return;
    }
    if (auto const* resize = std::get_if<proto::ResizePane>(&frame.pdu))
    {
        auto const size = sanePageSize(resize->columns, resize->lines);
        if (!size)
        {
            errorLog()("{}: rejecting pane {} grid {}x{} (out of range)",
                       _id,
                       resize->session,
                       resize->columns,
                       resize->lines);
            return;
        }
        // As above, the resync rides sessionResized(). A client re-asserts every pane size after a
        // client-area change, so most of these arrive already satisfied by the reprojection that
        // change performed — and SizeChange::Unchanged announces nothing.
        std::ignore = _host.applyPaneSize(SessionId { resize->session }, *size);
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
    if (auto const* createTab = std::get_if<proto::CreateTab>(&frame.pdu))
    {
        // The window is named by a session it hosts (0 = the host's own). Ignoring it put a "+"
        // clicked in a second daemon window into the first one.
        auto const beside =
            createTab->session != 0 ? std::optional { SessionId { createTab->session } } : std::nullopt;
        std::ignore = _host.createTab(spawnRequest(), beside);
        return;
    }
    if (std::holds_alternative<proto::NewWindow>(frame.pdu))
    {
        std::ignore = _host.createWindow(spawnRequest());
        return;
    }
    if (auto const* split = std::get_if<proto::SplitPane>(&frame.pdu))
    {
        // The decoder guarantees the orientation is Horizontal or Vertical (any
        // other wire byte is MalformedPdu at the protocol boundary), so the cast
        // below is provably safe. Split the pane hosting the target session (in
        // whichever window hosts it): make that pane active, then split
        // (splitActivePane acts on the tab's active pane).
        auto& model = _host.model();
        if (auto const [window, tab, leaf] = model.findSessionLeaf(vtworkspace::SessionId { split->session });
            leaf != nullptr)
        {
            model.setActivePane(tab->id(), leaf->id());
            _host.splitActivePane(tab->id(),
                                  static_cast<vtworkspace::SplitState>(split->orientation),
                                  proto::fromWireRatio(split->ratio),
                                  spawnRequest());
        }
        return;
    }
    if (auto const* resize = std::get_if<proto::ResizeSplit>(&frame.pdu))
    {
        auto& model = _host.model();
        auto const [firstWindow, tab, firstLeaf] =
            model.findSessionLeaf(vtworkspace::SessionId { resize->firstSession });
        auto const [secondWindow, secondTab, secondLeaf] =
            model.findSessionLeaf(vtworkspace::SessionId { resize->secondSession });
        // One nullptr answers all three ways this can fail: an unknown session, the same pane twice,
        // and two panes of different tabs (a divider only exists between two panes of one tree). A
        // stale name — a pane closed since the drag — is dropped, not guessed at; the next drag
        // re-asserts the ratio.
        auto* node = vtworkspace::Pane::lowestCommonAncestor(firstLeaf, secondLeaf);
        if (node == nullptr || tab == nullptr)
        {
            errorLog()("{}: ignoring ResizeSplit for sessions {}/{} (unknown, or not one divider)",
                       _id,
                       resize->firstSession,
                       resize->secondSession);
            return;
        }
        // A ratio that survives the wire round trip unchanged is not a change. Writing it anyway
        // would cost every attached client a full re-projection and a fresh LayoutState, which is
        // what a client re-asserting its layout on attach does.
        if (proto::toWireRatio(node->ratio()) != resize->ratio)
            model.setPaneRatio(tab->id(), node->id(), proto::fromWireRatio(resize->ratio));
        return;
    }
    if (auto const* changed = std::get_if<proto::SessionSettingsChanged>(&frame.pdu))
    {
        adoptClientSessionSettings(changed->sessionSettings);
        return;
    }
    if (auto const* request = std::get_if<proto::PerformOperation>(&frame.pdu))
    {
        // The mirror follows through the screen update the operation raises; there is no reply to send.
        if (auto const performed = _host.performOperation(*request); !performed)
            errorLog()("{}: ignoring {}: {}", _id, proto::describe(frame.pdu), describe(performed.error()));
        // A write queues for the session's pump, as input does, and backs up with it.
        pauseReadingIfBackedUp(SessionId { request->session });
        return;
    }
    if (auto const* close = std::get_if<proto::ClosePane>(&frame.pdu))
    {
        _host.handleSessionExit(vtworkspace::SessionId { close->session });
        return;
    }
    if (auto const* reply = std::get_if<proto::PermissionReply>(&frame.pdu))
    {
        // Read, never cast: a byte no answer has records nothing, and leaves the question open.
        if (auto const answer = grantStateOf(reply->grantState))
            _host.answerPermission(reply->hostId, reply->digest, *answer);
        else
            errorLog()("{}: ignoring a permission reply with answer {}", _id, reply->grantState);
        return;
    }
    // Unknown/unexpected PDUs are ignored: forward compatibility with a newer peer. Recorded
    // all the same — "the daemon ignored what I sent" is otherwise indistinguishable from "the
    // daemon never received it".
    errorLog()("{}: ignoring unexpected {} (serial {})", _id, proto::describe(frame.pdu), frame.serial);
}

NativeSession::DaemonRequest NativeSession::serveDaemonRequest(proto::DecodedFrame const& frame)
{
    auto& daemon = _services.daemon.get();
    if (std::holds_alternative<proto::DaemonStatusRequest>(frame.pdu))
    {
        if (auto const status = daemon.status())
            send(frame.serial, proto::DecodedPdu { *status });
        else
            refuse(frame);
        return DaemonRequest::Served;
    }
    if (auto const* stop = std::get_if<proto::StopDaemon>(&frame.pdu))
    {
        // Read, never cast: a reason no enumerator has stops nothing.
        auto const reason = stopReasonFrom(stop->reason);
        if (!reason)
        {
            errorLog()("{}: refusing a StopDaemon with reason {}", _id, stop->reason);
            refuse(frame);
        }
        else if (auto const stopped = daemon.requestStop(*reason); !stopped)
            refuse(frame);
        else
            connectionLog()("{}: the client asked the daemon to stop (reason {})", _id, stop->reason);
        return DaemonRequest::Served;
    }
    return DaemonRequest::None;
}

void NativeSession::refuse(proto::DecodedFrame const& frame)
{
    auto const type = proto::typeOf(frame.pdu);
    connectionLog()("{}: answering {} (serial {}) Unsupported", _id, proto::toString(type), frame.serial);
    send(frame.serial,
         proto::DecodedPdu {
             proto::Unsupported { .refusedSerial = frame.serial, .ident = std::to_underlying(type) } });
}

void NativeSession::adoptClientSessionSettings(std::optional<proto::WireSessionSettings> const& stated)
{
    if (!stated)
    {
        _clientSessionSettings.reset();
        connectionLog()("{}: sessions this client creates will emulate as the host's", _id);
        return;
    }
    _clientSessionSettings = fromWireSessionSettings(*stated, _host.settings());
    connectionLog()(
        "{}: sessions this client creates will emulate as {}", _id, _clientSessionSettings->terminalId);
}

bool NativeSession::completeHandshake(proto::DecodedFrame const& frame)
{
    auto const& policy = _services.handshake;
    auto const* hello = std::get_if<proto::ClientHello>(&frame.pdu);
    if (hello == nullptr)
    {
        errorLog()("{}: handshake rejected: expected ClientHello, got {}",
                   _id,
                   proto::toString(proto::typeOf(frame.pdu)));
        // Whatever sent it, the version alone is what every pre-negotiation client reads.
        send(frame.serial,
             proto::DecodedPdu { proto::ServerHello { .layout = proto::HelloLayout::VersionAlone } });
        return false;
    }
    auto const agreed = proto::negotiate(policy.versions, hello->versions);
    if (!agreed)
    {
        errorLog()(
            "{}: handshake rejected: {}", _id, proto::describe(agreed.error(), proto::PeerRole::Client));
        send(frame.serial, proto::DecodedPdu { refusalFor(*hello, policy.versions) });
        return false;
    }
    // Preshared-token auth (an empty token accepts any — the AF_UNIX default, where the socket's
    // permissions are the gate). A mismatch is answered with the very bytes a version refusal of this
    // hello is, and dropped, revealing nothing.
    if (!policy.expectedToken.empty() && !core::net::constantTimeEquals(hello->token, policy.expectedToken))
    {
        // Server-side only, and WITHOUT either token: the wire answer deliberately makes
        // this indistinguishable from a version mismatch, so the log must not undo on disk what
        // the protocol is careful not to reveal on the network.
        errorLog()("{}: handshake rejected: preshared token mismatch", _id);
        send(frame.serial, proto::DecodedPdu { refusalFor(*hello, policy.versions) });
        return false;
    }
    // Only an accepted client learns which machine the daemon runs on: what every record's locality was
    // judged against, so the client can tell whose "local" that is before it mirrors a single one. By an
    // id derived for this application, never the machine's own, which machine-id(5) keeps confidential.
    auto stated = appSpecificIdentity(_host.identity().view());
    send(frame.serial,
         proto::DecodedPdu { proto::ServerHello {
             .agreedVersion = *agreed,
             .versions = policy.versions,
             .capabilities = policy.capabilities,
             .machineId = std::move(stated.machineId),
             .hostname = std::move(stated.hostname),
             .releaseVersion = std::string { _services.daemon.get().releaseVersion() } } });
    _negotiated = proto::Negotiated { .version = *agreed,
                                      .peerCapabilities = hello->capabilities,
                                      .shared = proto::intersect(policy.capabilities, hello->capabilities) };
    _role = hello->role;
    switch (_role)
    {
        case proto::ConnectionRole::Attach:
            connectionLog()("{}: handshake complete (protocol v{})", _id, *agreed);
            attachClient(*hello);
            break;
        case proto::ConnectionRole::Control:
            // A probe or a command to the daemon itself: shown nothing, spawning nothing, counted as no one.
            connectionLog()("{}: handshake complete (protocol v{}, control connection)", _id, *agreed);
            break;
    }
    return true;
}

void NativeSession::attachClient(proto::ClientHello const& hello)
{
    // Counted, and subscribed, first: the spawn below fans out to the subscribers, as it did when every
    // connection subscribed before its hello.
    _clientTicket = _services.daemon.get().admitClient();
    _streamSubscription.emplace(_host, *this, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream);
    _layoutSubscription.emplace(_host, _layoutObserver, &SessionHost::subscribe, &SessionHost::unsubscribe);

    // Adopted BEFORE the spawn below: that session exists because THIS client attached, so it
    // inherits this client's profile like any tab the client goes on to open.
    adoptClientSessionSettings(hello.sessionSettings);

    // Attaching to an empty daemon spawns the first session.
    if (_host.model().window(_host.windowId())->tabCount() == 0)
    {
        connectionLog()("{}: daemon has no tabs; spawning the first session", _id);
        std::ignore = _host.createTab(spawnRequest());
    }

    // The window/tab/pane layout first, so the client builds its tabs and split
    // trees before the per-session content streams into them.
    pushLayout();

    // The attach snapshot: every hosted session of every window, full state. QUEUED, not pushed:
    // this runs inside the PDU pump's handler, which cannot suspend, so pushing here would put
    // every pane's whole grid into the send queue before the drain coroutine had run once.
    _host.model().forEachTab([this](vtworkspace::Window&, vtworkspace::Tab& tab) {
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                requestSnapshot(pane.session());
        });
    });

    // The questions still open: asked while no client was attached, or left unanswered by one that was --
    // a dismissed prompt sends nothing. A status-line extension waits for an answer, its command calls
    // failing as pending meanwhile. A client is sent each once, as it opens or as the client attaches --
    // one that cannot answer is sent none, and they wait for another, as with no client attached.
    if (asksPermissions())
        for (auto const& request: _host.pendingPermissionRequests())
            send(0, proto::DecodedPdu { request });
}

void NativeSession::reportPumpOutcome(PumpResult const& outcome) const
{
    if (auto const reason = describe(outcome))
    {
        if (isFailure(outcome.stop))
            errorLog()("{}: {}", _id, *reason);
        else
            connectionLog()("{}: {}", _id, *reason);
    }
}

core::async::Task<void> NativeSession::run()
{
    // Nothing is valid before a version-matching ClientHello; afterwards the
    // pump serves request PDUs until the peer disconnects.
    auto const outcome = co_await pumpPdus(
        _connection.get(),
        [this](proto::DecodedFrame const& frame) {
            // Traced HERE rather than in handlePdu: the handshake frame never reaches handlePdu, and
            // a trace that cannot show the ClientHello is useless for exactly the failures — version
            // and token mismatches — one most wants a trace for.
            if (protocolTraceLog)
                protocolTraceLog()(
                    "{} {}",
                    _id,
                    proto::traceLine(proto::Direction::Recv, frame.serial, frame.pdu, frame.consumed));
            if (!_negotiated)
                return completeHandshake(frame);
            handlePdu(frame);
            return true;
        },
        {},
        [this] { return awaitInputRoom(); });
    reportPumpOutcome(outcome);

    _closed = true;
    // Lifetime constraint: serveNativeClient destroys this session the moment
    // run() returns (the unique_ptr goes out of scope immediately after the
    // co_await). A debounce flush spawned before the disconnect may still be
    // parked in its 20ms delay with `this` captured in its coroutine frame.
    // pollUntil drains that pending flush — *this must still be alive for the
    // entire poll, so this poll MUST remain the last thing run() does before
    // returning. Any refactoring that moves logic after this point opens a
    // use-after-free window on _flushScheduled.
    //
    // The snapshot streamer joins the same barrier for the same reason, and needs it more: it
    // parks on the write queue's watermark, which is a far longer park than the flush debounce.
    // `_closed` was set just above, which is what unparks it — waitUntilBacklogBelow returns on
    // a closed queue, and the streamer's loops all test _closed — so this poll is bounded. The waiter
    // for room for command-block records parks on the same watermark, and joins for the same reason.
    co_await core::net::pollUntil(
        &_loop, [this] { return !_flushScheduled && !_streamingSession && _roomWaiter == RoomWaiter::Idle; });
    co_await closeGracefully(&_loop, &_writer, _connection.get());
}

namespace
{
    /// One native client's whole lifetime, as a free coroutine (a capturing
    /// lambda coroutine would dangle its closure; pointers live in the frame).
    core::async::Task<void> serveNativeClient(core::net::EventLoop* loop,
                                              SessionHost* host,
                                              ConnectionId id,
                                              std::unique_ptr<core::net::ISocket> connection,
                                              NativeSessionServices services)
    {
        // The session subscribes itself to the host, once its hello says it is a client.
        auto session = std::make_unique<NativeSession>(*loop,
                                                       *host,
                                                       std::move(id),
                                                       std::move(connection),
                                                       NativeSession::DefaultWriteQueueBytes,
                                                       std::move(services));
        co_await session->run();
    }
} // namespace

ConnectionHandler makeNativeHandler(core::net::EventLoop& loop,
                                    SessionHost& host,
                                    NativeSessionServices services)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host, services = std::move(services)](ConnectionId id,
                                                          std::unique_ptr<core::net::ISocket> connection) {
        return serveNativeClient(&loop, &host, std::move(id), std::move(connection), services);
    };
}

} // namespace vthost
