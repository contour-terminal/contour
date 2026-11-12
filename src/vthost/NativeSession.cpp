// SPDX-License-Identifier: Apache-2.0
#include <vthost/NativeSession.hpp>

#include <vtbackend/core/Image.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/vt/ClipboardSelection.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/net/ITlsContext.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Dispatch.hpp>
#include <vtcontrol/Json.hpp>
#include <vthost/CellPixelBounds.hpp>
#include <vthost/CommandBlockWire.hpp>
#include <vthost/ContextWire.hpp>
#include <vthost/CursorStyle.hpp>
#include <vthost/DaemonControlWire.hpp>
#include <vthost/GracefulClose.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/InputEchoWire.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/Logging.hpp>
#include <vthost/MirroredModes.hpp>
#include <vthost/MouseWire.hpp>
#include <vthost/NodeMove.hpp>
#include <vthost/NotifyWire.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/SoftFontWire.hpp>
#include <vthost/TabEdits.hpp>
#include <vthost/TabMerge.hpp>
#include <vthost/TimeWire.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vthost/control/ProgramStatusConvert.hpp>
#include <vthost/handover/HandoverRestore.hpp>
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

    /// Serializes the host workspace's whole tab/pane layout for the LayoutState PDU.
    [[nodiscard]] proto::LayoutState serializeLayout(vtworkspace::Workspace& workspace)
    {
        auto layout = proto::LayoutState {};
        layout.workspace = workspace.id().value;
        layout.activeTab = static_cast<uint32_t>(std::max(0, workspace.activeTabIndex()));
        for (auto const tabIndex: std::views::iota(0, workspace.tabCount()))
        {
            auto* tab = workspace.tabAt(tabIndex);
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

    /// Collects the soft-glyph cell side-table entries of one row.
    void appendSoftGlyphCells(std::vector<proto::SoftGlyphCellEntry>& out,
                              int64_t stableId,
                              vtbackend::Line const& line)
    {
        auto const& glyphs = line.storage().softGlyphs;
        if (!glyphs)
            return;
        for (auto const& [column, ref]: *glyphs)
            out.push_back(proto::SoftGlyphCellEntry {
                .stableId = stableId, .column = column, .font = ref.font, .position = ref.position });
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

    /// @param accepted The tokens an endpoint accepts; empty for one that takes any hello.
    /// @param token The token a hello carries.
    /// @return What @p token is granted: everything when @p accepted is empty, the grants of the first
    ///         accepted token it equals, or nullopt for none. Every entry is compared, in constant time, so
    ///         the time taken says nothing about which one matched.
    [[nodiscard]] std::optional<vtcontrol::ControlGrants> grantsOf(std::span<AcceptedToken const> accepted,
                                                                   std::string_view token)
    {
        if (accepted.empty())
            return vtcontrol::ControlGrants::all();
        auto matched = std::optional<vtcontrol::ControlGrants> {};
        for (auto const& entry: accepted)
            if (core::net::constantTimeEquals(token, entry.token) && !matched)
                matched = entry.grants;
        return matched;
    }

    /// Entries of a snapshot's per-cell side table, grouped by the stable id of the row they sit on.
    template <typename Entry>
    using EntriesByRow = std::unordered_map<int64_t, std::vector<Entry>>;

    /// @return @p entries grouped by their row's stable id.
    template <typename Entry>
    [[nodiscard]] EntriesByRow<Entry> groupByRow(std::vector<Entry> const& entries)
    {
        auto grouped = EntriesByRow<Entry> {};
        for (auto const& entry: entries)
            grouped[entry.stableId].push_back(entry);
        return grouped;
    }

    /// Appends to @p out the entries of every row in @p lines, in the order of the rows.
    template <typename Entry>
    void appendRowEntries(std::vector<Entry>& out,
                          EntriesByRow<Entry> const& byRow,
                          std::span<proto::WireLine const> lines)
    {
        for (auto const& line: lines)
            if (auto const found = byRow.find(line.stableId); found != byRow.end())
                out.insert(out.end(), found->second.begin(), found->second.end());
    }

    /// @return The session of every pane @p workspace shows, tab by tab.
    [[nodiscard]] std::vector<SessionId> sessionsShownBy(vtworkspace::Workspace const& workspace)
    {
        auto sessions = std::vector<SessionId> {};
        for (auto const tabIndex: std::views::iota(0, workspace.tabCount()))
            if (auto const* tab = workspace.tabAt(tabIndex))
                std::ranges::copy(tab->leafSessions(), std::back_inserter(sessions));
        return sessions;
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
    _services(std::move(services)),
    _maxWriteQueueBytes(maxWriteQueueBytes)
{
    // Every model change (the host fans them here once subscribed) re-pushes the whole layout of the
    // workspace it is in — infrequent, so a full resend beats a granular diff.
    _layoutObserver.onWorkspaceChange = [this](vtworkspace::WorkspaceId workspace) {
        layoutChanged(workspace);
    };
    _layoutObserver.onWorkspaceOrderChange = [this] {
        syncWorkspacePositions();
    };
    _layoutObserver.onTabChange = [this](vtworkspace::TabId tab) {
        layoutChanged(_host.model().workspaceOfTab(tab));
    };
}

NativeSession::~NativeSession()
{
    _upgradeLink->session = nullptr;
    _host.noteClientDetached(this);
    // Beside the count it mirrors, so that the event and connectedClients() never disagree; only for a
    // connection that was told attached.
    if (_phase == ConnectionPhase::Viewing)
        _services.control.get().clientDetached(std::format("{}", _id));
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
    if (_phase != ConnectionPhase::Viewing || _closed)
        return;
    if (mirrorsSession(session))
        queueIncrement(session);
    else
        noteActivity(session);
}

void NativeSession::queueIncrement(SessionId session)
{
    // Every producer of an increment comes through here -- a screen update, and the command-block
    // records followUpRecords and requeueOnSendRoom owe -- so each is timed by the same decision.
    _pendingSessions.insert(session.value);
    auto const now = _loop.clock().now();
    scheduleFlush(flushDeadline(_services.flush, flushReasonFor(session, now), now));
}

FlushReason NativeSession::flushReasonFor(SessionId session, core::platform::SteadyTimePoint now)
{
    // While the session's snapshot is queued or streaming its increment is held back anyway, and a follow
    // not pushed yet is owed a snapshot: hurrying either would only spin, every screen update arming one
    // more immediate flusher that holds the increment back again. The echo credit stays unspent for after.
    auto const follow = _followed.find(session.value);
    if (follow == _followed.end() || snapshotPending(session))
        return FlushReason::Coalesce;
    auto const lastInput = _host.lastInputAt(session);
    auto const reason = decideFlush(_services.flush,
                                    FlushInputs { .now = now,
                                                  .lastInput = lastInput,
                                                  .answeredInput = follow->second.answeredInput,
                                                  .increments = follow->second.increments });
    // Spent at once, not when the flush runs: a second update before it must not earn another.
    if (reason == FlushReason::Echo)
        follow->second.answeredInput = lastInput;
    return reason;
}

void NativeSession::scheduleFlush(core::platform::SteadyTimePoint deadline)
{
    if (_closed || (_flushDeadline && *_flushDeadline <= deadline))
        return;
    _flushDeadline = deadline;
    _loop.spawn(flushAt(claimFlush(), deadline));
}

void NativeSession::sessionClosed(SessionId session)
{
    forgetSession(session);
}

void NativeSession::forgetSession(SessionId session)
{
    _followed.erase(session.value);
    _pendingSessions.erase(session.value);
    // The snapshot bookkeeping forgets it too. The streamer would survive without this (buildPush
    // answers NoSuchSession and it moves on), but every per-session set this connection keeps must
    // be dropped in ONE place, or the next one added is dropped in none.
    std::erase(_snapshotQueue, session.value);
    _recordsAwaitingRoom.erase(session.value);
    _programStatusPending.erase(session.value);
    _pendingSummaries.erase(session.value);
    _activityMarkedAt.erase(session.value);
    _inputEchoPending.erase(session.value);
    _inputEchoSent.erase(session.value);
    _softFontsSent.erase(session.value);
}

void NativeSession::sessionResized(SessionId session)
{
    // A resize destroys the grid's row identity (a rebuild bumps the generation), so the mirror
    // cannot be brought forward by a delta — it needs the whole grid. Queued rather than pushed
    // inline, and not through the 20ms debounce either: one client's resize re-projects every
    // pane of every workspace, so this arrives once PER PANE within a single callback. Pushing each
    // one inline is the same burst the attach walk used to be, with the same outcome.
    if (_phase != ConnectionPhase::Viewing || _closed || !_followed.contains(session.value)
        || !mirrorsSession(session))
        return;
    requestSnapshot(session);
}

bool NativeSession::summarizesRaise(SessionId session) const
{
    return _phase == ConnectionPhase::Viewing && !_closed && summarizesPanes() && !mirrorsSession(session);
}

void NativeSession::sessionBell(SessionId session)
{
    // Every window is told, whichever workspace it shows: one mirroring the pane by the pane's own event, any
    // other by its summary. The pane's marks and the raise role say what each does with the bell.
    if (summarizesRaise(session))
        queueSummary(session, [](proto::PaneSummary& summary) { summary.bell = proto::PaneBell::Rang; });
    else
        emitSessionEvent(proto::SessionBell { .session = session.value });
}

void NativeSession::sessionNotify(SessionId session,
                                  std::string const& title,
                                  std::string const& body,
                                  std::optional<vtbackend::DisplayOccasion> occasion)
{
    // As a bell is: summarized for a window that does not mirror the pane, each notification kept.
    if (summarizesRaise(session))
        queueSummary(session, [&](proto::PaneSummary& summary) {
            summary.notifications.push_back(proto::PaneNotification {
                .title = title, .body = body, .occasion = wireOccasionOf(occasion) });
        });
    else
        emitSessionEvent(proto::SessionNotify {
            .session = session.value, .title = title, .body = body, .occasion = wireOccasionOf(occasion) });
}

void NativeSession::sessionCopyToClipboard(SessionId session,
                                           std::string const& data,
                                           vtbackend::ClipboardTargets targets)
{
    // One window lands a write: the one focused last among those showing the pane's workspace. A client that
    // does not follow the daemon takes no part in that choice, and lands the writes of the panes it mirrors.
    if (!_negotiated)
        return;
    if (summarizesPanes() ? _host.clipboardTargetOf(session) != std::optional { _id }
                          : !mirrorsSession(session))
        return;
    auto const forwarded = forwardedClipboardTargets(targets, _negotiated->shared);
    if (!forwarded)
        return;
    // The targets travel as OSC 52 spells them, which the client reads back with the parser that read the
    // session's own sequence.
    emitSessionEvent(
        proto::SessionClipboard { .session = session.value,
                                  .selection = vtbackend::clipboard_selection::format(*forwarded),
                                  .data = data });
}

std::optional<vtbackend::ClipboardTargets> forwardedClipboardTargets(vtbackend::ClipboardTargets targets,
                                                                     proto::CapabilitySet const& shared)
{
    if (shared.contains(proto::Capability::ClipboardTargets))
        return targets;
    if (!targets.test(vtbackend::ClipboardTarget::Clipboard))
        return std::nullopt;
    return vtbackend::ClipboardTargets { vtbackend::ClipboardTarget::Clipboard };
}

void NativeSession::sessionSignal(SessionId session, SessionSignal const& signal)
{
    if (_phase != ConnectionPhase::Viewing || _closed)
        return;
    if (std::holds_alternative<ProgramStatusChangedSignal>(signal))
    {
        if (!mirrorsProgramStatus())
            return;
        // Debounced like an increment: a loop reporting progress sends one set per coalescing window, not
        // one per report, and the set is read when it goes, so it is the newest.
        _programStatusPending.insert(session.value);
        auto const now = _loop.clock().now();
        scheduleFlush(flushDeadline(_services.flush, FlushReason::Coalesce, now));
        return;
    }
    // What a pane of another workspace did is summarized; a mirrored pane says it in its own stream.
    if (!summarizesPanes() || mirrorsSession(session))
        return;
    if (auto const* title = std::get_if<TitleChangedSignal>(&signal))
        queueSummary(session, [title](proto::PaneSummary& summary) { summary.title = title->title; });
    else if (std::holds_alternative<WorkingDirectoryChangedSignal>(signal))
        queueSummary(session, [this, session](proto::PaneSummary& summary) {
            if (auto const directory = _host.workingDirectoryOf(session))
                summary.cwd = directory->string();
        });
    else if (auto const* finished = std::get_if<CommandBlockFinishedSignal>(&signal))
        queueSummary(session, [finished](proto::PaneSummary& summary) {
            auto const& block = finished->summary;
            summary.finished = proto::FinishedCommand {
                .exitCode = block.exitCode,
                .durationMs = block.duration
                                  ? std::optional { static_cast<uint64_t>(
                                        std::chrono::duration_cast<std::chrono::milliseconds>(*block.duration)
                                            .count()) }
                                  : std::nullopt,
                .commandLine = block.commandLine.display(),
            };
        });
}

void NativeSession::raiseRoleChanged(proto::RaiseRole role)
{
    _raiseRole = role;
    // Before the hello the role waits for it, which states the role once the connection can be told.
    if (_phase != ConnectionPhase::Viewing || _closed || !summarizesPanes())
        return;
    // What was summarized under the old role goes first: the client judges each raise by the role it arrived
    // under, so a summary sent behind the change would be raised by the new holder and by none or two
    // windows. Without the send-room check the coalesced flush makes: what is pending is a few small
    // summaries, at most one per pane, which the role must not overtake whatever the room.
    flushSummaries();
    send(0, proto::DecodedPdu { proto::NotifyRole { .role = role } });
}

bool NativeSession::summarizesPanes() const noexcept
{
    return _following == DaemonFollowing::Yes;
}

namespace
{
    /// @return @p git as WorkspaceMeta carries it.
    [[nodiscard]] proto::WireGitInfo wireGitOf(GitInfo const& git)
    {
        auto const kind = [&] {
            switch (git.kind)
            {
                case vtbackend::GitHeadKind::Branch: return proto::GitHeadKindWire::Branch;
                case vtbackend::GitHeadKind::Detached: return proto::GitHeadKindWire::Detached;
                case vtbackend::GitHeadKind::OtherRef: break;
            }
            return proto::GitHeadKindWire::OtherRef;
        }();
        return proto::WireGitInfo {
            .branch = git.branch, .kind = kind, .ahead = git.ahead, .behind = git.behind
        };
    }
} // namespace

bool NativeSession::sharesWorkspaceMeta() const noexcept
{
    return _negotiated.has_value() && _negotiated->shared.contains(proto::Capability::WorkspaceMeta);
}

std::array<uint8_t, 16> NativeSession::controlIdOf(vtworkspace::WorkspaceId workspace) const
{
    return _host.objects().uuidOf(workspace).value_or(vtcontrol::Uuid {}).bytes;
}

void NativeSession::announceWorkspaceMeta(vtworkspace::Workspace const& workspace)
{
    if (!sharesWorkspaceMeta())
        return;
    // A workspace holding no tab is one the client lists nothing of, and prunes what it knew of it: it is
    // announced again, in full, with the layout that brings its first tab.
    if (workspace.tabCount() == 0)
    {
        _announcedMeta.erase(workspace.id().value);
        return;
    }
    auto const position = static_cast<uint32_t>(positionOf(workspace.id()));
    if (!_announcedMeta.try_emplace(workspace.id().value, position).second)
        return;
    auto const uuid = _host.objects().uuidOf(workspace.id()).value_or(vtcontrol::Uuid {});
    auto const ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = uuid };
    // Every field, so the client's metadata of the workspace starts from what it is now. The git state goes
    // to a following window only (@see sendWorkspaceGit); absent, it is in no repository, or none was looked
    // up.
    auto git = summarizesPanes() ? _host.gitInfoOf(workspace.id()).transform(wireGitOf) : std::nullopt;
    send(0,
         proto::DecodedPdu { proto::WorkspaceMeta { .workspace = workspace.id().value,
                                                    .controlId = uuid.bytes,
                                                    .label = std::string { _host.objects().labelOf(ref) },
                                                    .git = std::move(git),
                                                    .position = position } });
}

int NativeSession::positionOf(vtworkspace::WorkspaceId workspace) const
{
    auto const& model = _host.model();
    auto const indices = std::views::iota(0, model.workspaceCount());
    auto const found = std::ranges::find_if(indices, [&](int i) {
        auto const* candidate = model.workspaceAt(i);
        return candidate != nullptr && candidate->id() == workspace;
    });
    return found != indices.end() ? *found : 0;
}

void NativeSession::syncWorkspacePositions()
{
    if (_phase != ConnectionPhase::Viewing || _closed || !sharesWorkspaceMeta())
        return;
    auto const& model = _host.model();
    for (auto const i: std::views::iota(0, model.workspaceCount()))
    {
        auto const* workspace = model.workspaceAt(i);
        if (workspace == nullptr)
            continue;
        auto const told = _announcedMeta.find(workspace->id().value);
        // One not announced yet is told its position with the rest of its meta.
        if (told == _announcedMeta.end() || std::cmp_equal(told->second, i))
            continue;
        told->second = static_cast<uint32_t>(i);
        send(0,
             proto::DecodedPdu { proto::WorkspaceMeta { .workspace = workspace->id().value,
                                                        .controlId = controlIdOf(workspace->id()),
                                                        .position = static_cast<uint32_t>(i) } });
    }
}

void NativeSession::forgetRemovedWorkspaceMeta()
{
    auto const& model = _host.model();
    std::erase_if(_announcedMeta, [&model](auto const& announced) {
        return model.workspace(vtworkspace::WorkspaceId { announced.first }) == nullptr;
    });
}

void NativeSession::workspaceLabelChanged(vtcontrol::ObjectRef const& ref, std::string_view label)
{
    if (ref.kind != vtcontrol::ObjectKind::Workspace || _phase != ConnectionPhase::Viewing || _closed)
        return;
    auto const handle = _host.objects().find(ref);
    if (!handle)
        return;
    auto const workspace = std::get<vtworkspace::WorkspaceId>(*handle).value;
    // A workspace not announced yet is told its label in full with its layout.
    if (!_announcedMeta.contains(workspace))
        return;
    send(0,
         proto::DecodedPdu { proto::WorkspaceMeta { .workspace = workspace,
                                                    .controlId = ref.uuid.bytes,
                                                    .label = std::string { label },
                                                    .git = std::nullopt } });
}

void NativeSession::workspaceGitChanged(WorkspaceGitChangedSignal const& signal)
{
    sendWorkspaceGit(signal.workspace.value, signal.git.transform(wireGitOf));
}

void NativeSession::sendWorkspaceGit(uint64_t workspace, std::optional<proto::WireGitInfo> const& git)
{
    if (_phase != ConnectionPhase::Viewing || _closed || !summarizesPanes() || !sharesWorkspaceMeta()
        || !_announcedMeta.contains(workspace))
        return;
    send(0,
         proto::DecodedPdu { proto::WorkspaceMeta {
             .workspace = workspace,
             .controlId = controlIdOf(vtworkspace::WorkspaceId { workspace }),
             .label = std::nullopt,
             // None, not absent: absent says nothing changed, None that the repository is gone.
             .git = git.value_or(proto::WireGitInfo {}) } });
}

template <std::invocable<proto::PaneSummary&> Note>
void NativeSession::queueSummary(SessionId session, Note&& note)
{
    auto& summary =
        _pendingSummaries.try_emplace(session.value, proto::PaneSummary { .session = session.value })
            .first->second;
    std::forward<Note>(note)(summary);
    scheduleFlush(flushDeadline(_services.flush, FlushReason::Coalesce, _loop.clock().now()));
}

void NativeSession::noteActivity(SessionId session)
{
    if (!summarizesPanes())
        return;
    auto const now = _loop.clock().now();
    auto const [marked, first] = _activityMarkedAt.try_emplace(session.value, now);
    if (!first)
    {
        if (now - marked->second < _services.flush.activityMarkInterval)
            return;
        marked->second = now;
    }
    queueSummary(session,
                 [](proto::PaneSummary& summary) { summary.activity = proto::OutputActivity::Output; });
}

void NativeSession::flushSummaries()
{
    for (auto& [session, summary]: std::exchange(_pendingSummaries, {}))
    {
        if (!mirrorsSession(SessionId { session }))
        {
            send(0, proto::DecodedPdu { std::move(summary) });
            continue;
        }
        // Mirrored since: the pane's own stream says what it is now, but what it raised while it was not
        // mirrored was told to this connection nowhere else.
        auto raised = proto::PaneSummary { .session = session,
                                           .finished = std::move(summary.finished),
                                           .bell = summary.bell,
                                           .notifications = std::move(summary.notifications) };
        if (proto::carriesRaise(raised))
            send(0, proto::DecodedPdu { std::move(raised) });
    }
}

void NativeSession::announceUnmirroredPanes()
{
    if (_phase != ConnectionPhase::Viewing || _closed || !summarizesPanes())
        return;
    for (auto const session: _host.sessionIds())
    {
        if (mirrorsSession(session))
            continue;
        // Merge into a pending entry: it may already carry a finished command or an activity mark that
        // this announcement must not drop.
        auto& summary =
            _pendingSummaries.try_emplace(session.value, proto::PaneSummary { .session = session.value })
                .first->second;
        if (auto const* terminal = _host.terminal(session))
            summary.title = terminal->resolvedWindowTitle();
        if (auto const directory = _host.workingDirectoryOf(session))
            summary.cwd = directory->string();
        if (mirrorsProgramStatus() && holdsProgramStatus(session))
            _programStatusPending.insert(session.value);
    }
    scheduleFlush(flushDeadline(_services.flush, FlushReason::Coalesce, _loop.clock().now()));
}

bool NativeSession::holdsProgramStatus(SessionId session) const
{
    auto* const terminal = _host.terminal(session);
    if (terminal == nullptr)
        return false;
    auto const guard = std::lock_guard { std::as_const(*terminal) };
    return !terminal->programStatus().records().empty();
}

bool NativeSession::mirrorsProgramStatus() const noexcept
{
    return _negotiated && _negotiated->shared.contains(proto::Capability::ProgramStatus);
}

void NativeSession::pushProgramStatus(SessionId session, ProgramStatusPush when)
{
    auto* const terminal = _host.terminal(session);
    if (terminal == nullptr)
        return;
    auto pdu = proto::ProgramStatus {
        .session = session.value,
        .records = {},
        .timing = _negotiated && _negotiated->shared.contains(proto::Capability::ProgramStatusTiming)
                      ? proto::StatusTimingBlock::Included
                      : proto::StatusTimingBlock::Omitted,
    };
    {
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        for (auto const& record: terminal->programStatus().records())
            pdu.records.push_back(control::toWire(record));
    }
    if (pdu.records.empty() && when == ProgramStatusPush::IfAny)
        return;
    emitSessionEvent(std::move(pdu));
}

bool NativeSession::mirrorsSoftFonts() const noexcept
{
    return _negotiated && _negotiated->shared.contains(proto::Capability::SoftFonts);
}

void NativeSession::pushSoftFonts(SessionId session)
{
    if (!mirrorsSoftFonts())
        return;
    auto* const terminal = _host.terminal(session);
    if (terminal == nullptr)
        return;
    auto pdu = proto::SoftFontState { .session = session.value, .generation = 0, .fonts = {} };
    // One lookup: a session never told about fonts starts at generation 0, which is the store's own.
    auto const sent = _softFontsSent.try_emplace(session.value, uint64_t { 0 }).first;
    {
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        auto const& store = terminal->softFonts();
        pdu.generation = store.generation();
        if (pdu.generation == sent->second)
            return;
        pdu.fonts = softFontsToWire(store);
    }
    sent->second = pdu.generation;
    emitSessionEvent(std::move(pdu));
}

bool NativeSession::mirrorsInputEcho() const noexcept
{
    return _negotiated && _negotiated->shared.contains(proto::Capability::InputEcho);
}

void NativeSession::pushInputEcho(SessionId session)
{
    if (!mirrorsInputEcho())
        return;
    auto* const terminal = _host.terminal(session);
    if (terminal == nullptr)
    {
        _inputEchoSent.erase(session.value); // gone: nothing more to tell of it
        return;
    }
    // The device's own lock guards the read (UnixPty::inputEcho); the terminal's is not taken, so a parser
    // thread holding it never holds up the loop here.
    auto const echo = inputEchoToWire(terminal->device().inputEcho());
    auto const [sent, inserted] =
        _inputEchoSent.try_emplace(session.value, inputEchoToWire(vtpty::InputEcho::Unknown));
    if (sent->second == echo)
        return;
    sent->second = echo;
    emitSessionEvent(proto::SessionInputEcho { .session = session.value, .echo = echo });
}

void NativeSession::checkInputEchoWhenDue()
{
    if (!mirrorsInputEcho() || _closed)
        return;
    auto const now = _loop.clock().now();
    if (now >= _nextInputEchoCheck)
    {
        _nextInputEchoCheck = now + _services.flush.inputEchoCheck;
        for (auto const& [session, follow]: _followed)
            pushInputEcho(SessionId { session });
    }
    // The next check rides a flush of its own; an earlier flush that output arms runs this again, and
    // supersedes the one armed here, which then finds its deadline moved. Past the activity window nothing
    // is armed: an idle connection wakes no timer, and the next output or input opens the window again.
    if (now < _inputEchoActiveUntil)
        scheduleFlush(_nextInputEchoCheck);
}

void NativeSession::noteInputEchoActivity()
{
    _inputEchoActiveUntil = _loop.clock().now() + _services.flush.inputEchoWindow;
}

void NativeSession::sessionPermissionRequested(SessionId session, proto::PermissionRequest const& request)
{
    // A question is asked in its pane: one in a workspace this client does not show waits for a client that
    // does.
    if (!asks(request) || _closed || !mirrorsSession(session))
        return;
    send(0, proto::DecodedPdu { request });
}

void NativeSession::emitSessionEvent(proto::SessionEventPdu const& event)
{
    if (_phase != ConnectionPhase::Viewing || _closed)
        return;
    // Widening the narrow event set into the catalog variant the encoder speaks.
    std::visit([this](auto const& alternative) { send(0, proto::DecodedPdu { alternative }); }, event);
}

void NativeSession::pushLayout()
{
    if (_phase != ConnectionPhase::Viewing || _closed)
        return;
    refreshSelection();
    forgetUnmirroredSessions();
    // One LayoutState per daemon workspace, the selected one and every other: the client lists them all and
    // opens the one it selected, and the sessions of the rest stay unmirrored.
    auto& model = _host.model();
    auto pushed = false;
    for (auto const i: std::views::iota(0, model.workspaceCount()))
        if (auto* workspace = model.workspaceAt(i); workspace != nullptr)
        {
            sendWorkspaceLayout(*workspace);
            pushed = true;
        }
    forgetRemovedWorkspaceMeta();
    syncWorkspacePositions();
    noteModelSessions();
    if (pushed && batchesLayouts())
    {
        openLayoutBatch();
        _layoutBatch = LayoutBatch::Pushed;
    }
}

void NativeSession::sendWorkspaceLayout(vtworkspace::Workspace& workspace)
{
    // Asked before the layout goes out, and streamed after it: the client has the pane by then.
    if (workspace.id().value == _selectedWorkspace)
        requestArrivedSnapshots(workspace);
    sendLayout(serializeLayout(workspace));
    announceWorkspaceMeta(workspace);
}

void NativeSession::sendLayout(proto::LayoutState layout)
{
    if (_negotiated.has_value() && _negotiated->shared.contains(proto::Capability::TabEditAcks))
        layout.servedEdit = _servedEdit;
    send(0, proto::DecodedPdu { std::move(layout) });
}

bool NativeSession::batchesLayouts() const noexcept
{
    return _negotiated.has_value() && _negotiated->shared.contains(proto::Capability::LayoutBatch);
}

void NativeSession::layoutChanged(vtworkspace::WorkspaceId workspace)
{
    if (!batchesLayouts())
    {
        pushLayout();
        return;
    }
    if (_phase != ConnectionPhase::Viewing || _closed)
        return;
    // Every workspace changed is pushed as the batch ends, whichever one this client selects.
    if (workspace != vtworkspace::WorkspaceId {})
        _changedWorkspaces.insert(workspace.value);
    // A change the move being served made is part of what that move's batch completes: the client that asked
    // is told its move landed even when it mirrors none of what moved.
    if (_servingMove != 0)
        _layoutBatchCause = _servingMove;
    openLayoutBatch();
}

void NativeSession::openLayoutBatch()
{
    if (_layoutBatch == LayoutBatch::Closed)
        _loop.spawn(endLayoutBatch(claimLayoutBatch()));
}

bool NativeSession::flushChangedLayouts()
{
    auto& model = _host.model();
    auto const changed = std::exchange(_changedWorkspaces, {});
    // Asked before the selection moves on: a workspace the batch emptied, which the host then removed.
    auto const removed = std::ranges::to<std::vector>(
        changed | std::views::filter([&](uint64_t workspace) {
            return model.workspace(vtworkspace::WorkspaceId { workspace }) == nullptr;
        }));
    refreshSelection();
    forgetUnmirroredSessions();
    auto pushed = false;
    for (auto const i: std::views::iota(0, model.workspaceCount()))
        if (auto* workspace = model.workspaceAt(i);
            workspace != nullptr && changed.contains(workspace->id().value))
        {
            sendWorkspaceLayout(*workspace);
            pushed = true;
        }
    for (auto const workspace: removed)
    {
        auto emptied = proto::LayoutState {};
        emptied.workspace = workspace;
        sendLayout(std::move(emptied));
        pushed = true;
    }
    forgetRemovedWorkspaceMeta();
    syncWorkspacePositions();
    noteModelSessions();
    return pushed;
}

core::async::Task<void> NativeSession::endLayoutBatch(SpawnedFlowClaim claim)
{
    // Given back as the body ends, so a change made after this frame opens a batch of its own.
    auto const held = std::move(claim);
    // Spawned flows run once the frame that spawned them has handed control back to the loop: by now the
    // change has fired its last model event, and each workspace it changed is pushed once, as it left it.
    if (_phase == ConnectionPhase::Viewing && !_closed)
    {
        if (flushChangedLayouts())
            _layoutBatch = LayoutBatch::Pushed;
        if (_layoutBatch == LayoutBatch::Pushed || _layoutBatchCause != 0)
            // The batch states the connection's selection, which a workspace it ended may have moved.
            send(0,
                 proto::DecodedPdu { proto::LayoutBatchEnd { .causeSerial = _layoutBatchCause,
                                                             .selected = _selectedWorkspace } });
    }
    _changedWorkspaces.clear();
    _layoutBatchCause = 0;
    co_return;
}

void NativeSession::refreshSelection()
{
    if (_selectedWorkspace == 0)
        return;
    auto& model = _host.model();
    auto const* selected = model.workspace(vtworkspace::WorkspaceId { _selectedWorkspace });
    // A workspace ends by leaving the model, or by losing its last tab. One that never held a tab (the
    // empty one SelectWorkspace 0 creates before its first) has not ended.
    if (selected != nullptr && selected->tabCount() > 0)
    {
        _selectedTabs = SelectedTabs::Held;
        return;
    }
    if (selected != nullptr && _selectedTabs == SelectedTabs::NeverHeld)
        return;
    // The connection moves on to the one a launch would take; with none left it selects none, which
    // its window takes as the end of what it showed.
    auto const ids = std::ranges::to<std::vector>(
        std::views::iota(0, model.workspaceCount())
        | std::views::transform([&model](int index) { return model.workspaceAt(index); })
        | std::views::filter([](vtworkspace::Workspace const* workspace) {
              return workspace != nullptr && workspace->tabCount() > 0;
          })
        | std::views::transform(
            [](vtworkspace::Workspace const* workspace) { return workspace->id().value; }));
    auto const next = defaultSelection(ids, _host.lastSelectedWorkspace());
    connectionLog()("{}: workspace {} ended; selecting {}", _id, _selectedWorkspace, next.value_or(0));
    if (!next)
    {
        _selectedWorkspace = 0;
        _selectedTabs = SelectedTabs::NeverHeld;
        forgetUnmirroredSessions();
        announceUnmirroredPanes();
        return;
    }
    selectWorkspace(vtworkspace::WorkspaceId { *next });
}

void NativeSession::requestWorkspaceSnapshots(vtworkspace::Workspace& workspace)
{
    for (auto const session: sessionsShownBy(workspace))
        requestSnapshot(session);
}

void NativeSession::requestArrivedSnapshots(vtworkspace::Workspace& workspace)
{
    for (auto const session: sessionsShownBy(workspace))
        if (!_followed.contains(session.value) && _sessionsAtLastLayout.contains(session.value)
            && !snapshotPending(session))
            requestSnapshot(session);
}

void NativeSession::noteModelSessions()
{
    auto& model = _host.model();
    _sessionsAtLastLayout.clear();
    for (auto const i: std::views::iota(0, model.workspaceCount()))
        if (auto const* workspace = model.workspaceAt(i))
            for (auto const session: sessionsShownBy(*workspace))
                _sessionsAtLastLayout.insert(session.value);
}

void NativeSession::forgetUnmirroredSessions()
{
    auto const gone = std::ranges::to<std::vector>(
        _followed | std::views::keys
        | std::views::filter([this](uint64_t session) { return !mirrorsSession(SessionId { session }); }));
    for (auto const session: gone)
        forgetSession(SessionId { session });
}

void NativeSession::selectWorkspace(vtworkspace::WorkspaceId workspace)
{
    auto* found = _host.model().workspace(workspace);
    if (found == nullptr)
    {
        errorLog()("{}: not selecting workspace {}: there is none", _id, workspace.value);
        return;
    }
    _host.noteSelected(*this, workspace.value);
    if (_selectedWorkspace == workspace.value)
        return;
    _selectedWorkspace = workspace.value;
    _selectedTabs = found->tabCount() > 0 ? SelectedTabs::Held : SelectedTabs::NeverHeld;
    // What the old workspace's sessions were sent is not what the new selection continues from.
    forgetUnmirroredSessions();
    announceUnmirroredPanes();
    requestWorkspaceSnapshots(*found);
}

bool NativeSession::mirrorsSession(SessionId session) const
{
    auto const [workspace, tab, leaf] = _host.model().findSessionLeaf(session);
    return workspace != nullptr && workspace->id().value == _selectedWorkspace;
}

core::async::Task<void> NativeSession::flushAt(SpawnedFlowClaim claim,
                                               core::platform::SteadyTimePoint deadline)
{
    // Given back as the body ends, however it ends: a stopped loop unwinds a flush from its sleep.
    auto const held = std::move(claim);
    // A busy PTY produces many screenUpdated signals per frame-worth of output; one Delta per coalescing
    // workspace is what the client can use anyway. A deadline already due resumes without suspending.
    co_await _loop.sleepUntil(deadline);
    // An earlier deadline superseded this one, and its flusher flushed (or will): nothing is left to do.
    if (_flushDeadline != deadline)
        co_return;
    _flushDeadline.reset();
    if (_closed)
        co_return;
    // A session whose whole grid is about to be (or is being) re-described gets no increment: the
    // snapshot says everything this would, and one landing BETWEEN a snapshot's pieces would have
    // the mirror apply and repaint a grid it has only half received. Put it back rather than drop
    // it — rows changed after the snapshot was captured are still news. The reinsert targets the
    // member, which `exchange` just emptied, not the range being walked.
    // The echo first, so a client's pane holds it by the time the output that follows it lands: sudo turns
    // echo off before it prints the prompt.
    if (!_pendingSessions.empty() || !_inputEchoPending.empty())
        noteInputEchoActivity();
    for (auto const session: _pendingSessions)
        pushInputEcho(SessionId { session });
    for (auto const session: std::exchange(_inputEchoPending, {}))
        pushInputEcho(SessionId { session });
    checkInputEchoWhenDue();
    for (auto const session: std::exchange(_pendingSessions, {}))
        if (snapshotPending(SessionId { session }))
            _pendingSessions.insert(session);
        else
            pushDelta(SessionId { session });
    // Not held back by a snapshot: the set replaces whatever the mirror holds whole, whenever it lands,
    // and a mirror not bound yet keeps it on its RemoteScreen. Held back by a backlog, though: a set can
    // be large, a program reporting in a loop must not cost a slow client its connection, and the set is
    // read when it goes, so waiting loses nothing but the reports it overtook.
    if (hasSendRoom())
        for (auto const session: std::exchange(_programStatusPending, {}))
            pushProgramStatus(SessionId { session }, ProgramStatusPush::Always);
    if (hasSendRoom())
        flushSummaries();
    // Something was held back and nothing else will re-arm the flush: a session whose output
    // has stopped raises no further screen update, so without this its last increment would wait
    // for one that never comes.
    if (!_pendingSessions.empty() || !_programStatusPending.empty() || !_pendingSummaries.empty())
        scheduleFlush(_loop.clock().now() + _services.flush.coalesceDelay);
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
    // under a slow reader the records lag, rather than riding every workspace onto a backlog until the
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

    // Live tab name (OSC 30): pulled and diffed like the title -- as a string_view, so an unchanged name
    // allocates nothing -- and stated in SessionState on a snapshot. Empty stands for "none set" on the wire;
    // Terminal::setTabName() folds an empty name into none, so the two are one state on both ends.
    auto const& tabName = terminal.tabName();
    if (auto const current = tabName ? std::string_view { *tabName } : std::string_view {};
        current != follow.lastTabName)
    {
        follow.lastTabName = current;
        if (!snapshot)
        {
            delta.tabNameChanged = 1;
            delta.tabName = follow.lastTabName;
        }
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
        if (terminal.indicatorStatusLineDependsOnHostState())
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
        snap.tabName = terminal.tabName().value_or(std::string {});
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

        auto const sharesSoftFonts = mirrorsSoftFonts();
        auto const collect = [&](vtbackend::LineOffset offset, vtbackend::Line const& line) {
            delta.lines.push_back(toWireLine(grid, offset, line));
            appendImageCells(delta.imageCells, delta.lines.back().stableId, line);
            if (sharesSoftFonts)
                appendSoftGlyphCells(delta.softGlyphCells, delta.lines.back().stableId, line);
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
            // The increment after an attach snapshot is the first since it, and flushes at once; after a
            // resync's it is not (see Follow::historyAfterSnapshot).
            follow.increments =
                std::exchange(follow.historyAfterSnapshot, IncrementHistory::NoneSinceSnapshot);

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
    // Stated outright, empty included, like the rest of a snapshot: the host forgot it when the session's
    // next command started, and a client that attaches or resyncs after that is told none. Loop thread, as
    // the host's table is.
    if (state)
        state->interruptedCommand = _host.interruptedCommandOf(session);
    return BuiltPush { .delta = std::move(delta), .state = std::move(state) };
}

void NativeSession::pushDelta(SessionId session)
{
    // Asked before buildPush, which records the displayed page even when it bails: a follow never pushed
    // has none yet, and its bail is its first snapshot -- a tab opened after attaching -- not a resync.
    // Ahead of the delta, which may name a glyph of a font this connection has not been given.
    pushSoftFonts(session);
    auto const followed = _followed.find(session.value);
    auto const firstPush = followed == _followed.end() || !followed->second.lastDisplayedPage;
    auto built = buildPush(session, SnapshotMode::Delta);
    if (!built)
    {
        switch (built.error())
        {
            case BuildFailure::ResyncRequired:
                // The increment could not describe the batch. The whole grid can, and the whole grid
                // is exactly what must not go out inline. A resync's next increment coalesces; a first
                // snapshot's flushes at once, as an attach's does.
                if (!firstPush)
                    _followed.at(session.value).historyAfterSnapshot = IncrementHistory::SomeSinceSnapshot;
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
    _followed.at(session.value).increments = IncrementHistory::SomeSinceSnapshot;
}

bool NativeSession::snapshotPending(SessionId session) const noexcept
{
    return _streamingSession == session.value || std::ranges::contains(_snapshotQueue, session.value);
}

void NativeSession::requestSnapshot(SessionId session)
{
    if (_phase != ConnectionPhase::Viewing || _closed)
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
    _loop.spawn(streamSnapshots(claimStreamer(session)));
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
    // With room, the next debounce window carries what is owed. Without it, every workspace would only
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
    _loop.spawn(requeueOnSendRoom(claimRoomWaiter()));
}

core::async::Task<void> NativeSession::requeueOnSendRoom(SpawnedFlowClaim claim)
{
    // Given back as the body ends, for the reason streamSnapshots gives its claim back so.
    auto const held = std::move(claim);
    if (!co_await awaitSendRoom())
        co_return;
    for (auto const session: std::exchange(_recordsAwaitingRoom, {}))
        queueIncrement(SessionId { session });
}

core::async::Task<void> NativeSession::streamSnapshots(SpawnedFlowClaim claim)
{
    // Given back as the body ends, not by an assignment at the end: run()'s teardown waits for the
    // streamer having let go of `this`, and a cancellation unwinding past a trailing assignment would
    // leave it held — deadlocking the very wait that keeps `this` alive.
    auto const held = std::move(claim);

    while (!_snapshotQueue.empty() && !_closed)
    {
        auto const session = SessionId { _snapshotQueue.front() };
        _snapshotQueue.pop_front();
        _streamingSession = session.value;

        // Wait BEFORE building, not after: a snapshot captured and then parked behind a backlog
        // is a snapshot of the past, and this is where a peer that fell behind gets to catch up.
        if (!co_await awaitSendRoom())
            co_return;

        // A selection made while this waited left the session's workspace: it is no longer mirrored, and
        // building its snapshot would bring its state back.
        if (!mirrorsSession(session))
            continue;
        auto built = buildPush(session, SnapshotMode::Forced);
        if (!built)
            continue; // the host stopped hosting it while this waited its turn
        // Always engaged on this path — a snapshot carries its session's whole state — but read as
        // the optional it is, so the invariant lives in ONE place rather than in every consumer.
        if (built->state)
            send(0, proto::DecodedPdu { *std::move(built->state) }, session.value);
        // Before the rows, so that the screen holds the set by the time the completing piece replays it.
        if (mirrorsProgramStatus())
            pushProgramStatus(session, ProgramStatusPush::IfAny);
        // And the soft fonts, for the same reason: the rows that follow name their glyphs.
        pushSoftFonts(session);
        // A snapshot states the echo too, for a client that attached while a prompt waits.
        pushInputEcho(session);
        // A snapshot that stated only the newest records that fit had buildPush queue the older
        // remainder as increments, which the debounce holds back until this run is off the wire.
        co_await sendSnapshotPieces(std::move(built->delta), session);
        // Left behind by a selection made while the pieces went out.
        if (!mirrorsSession(session))
            forgetSession(session);

        // The session is let go once the backlog is back under a piece, not the moment its last
        // piece joined it. Letting go releases the increment flushAt held back for the whole run —
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
    // a different piece than its row would be dropped on arrival. The soft-glyph cells travel with
    // their rows for the same reason. Both entry types are trivially copyable, so these are copies
    // however they are spelled; only the grouping matters.
    auto const imagesByRow = groupByRow(std::exchange(delta.imageCells, {}));
    auto const softGlyphsByRow = groupByRow(std::exchange(delta.softGlyphCells, {}));

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
        appendRowEntries(piece.imageCells, imagesByRow, piece.lines);
        appendRowEntries(piece.softGlyphCells, softGlyphsByRow, piece.lines);

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

void NativeSession::handleInput(proto::Input const& input)
{
    if (_host.terminal(SessionId { input.session }) == nullptr)
    {
        errorLog()(
            "{}: Input for unknown session {} ({} bytes dropped)", _id, input.session, input.data.size());
        return;
    }
    // Through the host, whose session pump writes it: this is the loop thread, which must not park on a
    // PTY write while the application leaves its input unread.
    _host.writeInputFrom(
        SessionId { input.session },
        std::string_view { reinterpret_cast<char const*>(input.data.data()), input.data.size() },
        _id,
        input.kind);
    pauseReadingIfBackedUp(SessionId { input.session });
    // The program may answer the keys by turning echo back on without printing a byte: read it again at the
    // next flush, which the session's pump has written the keys by.
    if (mirrorsInputEcho())
    {
        noteInputEchoActivity();
        _inputEchoPending.insert(input.session);
        scheduleFlush(_loop.clock().now() + _services.flush.coalesceDelay);
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
    // A liveness probe, from a connection of either role: echoed under its serial, so the client can tell a
    // quiet daemon from a vanished one. Without the capability it is a PDU this daemon does not serve.
    if (auto const* heartbeat = std::get_if<proto::Heartbeat>(&frame.pdu))
    {
        if (_negotiated->shared.contains(proto::Capability::Heartbeat))
            send(frame.serial, proto::DecodedPdu { *heartbeat });
        else
            refuse(frame);
        return;
    }
    if (serveControlApiRequest(frame) == DaemonRequest::Served)
        return;
    switch (_phase)
    {
        case ConnectionPhase::Viewing: break;
        case ConnectionPhase::AwaitingHello:
        case ConnectionPhase::Controlling:
            // Not a client: nothing that reads or changes the sessions is a Control connection's to send.
            refuse(frame);
            return;
    }
    if (auto const* input = std::get_if<proto::Input>(&frame.pdu))
    {
        handleInput(*input);
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
        // The resync is not issued here. A client area re-projects every pane of every workspace, and
        // the resulting grids belong to every attached client — not just this one. SessionHost
        // announces each moved grid through sessionResized(), which lands on all of them.
        std::ignore =
            _host.applyClientSize(this, *size, statedCellPixelSize(resize->cellPixelSize, "client"));
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
        std::ignore = _host.applyPaneSize(
            this, SessionId { resize->session }, *size, statedCellPixelSize(resize->cellPixelSize, "pane"));
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
        // The workspace is named by a session it hosts (0 = the host's own). Ignoring it put a "+"
        // clicked in a second daemon workspace into the first one.
        auto const beside =
            createTab->session != 0 ? std::optional { SessionId { createTab->session } } : std::nullopt;
        std::ignore = _host.createTab(spawnRequest(createTab->profileName, beside), beside);
        return;
    }
    if (auto const* split = std::get_if<proto::SplitPane>(&frame.pdu))
    {
        // The decoder guarantees the orientation is Horizontal or Vertical (any
        // other wire byte is MalformedPdu at the protocol boundary), so the cast
        // below is provably safe. Split the pane hosting the target session (in
        // whichever workspace hosts it): make that pane active, then split
        // (splitActivePane acts on the tab's active pane).
        auto& model = _host.model();
        if (auto const [workspace, tab, leaf] =
                model.findSessionLeaf(vtworkspace::SessionId { split->session });
            leaf != nullptr)
        {
            model.setActivePane(tab->id(), leaf->id());
            _host.splitActivePane(tab->id(),
                                  static_cast<vtworkspace::SplitState>(split->orientation),
                                  proto::fromWireRatio(split->ratio),
                                  spawnRequest(split->profileName, SessionId { split->session }));
        }
        return;
    }
    if (auto const* resize = std::get_if<proto::ResizeSplit>(&frame.pdu))
    {
        auto& model = _host.model();
        auto const [firstWorkspace, tab, firstLeaf] =
            model.findSessionLeaf(vtworkspace::SessionId { resize->firstSession });
        auto const [secondWorkspace, secondTab, secondLeaf] =
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
    if (serveTabVerb(frame) == TabVerb::Served || serveWorkspaceVerb(frame) == TabVerb::Served)
        return;
    if (auto const* close = std::get_if<proto::ClosePane>(&frame.pdu))
    {
        _host.closeSession(vtworkspace::SessionId { close->session });
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

NativeSession::TabVerb NativeSession::serveTabVerb(proto::DecodedFrame const& frame)
{
    if (auto const* edit = std::get_if<proto::EditTab>(&frame.pdu))
    {
        // A client that did not negotiate it may not edit: it is refused as a PDU this daemon does not know.
        if (!_negotiated->shared.contains(proto::Capability::TabEdits))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Served before it is applied: the push the change fans out, to this client included, has seen it.
        _servedEdit = frame.serial;
        // A change fans out as a model event, and so as a layout push to every client, this one included.
        if (auto const applied = applyTabEdit(_host.model(), *edit); !applied)
            errorLog()("{}: ignoring {}: {}", _id, proto::describe(frame.pdu), describe(applied.error()));
        return TabVerb::Served;
    }
    if (auto const* merge = std::get_if<proto::MergeTabIntoSplit>(&frame.pdu))
    {
        // As for EditTab, a client that did not negotiate it is refused.
        if (!_negotiated->shared.contains(proto::Capability::TabMerge))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Applied to the model, so the layout push that reaches every client, this one included, is the
        // answer: the dragged tab's sessions move into the target tab and none is spawned or closed.
        if (auto const merged = applyTabMerge(_host.model(), *merge); !merged)
        {
            errorLog()("{}: ignoring {}: {}", _id, proto::describe(frame.pdu), describe(merged.error()));
            // Said, so the user whose drop did nothing is told: no layout push follows a refusal.
            send(frame.serial,
                 proto::DecodedPdu {
                     proto::TabMergeRefused { .reason = std::to_underlying(merged.error()) } });
        }
        return TabVerb::Served;
    }
    if (auto const* move = std::get_if<proto::MoveNode>(&frame.pdu))
    {
        // As for EditTab, a client that did not negotiate it is refused.
        if (!_negotiated->shared.contains(proto::Capability::MoveNode))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Applied to the model, so the layout pushes that reach every client, this one included, are the
        // answer: the moved sessions keep running and none is spawned or closed. The batch those pushes
        // open names this request, so the client can tell its move landed.
        _servingMove = frame.serial;
        auto const moved =
            applyMoveNode(_host.model(),
                          *move,
                          MoveWorkspaceHooks { .create = [this] { return _host.createEmptyWorkspace().id(); },
                                               .discard =
                                                   [this](vtworkspace::WorkspaceId workspace) {
                                                       _host.discardWorkspace(workspace);
                                                   } });
        _servingMove = 0;
        if (!moved)
        {
            errorLog()("{}: ignoring {}: {}", _id, proto::describe(frame.pdu), describe(moved.error().error));
            // Said, so the user whose drop did nothing is told: no layout push follows a refusal.
            auto const& failure = moved.error();
            send(frame.serial,
                 proto::DecodedPdu { proto::MoveNodeRefused {
                     .reason = std::to_underlying(failure.error),
                     .modelReason = failure.model ? std::to_underlying(*failure.model) : uint8_t { 0 } } });
        }
        return TabVerb::Served;
    }
    if (auto const* set = std::get_if<proto::SetSplitOrientation>(&frame.pdu))
    {
        // The same feature family as MoveNode, so the same gate.
        if (!_negotiated->shared.contains(proto::Capability::MoveNode))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Read, never cast: a byte no axis names changes nothing. Applied to the model, so the layout push
        // that reaches every client, this one included, is the answer; a stale session is a logged no-op.
        auto const axis = static_cast<vtworkspace::SplitState>(set->state);
        if (auto const applied =
                _host.model().setSplitOrientation(vtworkspace::SessionId { set->anchorSession }, axis);
            !applied)
            errorLog()("{}: ignoring {}", _id, proto::describe(frame.pdu));
        return TabVerb::Served;
    }
    return TabVerb::None;
}

NativeSession::TabVerb NativeSession::serveWorkspaceVerb(proto::DecodedFrame const& frame)
{
    if (auto const* select = std::get_if<proto::SelectWorkspace>(&frame.pdu))
    {
        if (!_negotiated->shared.contains(proto::Capability::DaemonFollow))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // A workspace whose last session ended is gone, though the model may keep it: selected, it would read
        // as one the connection never held a tab of, which it would keep showing empty.
        auto const asked = vtworkspace::WorkspaceId { select->workspace };
        auto const known =
            select->workspace != 0 && _host.model().workspace(asked) != nullptr && !_host.hasEnded(asked);
        if (known)
            selectWorkspace(asked);
        else if (select->workspace == 0)
        {
            // A workspace of its own, which the connection selects as it creates it.
            // Started where the pane that asked is, as a tab made from it would be.
            auto const from = select->directoryFromSession
                                  ? std::optional { vtworkspace::SessionId { *select->directoryFromSession } }
                                  : std::nullopt;
            if (auto const* created = _host.createWorkspace(spawnRequest({}, from)))
                selectWorkspace(created->id());
            else
                errorLog()("{}: the workspace this client asked to select could not be created", _id);
        }
        else
            errorLog()("{}: not selecting {}: there is no such workspace", _id, proto::describe(frame.pdu));
        // Answered whatever came of it, with the selection as it stands and the request named as the batch's
        // cause: a client that asked for a workspace the daemon does not have learns at once that it kept the
        // old one, and the request is not one the daemon does not understand.
        if (batchesLayouts())
        {
            _layoutBatchCause = std::max(_layoutBatchCause, frame.serial);
            openLayoutBatch();
            _layoutBatch = LayoutBatch::Pushed;
        }
        return TabVerb::Served;
    }
    if (auto const* move = std::get_if<proto::MoveWorkspace>(&frame.pdu))
    {
        if (!_negotiated->shared.contains(proto::Capability::WorkspaceMeta))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Applied to the model, which announces the move: every client is told the new positions.
        if (_host.model().workspace(vtworkspace::WorkspaceId { move->workspace }) == nullptr)
            errorLog()("{}: ignoring {}: there is no such workspace", _id, proto::describe(frame.pdu));
        else
            _host.model().moveWorkspace(
                vtworkspace::WorkspaceId { move->workspace },
                static_cast<int>(std::min<uint32_t>(move->toPosition, std::numeric_limits<int>::max())));
        return TabVerb::Served;
    }
    if (auto const* close = std::get_if<proto::CloseWorkspace>(&frame.pdu))
    {
        if (!_negotiated->shared.contains(proto::Capability::WorkspaceMeta))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Whatever the workspace holds now, in this one step: a tab another client added a moment ago too.
        if (_host.model().workspace(vtworkspace::WorkspaceId { close->workspace }) == nullptr)
            errorLog()("{}: ignoring {}: there is no such workspace", _id, proto::describe(frame.pdu));
        else
            _host.closeWorkspace(vtworkspace::WorkspaceId { close->workspace });
        return TabVerb::Served;
    }
    if (auto const* set = std::get_if<proto::SetWorkspaceLabel>(&frame.pdu))
    {
        if (!_negotiated->shared.contains(proto::Capability::WorkspaceMeta))
        {
            refuse(frame);
            return TabVerb::Served;
        }
        // Set on the object registry, whose label observers tell every client and every control subscriber.
        auto const uuid = _host.objects().uuidOf(vtworkspace::WorkspaceId { set->workspace });
        if (!uuid)
            errorLog()("{}: ignoring {}: there is no such workspace", _id, proto::describe(frame.pdu));
        else
            _host.objects().setLabel(
                vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = *uuid }, set->label);
        return TabVerb::Served;
    }
    if (std::holds_alternative<proto::ReportFocus>(frame.pdu))
    {
        if (!_negotiated->shared.contains(proto::Capability::DaemonFollow))
            refuse(frame);
        else
            _host.noteFocus(*this, std::get<proto::ReportFocus>(frame.pdu).focus);
        return TabVerb::Served;
    }
    if (auto const* reply = std::get_if<proto::SwitchReply>(&frame.pdu))
    {
        if (!_negotiated->shared.contains(proto::Capability::WorkspaceSwitch))
            refuse(frame);
        else
            _host.completeSwitch(_id, *reply);
        return TabVerb::Served;
    }
    return TabVerb::None;
}

NativeSession::GrantCheck NativeSession::requireGrant(vtcontrol::Permission permission,
                                                      proto::DecodedFrame const& frame)
{
    if (_grants.contains(permission))
        return GrantCheck::Granted;
    errorLog()("{}: refusing {} (serial {}): the token lacks grant '{}'",
               _id,
               proto::toString(proto::typeOf(frame.pdu)),
               frame.serial,
               vtcontrol::nameOf(permission));
    refuse(frame);
    return GrantCheck::Refused;
}

NativeSession::DaemonRequest NativeSession::serveDaemonRequest(proto::DecodedFrame const& frame)
{
    auto& daemon = _services.daemon.get();
    if (std::holds_alternative<proto::DaemonStatusRequest>(frame.pdu))
    {
        if (requireGrant(vtcontrol::Permission::Read, frame) == GrantCheck::Refused)
            return DaemonRequest::Served;
        if (auto const status = daemon.status())
            send(frame.serial, proto::DecodedPdu { *status });
        else
            refuse(frame);
        return DaemonRequest::Served;
    }
    if (auto const* upgrade = std::get_if<proto::UpgradeDaemon>(&frame.pdu))
    {
        // A PDU this daemon does not serve to a peer that did not negotiate it.
        if (!_negotiated->shared.contains(proto::Capability::LiveUpgrade))
        {
            refuse(frame);
            return DaemonRequest::Served;
        }
        if (requireGrant(vtcontrol::Permission::Write, frame) == GrantCheck::Refused)
            return DaemonRequest::Served;
        // Only a peer on this machine may replace the daemon's binary: a network peer's token is not that.
        if (!isLocal(_id))
        {
            errorLog()("{}: refusing an UpgradeDaemon: live upgrade is local only", _id);
            send(frame.serial,
                 proto::DecodedPdu {
                     upgradeFailureReply(handover::UpgradeError { handover::UpgradeRefusal::LocalOnly }) });
            return DaemonRequest::Served;
        }
        connectionLog()("{}: the client asked the daemon to upgrade to '{}'", _id, upgrade->imageId);
        _loop.spawn(serveUpgradeRequest(
            &daemon, _upgradeLink, frame.serial, handover::UpgradeTarget { .imageId = upgrade->imageId }));
        return DaemonRequest::Served;
    }
    if (auto const* stop = std::get_if<proto::StopDaemon>(&frame.pdu))
    {
        if (requireGrant(vtcontrol::Permission::Write, frame) == GrantCheck::Refused)
            return DaemonRequest::Served;
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

void NativeSession::replyThroughLink(UpgradeLink const& link,
                                     uint64_t serial,
                                     proto::UpgradeDaemonReply reply)
{
    if (link.session != nullptr)
        link.session->send(serial, proto::DecodedPdu { std::move(reply) });
}

core::async::Task<void> NativeSession::serveUpgradeRequest(DaemonControl* daemon,
                                                           std::shared_ptr<UpgradeLink> link,
                                                           uint64_t serial,
                                                           handover::UpgradeTarget target)
{
    // The answer to "started" is sent by the daemon from inside its announcement, ahead of DaemonUpgrading
    // and of the closing of this very connection; the callbacks hold the link, never the connection.
    co_await serveUpgrade(
        daemon,
        std::move(target),
        [link, serial](std::string_view) { replyThroughLink(*link, serial, upgradeStartedReply()); },
        [link, serial](handover::UpgradeError const& error) {
            replyThroughLink(*link, serial, upgradeFailureReply(error));
        });
}

void NativeSession::announceUpgrade(std::string_view toRelease)
{
    if (_closed || !_negotiated || !_negotiated->shared.contains(proto::Capability::LiveUpgrade))
        return;
    send(0, proto::DecodedPdu { proto::DaemonUpgrading { .toRelease = std::string { toRelease } } });
}

void NativeSession::abortForUpgrade()
{
    connectionLog()("{}: closing the connection at once for the daemon's upgrade", _id);
    // Whatever stage the end is in -- the pump still running, or the flush of what is queued waiting on a
    // peer that reads nothing -- a parked read or write ends cancelled, and run() unwinds.
    _upgradeEnd = UpgradeEnd::Ending;
    _connection->close();
}

void NativeSession::closeForUpgrade()
{
    if (_closed)
        return;
    connectionLog()("{}: ending the connection for the daemon's upgrade", _id);
    _upgradeEnd = UpgradeEnd::Ending;
    // Retires the read the pump is parked on; the pump then sees it must read no more, and run() flushes the
    // queue before it closes.
    _connection->cancelRead();
}

namespace
{
    /// What a control call knows about the connection serving it; lives for the call alone.
    class RequestContext final: public vtcontrol::CallContext
    {
      public:
        RequestContext(std::string_view caller,
                       vtcontrol::EventSink& events,
                       vtcontrol::ControlGrants grants,
                       vtcontrol::CallerLocality locality):
            _caller(caller), _events(events), _grants(grants), _locality(locality)
        {
        }

        [[nodiscard]] std::string_view caller() const noexcept override { return _caller; }
        [[nodiscard]] vtcontrol::EventSink& events() noexcept override { return _events; }
        [[nodiscard]] vtcontrol::ControlGrants grants() const noexcept override { return _grants; }
        [[nodiscard]] vtcontrol::CallerLocality locality() const noexcept override { return _locality; }

      private:
        std::string_view _caller;
        vtcontrol::EventSink& _events;
        vtcontrol::ControlGrants _grants;
        vtcontrol::CallerLocality _locality;
    };
} // namespace

NativeSession::DaemonRequest NativeSession::serveControlApiRequest(proto::DecodedFrame const& frame)
{
    auto const* request = std::get_if<proto::ControlRequest>(&frame.pdu);
    if (request == nullptr)
        return DaemonRequest::None;
    // The control API, for a Control connection that shares it; to anyone else a PDU this daemon does not
    // serve them.
    if (_phase == ConnectionPhase::Controlling && _negotiated->shared.contains(proto::Capability::ControlApi))
        serveControlRequest(frame.serial, *request);
    else
        refuse(frame);
    return DaemonRequest::Served;
}

void NativeSession::serveControlRequest(uint64_t serial, proto::ControlRequest const& request)
{
    auto context = RequestContext { request.context,
                                    _controlEvents,
                                    _grants,
                                    isLocal(_id) ? vtcontrol::CallerLocality::Local
                                                 : vtcontrol::CallerLocality::Remote };
    vtcontrol::dispatch(_services.control.get(),
                        context,
                        vtcontrol::Caller::Socket,
                        request.method,
                        request.params,
                        // Possibly completed later -- a wait, or an upgrade -- and an upgrade's flow outlives
                        // the connection (it closes this very one), so the completion holds the link, never
                        // `this`: a wait is dropped by run() before it lets `this` go, an upgrade is not.
                        [link = _upgradeLink, serial, requestId = request.requestId](
                            std::expected<std::string, vtcontrol::ControlError> result) {
                            auto* const session = link->session;
                            if (session == nullptr || session->_closed)
                                return;
                            auto reply = proto::ControlReply { .requestId = requestId };
                            if (result)
                                reply.body = std::move(*result);
                            else
                            {
                                reply.outcome = proto::ControlOutcome::Error;
                                reply.body = vtcontrol::encodeJson(result.error());
                            }
                            session->send(serial, proto::DecodedPdu { std::move(reply) });
                        });
}

void NativeSession::deliverControlEvent(uint32_t subscription, vtcontrol::Event const& event)
{
    if (_closed)
        return;
    // A subscriber that does not read must not cost the daemon memory, nor itself the connection: past half
    // the bound, events are counted instead of queued, leaving the other half to the replies.
    if (_writer.backlogBytes() > _maxWriteQueueBytes / 2)
    {
        ++_droppedEvents[subscription];
        return;
    }
    if (auto const dropped = _droppedEvents.find(subscription); dropped != _droppedEvents.end())
    {
        auto const count = dropped->second;
        _droppedEvents.erase(dropped);
        send(0,
             proto::DecodedPdu { proto::ControlEvent {
                 .subscription = subscription,
                 .event = vtcontrol::encodeEvent(vtcontrol::Event {
                     .origin = {}, .payload = vtcontrol::EventsDroppedEvent { .count = count } }) } });
    }
    send(0,
         proto::DecodedPdu {
             proto::ControlEvent { .subscription = subscription, .event = vtcontrol::encodeEvent(event) } });
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

namespace
{
    /// How core-cpp's SSHSIG refusals read in a log; the signature itself is never logged.
    struct SigErrorName
    {
        core::net::SshSigError error;
        std::string_view name;
    };
    constexpr auto SigErrorNames = std::to_array<SigErrorName>({
        { core::net::SshSigError::Malformed, "malformed signature" },
        { core::net::SshSigError::WrongNamespace, "wrong namespace" },
        { core::net::SshSigError::KeyMismatch, "signed by another key" },
        { core::net::SshSigError::BadSignature, "signature does not verify" },
        { core::net::SshSigError::UserNotPresent, "security key user presence not set" },
        { core::net::SshSigError::UnsupportedHash, "unsupported hash" },
    });
} // namespace

void NativeSession::preAuthExpired()
{
    if (_closed)
        return;
    errorLog()("{}: no hello was accepted within {}s of the accept; closing", _id, PreAuthTimeout.count());
    _closed = true;
    // The queue closes _connection too, which resumes the parked read and ends the flow that owns this.
    _writer.close();
}

void NativeSession::sendKeyChallenge(KeyAuthPolicy const& keyAuth, uint64_t serial)
{
    keyAuth.random.get().fill(_keyNonce);
    auto const methods = std::to_underlying(proto::AuthMethod::Key)
                         | (keyAuth.auth == DaemonAuth::Any && !_services.handshake.acceptedTokens.empty()
                                ? std::to_underlying(proto::AuthMethod::Token)
                                : 0);
    send(serial,
         proto::DecodedPdu {
             proto::AuthChallenge { .nonce = _keyNonce, .methods = static_cast<std::uint8_t>(methods) } });
}

std::optional<vtcontrol::ControlGrants> NativeSession::verifyKeyProof(KeyAuthPolicy const& keyAuth,
                                                                      proto::KeyProof const& proof) const
{
    auto const* entry = findAuthorizedKeyByBlob(keyAuth.keys, proof.publicKey);
    if (entry == nullptr)
    {
        // The fingerprint names the key for the operator who must add it; the signature is never logged.
        auto const offered = core::net::parseSshPublicKeyBlob(proof.publicKey);
        errorLog()("{}: key proof refused: {} is not an authorized key",
                   _id,
                   offered ? core::net::sshFingerprintOf(*offered) : std::string { "an unparsable key" });
        return std::nullopt;
    }
    auto const message = keyAuthMessage(_keyNonce, keyAuth.channelBinding);
    auto const verified =
        core::net::verifySshSignature(entry->key, KeyAuthNamespace, message, proof.signature);
    if (!verified)
    {
        auto const named = std::ranges::find(SigErrorNames, verified.error(), &SigErrorName::error);
        errorLog()("{}: key proof refused for {}: {}",
                   _id,
                   core::net::sshFingerprintOf(entry->key),
                   named != SigErrorNames.end() ? named->name : std::string_view { "refused" });
        return std::nullopt;
    }
    connectionLog()("{}: key {} authenticated", _id, core::net::sshFingerprintOf(entry->key));
    return entry->grants;
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
    auto grants = std::optional<vtcontrol::ControlGrants> {};
    if (!policy.keyAuth)
        grants = grantsOf(policy.acceptedTokens, hello->token);
    else if (hello->keyProof)
    {
        grants = verifyKeyProof(*policy.keyAuth, *hello->keyProof);
        if (!grants)
        {
            ++_keyAttempts;
            if (_keyAttempts < MaxKeyAuthAttempts)
            {
                // Another go, on a fresh nonce: the agent may hold several keys, and a refused one says
                // nothing about which are listed.
                sendKeyChallenge(*policy.keyAuth, frame.serial);
                return true;
            }
        }
    }
    else if (policy.keyAuth->auth == DaemonAuth::Any && !policy.acceptedTokens.empty())
        grants = grantsOf(policy.acceptedTokens, hello->token);
    // Otherwise a hello with no proof has nothing to be admitted by: an endpoint with keys but no token must
    // not read its empty token list as "accept any".
    if (!grants)
    {
        // Server-side only, and WITHOUT either token: the wire answer deliberately makes
        // this indistinguishable from a version mismatch, so the log must not undo on disk what
        // the protocol is careful not to reveal on the network.
        errorLog()("{}: handshake rejected: authentication failed", _id);
        send(frame.serial, proto::DecodedPdu { refusalFor(*hello, policy.versions) });
        return false;
    }
    // A narrow token is for Control connections alone: a viewer types into and reads every pane. Refused
    // here, before the client is counted or anything spawns, with the same bytes as a wrong token.
    if (hello->role == proto::ConnectionRole::Attach && *grants != vtcontrol::ControlGrants::all())
    {
        errorLog()("{}: handshake rejected: token lacks the grants an attach connection needs (has {})",
                   _id,
                   vtcontrol::toString(*grants));
        send(frame.serial, proto::DecodedPdu { refusalFor(*hello, policy.versions) });
        return false;
    }
    _grants = *grants;
    _preAuthTimer.reset(); // authenticated: the deadline is met
    // Only an accepted client learns which machine the daemon runs on: what every record's locality was
    // judged against, so the client can tell whose "local" that is before it mirrors a single one. By an
    // id derived for this application, never the machine's own, which machine-id(5) keeps confidential.
    auto stated = appSpecificIdentity(_host.identity().view());
    auto reply =
        proto::ServerHello { .agreedVersion = *agreed,
                             .versions = policy.versions,
                             .capabilities = policy.capabilities,
                             .machineId = std::move(stated.machineId),
                             .hostname = std::move(stated.hostname),
                             .releaseVersion = std::string { _services.daemon.get().releaseVersion() } };
    auto negotiated =
        proto::Negotiated { .version = *agreed,
                            .peerCapabilities = hello->capabilities,
                            .shared = proto::intersect(policy.capabilities, hello->capabilities) };
    switch (hello->role)
    {
        case proto::ConnectionRole::Attach:
            connectionLog()("{}: handshake complete (protocol v{})", _id, *agreed);
            attachClient(frame.serial, *hello, std::move(reply), std::move(negotiated));
            break;
        case proto::ConnectionRole::Control:
            // A probe, a command to the daemon itself, or a control API caller -- whether or not it shares
            // the API, as the status and stop verbs ride the same role: shown nothing, spawning nothing,
            // counted as no one, and no client attaching.
            connectionLog()("{}: handshake complete (protocol v{}, control connection)", _id, *agreed);
            send(frame.serial, proto::DecodedPdu { std::move(reply) });
            _negotiated = std::move(negotiated);
            _phase = ConnectionPhase::Controlling;
            break;
    }
    return true;
}

std::optional<vtpty::ImageSize> NativeSession::statedCellPixelSize(
    std::optional<proto::WireCellPixelSize> const& stated, std::string_view origin) const
{
    if (!stated)
        return std::nullopt;
    auto const cell = sanePixelSize(stated->width, stated->height);
    if (!cell)
        errorLog()(
            "{}: rejecting {} cell size {}x{} (out of range)", _id, origin, stated->width, stated->height);
    return cell;
}

void NativeSession::attachClient(uint64_t serial,
                                 proto::ClientHello const& hello,
                                 proto::ServerHello reply,
                                 proto::Negotiated negotiated)
{
    // Known before the host counts the client (executeAttachPlan): its routing decides who raises from the
    // first view it takes, and a client that does not follow the daemon is in none.
    _following = negotiated.shared.contains(proto::Capability::DaemonFollow) ? DaemonFollowing::Yes
                                                                             : DaemonFollowing::No;
    // Counted, and subscribed, first: the spawn below fans out to the subscribers, as it did when every
    // connection subscribed before its hello.
    _clientTicket = _services.daemon.get().admitClient();
    _streamSubscription.emplace(_host, *this, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream);
    _layoutSubscription.emplace(_host, _layoutObserver, &SessionHost::subscribe, &SessionHost::unsubscribe);
    if (negotiated.shared.contains(proto::Capability::WorkspaceMeta))
        _labelSubscription.emplace(
            _host.objects().onLabelChanged([this](vtcontrol::ObjectRef const& ref, std::string_view label) {
                workspaceLabelChanged(ref, label);
            }));

    // Adopted BEFORE the spawn below: that session exists because THIS client attached, so it
    // inherits this client's profile like any tab the client goes on to open. A profile the host
    // resolves by name supersedes the settings the client states: the host decides what runs.
    if (negotiated.shared.contains(proto::Capability::ProfileByName) && !hello.profileName.empty())
    {
        _profileName = hello.profileName;
        connectionLog()("{}: sessions this client creates run the host's profile '{}'", _id, _profileName);
    }
    else
        adoptClientSessionSettings(hello.sessionSettings);

    // The directory the attach's own session starts in, handed to the plan that spawns it. Only an absolute
    // one: a relative path names a place under the client's working directory, which the daemon cannot see.
    auto const workingDirectory = [&]() -> std::filesystem::path {
        if (hello.workingDirectory.empty())
            return {};
        if (auto path = core::pathFromUtf8(hello.workingDirectory); path && path->is_absolute())
            return *std::move(path);
        errorLog()("{}: ignoring the hello's working directory (not an absolute path)", _id);
        return {};
    }();

    // The stated client area, applied BEFORE the spawn and the attach snapshots below, so the first
    // session spawns at the size the client displays and its snapshot needs no resize and resync. An
    // area out of range is refused as a ResizeRequest's is; the session then spawns at the host's
    // size and the client's own ResizeRequest corrects it, as it does against a daemon that predates
    // the field. The cell rides on the area, so the first session spawns at both; one stated beside a
    // refused area still counts, as it describes the client, not the area.
    auto const helloCell = statedCellPixelSize(hello.cellPixelSize, "hello");
    auto const helloArea = hello.clientArea.and_then(
        [](proto::WireClientArea const& area) { return sanePageSize(area.columns, area.lines); });
    if (helloArea)
        std::ignore = _host.applyClientSize(this, *helloArea, helloCell);
    else
    {
        if (hello.clientArea)
            errorLog()("{}: rejecting hello client area {}x{} (out of range)",
                       _id,
                       hello.clientArea->columns,
                       hello.clientArea->lines);
        if (helloCell)
            _host.applyCellPixelSize(this, *helloCell);
    }

    // The preference the client shows, applied BEFORE the plan spawns anything, so a session spawned for this
    // attach is built under it and its first snapshot is no flash of the other scheme.
    auto const colorPreference = [&]() -> std::optional<vtbackend::ColorPreference> {
        switch (hello.colorPreference)
        {
            case proto::WireColorPreference::Dark: return vtbackend::ColorPreference::Dark;
            case proto::WireColorPreference::Light: return vtbackend::ColorPreference::Light;
            case proto::WireColorPreference::Unstated: break;
        }
        return std::nullopt;
    }();
    if (colorPreference)
        _host.adoptColorPreference(*colorPreference);

    // After the client area, so whatever the plan spawns spawns at the size the client displays (D7). A
    // client that cannot state an intent is a launch, as every client was before intents existed.
    executeAttachPlan(negotiated.shared.contains(proto::Capability::AttachIntent) ? hello.attachIntent
                                                                                  : proto::AttachIntent {},
                      workingDirectory);

    reply.selectedWorkspace = _selectedWorkspace;
    reply.daemonInstance = _host.instance();
    // Told to the first attaching client only, whichever workspace it selects: the host forgets it with the
    // answer. Every workspace is listed to it.
    if (auto const restored = _host.takeRestoreAnnouncement([](uint64_t) { return true; }))
    {
        reply.restoredWorkspaces = restored->workspaces;
        reply.restoredTabs = restored->tabs;
    }
    send(serial, proto::DecodedPdu { std::move(reply) });
    _negotiated = std::move(negotiated);
    _phase = ConnectionPhase::Viewing;
    // The role is stated once, whatever it is, so a window never has to assume one; a change is sent as it
    // happens.
    if (summarizesPanes())
        send(0, proto::DecodedPdu { proto::NotifyRole { .role = _raiseRole } });
    // The idle check of the echo state starts one interval from now, and runs for the activity window.
    _nextInputEchoCheck = _loop.clock().now() + _services.flush.inputEchoCheck;
    noteInputEchoActivity();
    if (mirrorsInputEcho())
        scheduleFlush(_nextInputEchoCheck);

    // The workspace/tab/pane layout first, so the client builds its tabs and split
    // trees before the per-session content streams into them.
    pushLayout();
    announceUnmirroredPanes();

    // A live upgrade that could not bring a session's screen back says so to the first client that mirrors
    // the session: what it sees is a fresh screen, the program behind it untouched.
    for (auto const session:
         _host.takeUnrestoredScreens([this](vtworkspace::SessionId id) { return mirrorsSession(id); }))
        emitSessionEvent(proto::SessionNotify { .session = session.value,
                                                .title = std::string { handover::UnrestoredScreenTitle },
                                                .body = std::string { handover::UnrestoredScreenNotice },
                                                .occasion = proto::NotifyOccasion::Always });

    // What the daemon could not start as configured after an upgrade, told on the first session this client
    // mirrors -- the notice is the daemon's, a session only where a client mirrors it -- and kept for a
    // client that mirrors one when this one mirrors none.
    auto const sessions = _host.sessionIds();
    if (auto const shown =
            std::ranges::find_if(sessions, [this](vtworkspace::SessionId id) { return mirrorsSession(id); });
        shown != sessions.end())
        for (auto& notice: _host.takeDegradations())
            emitSessionEvent(proto::SessionNotify { .session = shown->value,
                                                    .title = std::string { handover::UnrestoredScreenTitle },
                                                    .body = std::move(notice),
                                                    .occasion = proto::NotifyOccasion::Always });

    // The sessions that already existed take the stated preference before their snapshot is built; a change
    // only, so what a program set on one survives.
    if (colorPreference)
        _host.model().forEachTab([&](vtworkspace::Workspace& workspace, vtworkspace::Tab& tab) {
            if (workspace.id().value != _selectedWorkspace)
                return;
            tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
                if (pane.isLeaf())
                    _host.applyColorPreference(pane.session(), *colorPreference);
            });
        });

    // The attach snapshot: every hosted session of the selected workspace, full state. QUEUED, not pushed:
    // this runs inside the PDU pump's handler, which cannot suspend, so pushing here would put
    // every pane's whole grid into the send queue before the drain coroutine had run once.
    _host.model().forEachTab([this](vtworkspace::Workspace& workspace, vtworkspace::Tab& tab) {
        if (workspace.id().value != _selectedWorkspace)
            return;
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                requestSnapshot(pane.session());
        });
    });

    // The questions still open: asked while no client was attached, or left unanswered by one that was --
    // a dismissed prompt sends nothing. A status-line extension waits for an answer, its command calls
    // failing as pending meanwhile. A client is sent each once, as it opens or as the client attaches --
    // one that cannot answer is sent none, and they wait for another, as with no client attached.
    for (auto const& request: _host.pendingPermissionRequests())
        if (asks(request) && mirrorsSession(SessionId { request.session }))
            send(0, proto::DecodedPdu { request });
}

void NativeSession::executeAttachPlan(proto::AttachIntent const& intent,
                                      std::filesystem::path const& workingDirectory)
{
    auto& model = _host.model();
    auto context = AttachContext { .primaryWorkspace = _host.workspaceId().value,
                                   .workspaces = {},
                                   .workingDirectory = workingDirectory.empty() ? WorkingDirectoryStated::No
                                                                                : WorkingDirectoryStated::Yes,
                                   .instance = _host.instance(),
                                   .lastSelected = _host.lastSelectedWorkspace() };
    // A workspace whose last session ended is not listed: a Select naming it is then a launch's choice, as a
    // SelectWorkspace of it is refused (serveWorkspaceVerb), and the connection never lands on it empty.
    for (auto const i: std::views::iota(0, model.workspaceCount()))
        if (auto const* workspace = model.workspaceAt(i);
            workspace != nullptr && !_host.hasEnded(workspace->id()))
            context.workspaces.push_back(
                WorkspaceSummary { .id = workspace->id().value, .tabCount = workspace->tabCount() });
    std::ranges::sort(context.workspaces, {}, &WorkspaceSummary::id);

    auto const plan = planAttach(intent, context);
    _host.noteClientAttached(this);
    _services.control.get().clientAttached(std::format("{}", _id));
    // Set directly: the attach walk queues the snapshots of the selected workspace once the hello is sent.
    if (plan.select)
    {
        _selectedWorkspace = *plan.select;
        _selectedTabs = SelectedTabs::NeverHeld;
        _host.noteSelected(*this, _selectedWorkspace);
    }

    switch (plan.creation)
    {
        case AttachCreation::None: break;
        case AttachCreation::FirstTabInPrimary:
            connectionLog()("{}: daemon has no tabs; spawning the first session", _id);
            std::ignore = _host.createTab(attachSpawnRequest(workingDirectory));
            break;
        case AttachCreation::NewWorkspace:
            if (auto const* workspace = _host.createWorkspace(attachSpawnRequest(workingDirectory)))
            {
                connectionLog()("{}: attaching to a new workspace {}", _id, workspace->id().value);
                _selectedWorkspace = workspace->id().value;
                _host.noteSelected(*this, _selectedWorkspace);
            }
            else
                errorLog()("{}: the workspace this attach asked for could not be created", _id);
            break;
        case AttachCreation::TabInSelected: {
            // The workspace selected gets the terminal asked for in the stated directory as one more tab,
            // named by a session it hosts, which is how createTab finds a workspace.
            auto const workspace = vtworkspace::WorkspaceId { _selectedWorkspace };
            auto const* shown = model.workspace(workspace);
            auto const* tab = shown != nullptr ? shown->activeTab() : nullptr;
            auto const beside =
                tab != nullptr ? std::optional { tab->rootPane()->firstLeaf()->session() } : std::nullopt;
            connectionLog()(
                "{}: opening a tab in workspace {} for the stated directory", _id, workspace.value);
            if (_host.createTab(attachSpawnRequest(workingDirectory), beside) == nullptr)
                errorLog()("{}: the tab for the stated directory could not be created", _id);
            break;
        }
    }
}

void NativeSession::reportPumpOutcome(PumpResult const& outcome) const
{
    // The read the upgrade retired fails as cancelled: that is the way this connection ends, not a failure.
    if (_upgradeEnd == UpgradeEnd::Ending)
        return;
    if (auto const reason = describe(outcome))
    {
        if (isFailure(outcome.stop))
            errorLog()("{}: {}", _id, *reason);
        else
            connectionLog()("{}: {}", _id, *reason);
    }
}

NativeSession::SpawnedFlowClaim::~SpawnedFlowClaim()
{
    if (_session == nullptr)
        return;
    _release(*_session);
    // Queued, not resumed inline: this may run inside the frame of the flow giving the claim back.
    if (_session->_spawnedFlowsWaiter && _session->spawnedFlowsEnded())
        _session->_loop.submit(std::exchange(_session->_spawnedFlowsWaiter, {}));
}

NativeSession::SpawnedFlowClaim NativeSession::claimFlush()
{
    ++_flushesInFlight;
    return SpawnedFlowClaim { *this, [](NativeSession& self) noexcept { --self._flushesInFlight; } };
}

NativeSession::SpawnedFlowClaim NativeSession::claimStreamer(SessionId session)
{
    _streamingSession = session.value;
    return SpawnedFlowClaim { *this, [](NativeSession& self) noexcept { self._streamingSession.reset(); } };
}

NativeSession::SpawnedFlowClaim NativeSession::claimLayoutBatch()
{
    _layoutBatch = LayoutBatch::Open;
    return SpawnedFlowClaim { *this,
                              [](NativeSession& self) noexcept { self._layoutBatch = LayoutBatch::Closed; } };
}

NativeSession::SpawnedFlowClaim NativeSession::claimRoomWaiter()
{
    _roomWaiter = RoomWaiter::Parked;
    return SpawnedFlowClaim { *this,
                              [](NativeSession& self) noexcept { self._roomWaiter = RoomWaiter::Idle; } };
}

/// An awaiter, so its three members carry the names the coroutine machinery calls them by.
struct NativeSession::SpawnedFlowsEnded
{
    NativeSession* session;

    // NOLINTNEXTLINE(readability-identifier-naming) -- bound by spelling: the coroutine machinery calls it.
    [[nodiscard]] bool await_ready() const noexcept { return session->spawnedFlowsEnded(); }
    // NOLINTNEXTLINE(readability-identifier-naming) -- bound by spelling: the coroutine machinery calls it.
    void await_suspend(std::coroutine_handle<> awaiting) const noexcept
    {
        session->_spawnedFlowsWaiter = awaiting;
    }
    // NOLINTNEXTLINE(readability-identifier-naming) -- bound by spelling: the coroutine machinery calls it.
    void await_resume() const noexcept {}
};

core::async::Task<void> NativeSession::run()
{
    _upgradeSubscription.emplace(_services.daemon.get(), *this);
    // Whatever ends the pump ends it here, so that the barrier below runs on every way out. Every exception,
    // not just a stopped loop's OperationCancelled: any of them leaves this frame, and serveNativeClient
    // frees `this` the moment it does -- under every flow still holding it, which is the use-after-free the
    // barrier exists to prevent. Rethrown, unchanged, once that barrier is passed.
    auto failure = std::exception_ptr {};
    try
    {
        if (auto const deadline = _services.handshake.authDeadline)
            _preAuthTimer.emplace(
                _loop,
                *deadline,
                [](void* state) { static_cast<NativeSession*>(state)->preAuthExpired(); },
                this);
        // A daemon that authenticates by key speaks first: the challenge the hello's proof must answer.
        if (_services.handshake.keyAuth)
            sendKeyChallenge(*_services.handshake.keyAuth, 0);
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
                if (_phase == ConnectionPhase::AwaitingHello)
                    return completeHandshake(frame);
                handlePdu(frame);
                return true;
            },
            [this] { return _upgradeEnd == UpgradeEnd::Ending; },
            [this] { return awaitInputRoom(); });
        reportPumpOutcome(outcome);
    }
    catch (...)
    {
        failure = std::current_exception();
    }

    _closed = true;
    // Before the barrier below: an upgrade's flow must find no connection that is going.
    _upgradeLink->session = nullptr;
    // Before the barrier below: a wait completing later must find no sink of a connection that is going.
    _services.control.get().sinkClosed(_controlEvents);
    // Lifetime constraint: serveNativeClient destroys this session the moment
    // run() returns (the unique_ptr goes out of scope immediately after the
    // co_await). Flows spawned before the disconnect may still hold `this` --
    // flushes parked until their deadlines, superseded ones included, or not
    // started yet; the snapshot streamer; the waiter for send room -- so this
    // barrier MUST remain the last thing run() does before it lets go of `this`.
    // Any refactoring that moves logic after this point opens a use-after-free
    // workspace on every one of them.
    //
    // The barrier is no poll, so a stopped loop -- whose every wait throws -- cannot cut it short, and it
    // never spins: it parks until the last claim is given back (@see SpawnedFlowClaim). It ends because
    // every claimed flow ends: the flows park only on the loop's own waits, which `_closed` (set above)
    // ends -- waitUntilBacklogBelow returns on it, and the streamer's loops test it -- or a deadline does,
    // and a stopped loop unparks and cancels all of them; a frame the loop frees unstarted gives its claim
    // back from its parameter. Were one never to end, this would wait rather than spin, and LoopDrain's
    // bounded drain would report the flow still alive.
    co_await SpawnedFlowsEnded { this };
    if (failure)
    {
        _upgradeSubscription.reset();
        std::rethrow_exception(failure);
    }
    co_await closeGracefully(&_loop, &_writer, _connection.get());
    // Subscribed until here, so that an upgrade's abortForUpgrade() reaches a connection whose orderly end a
    // peer that reads nothing holds up; closeForUpgrade() and announceUpgrade() see `_closed` meanwhile.
    _upgradeSubscription.reset();
}

bool NativeSession::spawnedFlowsEnded() const noexcept
{
    return _flushesInFlight == 0 && !_streamingSession && _roomWaiter == RoomWaiter::Idle
           && _layoutBatch == LayoutBatch::Closed;
}

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

namespace
{
    /// What the handshake deadline's timer needs to end a connection and say so.
    struct HandshakeExpiry
    {
        core::net::ISocket* socket;
        ConnectionId const* id;
    };

    /// One TLS client's lifetime: the handshake settled first, so that its channel binding is known before
    /// the native session speaks, then the session itself. A free coroutine for the reason serveNativeClient
    /// is.
    core::async::Task<void> serveTlsNativeClient(core::net::EventLoop* loop,
                                                 SessionHost* host,
                                                 ConnectionId id,
                                                 std::unique_ptr<core::net::ISocket> socket,
                                                 NativeSessionServices services,
                                                 std::shared_ptr<TlsKeyAuth const> keyAuth)
    {
        // From the accept: the TLS handshake and then the hello share this one budget, so a peer that
        // stalls in either is closed. Closing the socket ends whatever is parked on it.
        auto const deadline = loop->clock().now() + PreAuthTimeout;
        services.handshake.authDeadline = deadline;
        auto expiry = HandshakeExpiry { .socket = socket.get(), .id = &id };
        auto handshakeTimer = core::net::DeadlineTimer {
            *loop,
            deadline,
            [](void* state) {
                auto const& stalled = *static_cast<HandshakeExpiry*>(state);
                errorLog()("{}: the TLS handshake was not finished within {}s of the accept; closing",
                           *stalled.id,
                           PreAuthTimeout.count());
                stalled.socket->close();
            },
            &expiry
        };
        auto const handshaken = co_await socket->handshakeIfNeeded();
        if (!handshaken)
            co_return; // No channel to answer on: dropped, as an accept loop drops it.
        handshakeTimer.disarm();
        if (keyAuth)
        {
            auto const* const session = core::net::tlsSessionOf(*socket);
            auto binding = session != nullptr ? session->channelBinding()
                                              : std::unexpected(std::string { "not a TLS connection" });
            if (!binding)
            {
                errorLog()("{}: no channel binding, refusing the connection: {}", id, binding.error());
                co_return;
            }
            services.handshake.keyAuth = KeyAuthPolicy { .keys = keyAuth->keys,
                                                         .auth = keyAuth->auth,
                                                         .channelBinding = *binding,
                                                         .random = keyAuth->random };
        }
        co_await serveNativeClient(loop, host, std::move(id), std::move(socket), std::move(services));
    }
} // namespace

std::expected<ConnectionHandler, std::string> makeTlsNativeHandler(
    core::net::EventLoop& loop,
    SessionHost& host,
    std::shared_ptr<core::net::ITlsContext> tls,
    NativeSessionServices services,
    TlsKeyAuth keyAuth)
{
    // An empty token list accepts any hello, which is right only where a socket's permissions are the gate.
    // The command line refuses such a listener already; this is the rule where it is enforced, whoever builds
    // the listener.
    if (keyAuth.keys.empty() && services.handshake.acceptedTokens.empty())
        return std::unexpected(
            std::string { "a TCP listener needs an authorized key or a token: with neither it "
                          "would admit anyone who connects" });
    auto shared = keyAuth.keys.empty() ? std::shared_ptr<TlsKeyAuth const> {}
                                       : std::make_shared<TlsKeyAuth const>(std::move(keyAuth));
    // NOT a coroutine itself, as makeNativeHandler's is not: it only builds the free coroutine's task.
    return ConnectionHandler {
        [tls = std::move(tls), services = std::move(services), shared = std::move(shared), &loop, &host](
            ConnectionId id, std::unique_ptr<core::net::ISocket> socket) {
            return serveTlsNativeClient(
                &loop, &host, std::move(id), tls->wrap(std::move(socket), loop), services, shared);
        }
    };
}

} // namespace vthost
