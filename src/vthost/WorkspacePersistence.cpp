// SPDX-License-Identifier: Apache-2.0
#include <vthost/WorkspacePersistence.hpp>

#include <vtbackend/screen/LocalTime.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <format>
#include <utility>

#include <vthost/HistorySeed.hpp>

namespace vthost
{

namespace
{
    /// @return What @p step was doing, for a log line.
    [[nodiscard]] constexpr std::string_view describe(StoreStep step) noexcept
    {
        switch (step)
        {
            case StoreStep::CreateDirectory: return "creating its directory";
            case StoreStep::Restrict: return "making it private";
            case StoreStep::WriteTemporary: return "writing it";
            case StoreStep::SyncTemporary: return "flushing it to the disk";
            case StoreStep::Replace: return "replacing the previous one";
            case StoreStep::SyncDirectory: return "flushing its replacement to the disk";
        }
        return "writing it";
    }

    [[nodiscard]] std::filesystem::path withSuffix(std::filesystem::path const& path, std::string_view suffix)
    {
        auto name = path.filename().string();
        name += suffix;
        return path.parent_path() / name;
    }
} // namespace

// ---------------------------------------------------------------------------
// SnapshotWriter

SnapshotWriter::SnapshotWriter(SessionSnapshotStore const& store,
                               WriterThreading threading,
                               DiagnosticSink report):
    _store { store },
    _report { std::move(report) },
    _thread { threading == WriterThreading::Background
                  ? std::jthread { [this](std::stop_token const& stop) { run(stop); } }
                  : std::jthread {} }
{
}

SnapshotWriter::~SnapshotWriter()
{
    // What was submitted reaches the disk before the thread goes; the jthread then stops and joins.
    flush();
}

void SnapshotWriter::submit(WorkspaceSnapshot snapshot, AfterWrite after)
{
    if (!_thread.joinable())
    {
        write(Pending { .snapshot = std::move(snapshot), .after = after });
        return;
    }
    {
        auto const lock = std::scoped_lock { _mutex };
        // A confirmation asked for by the snapshot replaced is still owed by the one replacing it.
        if (_pending)
            after = std::max(after, _pending->after);
        _pending = Pending { .snapshot = std::move(snapshot), .after = after, .generation = ++_submitted };
    }
    _changed.notify_all();
}

void SnapshotWriter::flush()
{
    if (!_thread.joinable())
        return;
    auto lock = std::unique_lock { _mutex };
    auto const target = _submitted;
    _changed.wait(lock, [&] { return _written >= target; });
}

void SnapshotWriter::run(std::stop_token const& stop)
{
    auto lock = std::unique_lock { _mutex };
    // Ends only once stopped with nothing pending: the destructor flushes before it stops.
    while (_changed.wait(lock, stop, [&] { return _pending.has_value(); }))
    {
        auto job = *std::move(_pending);
        _pending.reset();
        lock.unlock();
        write(job);
        lock.lock();
        _written = job.generation;
        _changed.notify_all();
    }
}

void SnapshotWriter::write(Pending const& pending)
{
    auto const written = _store.write(pending.snapshot);
    if (!written)
    {
        _health.store(WriteHealth::Failing, std::memory_order_release);
        // Once per failing step: a full disk is not news every interval.
        if (_failingStep != written.error().step)
            _report(std::format("workspace snapshot not written ({}): {}; the previous one is kept",
                                describe(written.error().step),
                                written.error().detail));
        _failingStep = written.error().step;
        return;
    }
    _health.store(WriteHealth::Healthy, std::memory_order_release);
    if (_failingStep)
        _report("workspace snapshot writes recovered");
    _failingStep.reset();

    if (pending.after != AfterWrite::ConfirmRestored || _restore == RestoreConfirmation::Confirmed)
        return;
    if (auto const confirmed = _store.confirmRestored(); !confirmed)
    {
        // Retried with the next write: a `.restored` left behind would be restored in place of this file.
        if (_restore == RestoreConfirmation::Unconfirmed)
            _report(std::format("the restored workspace's copy could not be removed: {}",
                                confirmed.error().detail));
        _restore = RestoreConfirmation::Retrying;
        return;
    }
    _restore = RestoreConfirmation::Confirmed;
}

// ---------------------------------------------------------------------------
// WorkspacePersistence

WorkspacePersistence::WorkspacePersistence(core::net::EventLoop& loop,
                                           SessionHost& host,
                                           SnapshotWriter& writer,
                                           WorkspacePersistenceConfig config,
                                           core::platform::IWallClock const& wallClock):
    _loop { loop },
    _host { host },
    _writer { writer },
    _config { std::move(config) },
    _wallClock { wallClock },
    _gate { std::make_shared<Gate>(Gate { .self = this }) },
    _modelSubscription { host, *this, &SessionHost::subscribe, &SessionHost::unsubscribe },
    _streamSubscription { host, *this, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream }
{
    armPeriodic();
}

WorkspacePersistence::~WorkspacePersistence()
{
    _gate->self = nullptr;
}

void WorkspacePersistence::finalize(ShutdownWorkspace disposition)
{
    if (_phase == Phase::Sealed)
        return;
    _phase = Phase::Sealed;
    _periodic.reset();
    _deferred.reset();
    switch (disposition)
    {
        case ShutdownWorkspace::Keep:
            if (decideSnapshotWrite(SnapshotTrigger::Shutdown, _state, _loop.clock().now(), _config.schedule)
                    .action
                == WriteAction::WriteNow)
                writeSnapshot();
            break;
        case ShutdownWorkspace::Discard:
            // Nothing is to come back, from this file or from a restore's kept copy.
            _writer.submit(
                WorkspaceSnapshot { .takenAt = std::chrono::floor<std::chrono::seconds>(_wallClock.now()) },
                AfterWrite::ConfirmRestored);
            break;
    }
    _writer.flush();
}

void WorkspacePersistence::sessionScreenUpdated(vtworkspace::SessionId /*session*/)
{
    // Picked up by the next periodic write: output is too frequent to schedule a write of its own.
    _state.snapshot = SnapshotState::Dirty;
}

void WorkspacePersistence::sessionEnded(vtworkspace::SessionId /*session*/, SessionEnd cause)
{
    if (_phase == Phase::Sealed)
        return;
    // Inline, not posted: a shutdown posted right after the last session closed must already see the kill.
    if (cause == SessionEnd::Killed)
        _state.lastKill = _loop.clock().now();
    consider(triggerOf(cause), Origin::Event);
}

SnapshotTrigger WorkspacePersistence::triggerOf(SessionEnd cause) noexcept
{
    switch (cause)
    {
        case SessionEnd::Exited: return SnapshotTrigger::SessionExited;
        case SessionEnd::Requested: return SnapshotTrigger::SessionClosedByRequest;
        case SessionEnd::Killed: return SnapshotTrigger::SessionKilled;
    }
    return SnapshotTrigger::SessionExited;
}

ScheduleState WorkspacePersistence::scheduleState() const noexcept
{
    auto state = _state;
    if (_writer.health() == WriteHealth::Failing)
        state.snapshot = SnapshotState::Dirty;
    return state;
}

void WorkspacePersistence::consider(SnapshotTrigger trigger, Origin origin)
{
    if (_phase == Phase::Sealed)
        return;
    if (trigger != SnapshotTrigger::Periodic)
        _state.snapshot = SnapshotState::Dirty;
    auto const decision =
        decideSnapshotWrite(trigger, scheduleState(), _loop.clock().now(), _config.schedule);
    switch (decision.action)
    {
        case WriteAction::Skip: return;
        case WriteAction::Defer: deferUntil(decision.at); return;
        case WriteAction::WriteNow:
            if (origin == Origin::Timer)
            {
                writeSnapshot();
                return;
            }
            _loop.post([gate = _gate] {
                // A write posted twice, or overtaken by a periodic one, finds nothing changed.
                if (auto* const self = gate->self; self != nullptr && self->_phase == Phase::Open
                                                   && self->scheduleState().snapshot == SnapshotState::Dirty)
                    self->writeSnapshot();
            });
            return;
    }
}

void WorkspacePersistence::writeSnapshot()
{
    auto snapshot = captureWorkspace(
        _host,
        CaptureOptions { .scrollback = _config.scrollback, .scrollbackRows = _config.scrollbackRows },
        std::chrono::floor<std::chrono::seconds>(_wallClock.now()));
    // A realized restore's kept copy goes with the first write that succeeds after it.
    auto const after = _host.restoreOutcome() == RestoreOutcome::Realized ? AfterWrite::ConfirmRestored
                                                                          : AfterWrite::Nothing;
    _writer.submit(std::move(snapshot), after);
    _state.snapshot = SnapshotState::Clean;
}

void WorkspacePersistence::deferUntil(core::platform::SteadyTimePoint at)
{
    if (_deferred && !_deferred->settled() && _deferredAt <= at)
        return;
    _deferred.reset();
    _deferredAt = at;
    _deferred.emplace(_loop, at, &WorkspacePersistence::deferredExpired, this);
}

void WorkspacePersistence::armPeriodic()
{
    _periodic.reset();
    _periodic.emplace(
        _loop, _loop.clock().now() + _config.interval, &WorkspacePersistence::periodicExpired, this);
}

void WorkspacePersistence::periodicExpired(void* self)
{
    auto& persistence = *static_cast<WorkspacePersistence*>(self);
    // Re-armed from inside its own callback, which DeadlineTimer allows: it is settled before it is called.
    persistence.armPeriodic();
    persistence.consider(SnapshotTrigger::Periodic, Origin::Timer);
}

void WorkspacePersistence::deferredExpired(void* self)
{
    // Asked again as Periodic: a kill that arrived after the deadline was armed still pushes the write past
    // its grace, and a write made meanwhile leaves nothing to do.
    static_cast<WorkspacePersistence*>(self)->consider(SnapshotTrigger::Periodic, Origin::Timer);
}

void WorkspacePersistence::tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::tabMovedToWindow(
    vtworkspace::WindowId, vtworkspace::TabId, int, vtworkspace::WindowId, int)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneOrientationChanged(vtworkspace::TabId,
                                                  vtworkspace::PaneId,
                                                  vtworkspace::SplitState)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneSwapped(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneZoomChanged(vtworkspace::TabId, std::optional<vtworkspace::PaneId>)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::paneTreeRestructured(vtworkspace::TabId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::tabTitleChanged(vtworkspace::TabId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

void WorkspacePersistence::tabColorChanged(vtworkspace::TabId)
{
    consider(SnapshotTrigger::LayoutChanged, Origin::Event);
}

// ---------------------------------------------------------------------------
// Restoring

std::chrono::local_seconds localTimeOf(std::chrono::sys_seconds takenAt)
{
    // Not std::chrono::current_zone(): Apple's libc++ has no time zone database.
    return vtbackend::systemLocalTime(takenAt);
}

// ---------------------------------------------------------------------------
// DaemonPersistence

DaemonPersistence::DaemonPersistence(core::net::EventLoop& loop,
                                     SessionHost& host,
                                     SessionSnapshotStore const& store,
                                     std::optional<RestoreSource> restoredFrom,
                                     WorkspacePersistenceConfig config,
                                     core::platform::IWallClock const& wallClock,
                                     WriterThreading threading,
                                     DiagnosticSink const& report):
    _writer { store, threading, report }, _persistence { loop, host, _writer, std::move(config), wallClock }
{
    // Closed before the first write can confirm it: the writes only start with the first interval.
    if (!restoredFrom)
        return;
    auto const outcome = host.restoreOutcome();
    auto const pending = core::pathToUtf8(withSuffix(store.path(), ".restored"));
    if (auto const ended = store.endRestore(*restoredFrom, outcome); !ended)
        report(std::format("workspace restore: {} could not be closed: {}", pending, ended.error().detail));
    else if (outcome == RestoreOutcome::NothingRealized)
        report(*restoredFrom == RestoreSource::PendingRestore
                   ? std::format("workspace restore: again no pane of {} came back; set aside as {}",
                                 pending,
                                 core::pathToUtf8(withSuffix(store.path(), ".failed")))
                   : std::format("workspace restore: no pane came back; {} is kept for the next start",
                                 pending));
}

std::optional<WorkspaceRestore> prepareRestore(SessionSnapshotStore const& store,
                                               WorkspacePersistenceConfig const& config,
                                               DiagnosticSink const& report,
                                               core::platform::WallClockRef wall)
{
    auto loaded = store.load();
    auto const file =
        loaded.source == RestoreSource::PendingRestore ? withSuffix(store.path(), ".restored") : store.path();
    auto const name = core::pathToUtf8(file);
    switch (loaded.verdict)
    {
        case LoadVerdict::Absent: return std::nullopt;
        case LoadVerdict::Empty:
            report(std::format("workspace restore: the previous daemon left no workspace ({})", name));
            return std::nullopt;
        case LoadVerdict::SetAside:
            report(std::format("workspace restore: {} was damaged ({}); set aside as {}.corrupt",
                               name,
                               loaded.detail,
                               name));
            return std::nullopt;
        case LoadVerdict::Unsupported:
            report(std::format("workspace restore: {} was written by a newer release ({}); left in place",
                               name,
                               loaded.detail));
            return std::nullopt;
        case LoadVerdict::Unreadable:
            report(std::format("workspace restore: {} could not be read: {}", name, loaded.detail));
            return std::nullopt;
        case LoadVerdict::Restorable: break;
    }

    if (auto const begun = store.beginRestore(loaded.source); !begun)
        report(std::format(
            "workspace restore: {} could not be kept aside while restoring: {}", name, begun.error().detail));
    if (loaded.droppedHistories > 0)
        report(
            std::format("workspace restore: the history of {} pane(s) did not decode; they come back blank",
                        loaded.droppedHistories));
    auto snapshot = *std::move(loaded.snapshot);
    report(std::format("workspace restore: restoring {} window(s) from {}", snapshot.windows.size(), name));
    auto const divider = RestoreDivider {
        .sessionEndedAt = localTimeOf(snapshot.takenAt),
        .today = std::chrono::floor<std::chrono::days>(
            localTimeOf(std::chrono::floor<std::chrono::seconds>(wall.now()))),
    };
    return WorkspaceRestore { .snapshot = std::move(snapshot),
                              .rerunPrograms = config.rerunPrograms,
                              .divider = divider,
                              .source = loaded.source };
}

} // namespace vthost
