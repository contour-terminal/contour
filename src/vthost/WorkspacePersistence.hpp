// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Keeping the workspace snapshot current: WorkspacePersistence watches a SessionHost and decides when to
/// write (SnapshotSchedule.hpp), SnapshotWriter writes off the loop thread, and prepareRestore() turns the
/// file a daemon finds at start into what SessionHostConfig::restore takes.
///
/// Threading. WorkspacePersistence lives on the loop thread: every event, timer and capture runs there, and
/// so does each call into SnapshotWriter. A Background SnapshotWriter owns one writer thread, which alone
/// encodes and writes the file, reports failures and confirms a restore. The two share only what the
/// writer's mutex guards (the one-slot pending snapshot and the submitted/written generations) and the
/// writer's health, an atomic. A snapshot crosses the boundary by value: the writer thread never reads the
/// host, the model or a terminal.

#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/SnapshotSchedule.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/WorkspacePersistenceConfig.hpp>

namespace vthost
{

/// Where SnapshotWriter writes.
enum class WriterThreading : std::uint8_t
{
    Inline = 0,     ///< On the thread that submits: tests.
    Background = 1, ///< On a writer thread of its own: a daemon, whose loop must not wait on a disk.
};

/// What a snapshot write does once the file is on disk.
enum class AfterWrite : std::uint8_t
{
    Nothing = 0,
    ConfirmRestored, ///< Drops the `.restored` file a realized restore kept (SessionSnapshotStore).
};

/// How the last write went.
enum class WriteHealth : std::uint8_t
{
    Healthy = 0, ///< It succeeded, or none was made.
    Failing,     ///< It failed: the file holds an older snapshot.
};

/// Where a one-line diagnostic goes. Called on the thread that wrote: a Background writer's own.
using DiagnosticSink = std::function<void(std::string_view)>;

/// Writes snapshots, the newest one winning, and reports a failing disk once until it recovers.
class SnapshotWriter
{
  public:
    /// @param store The file the snapshots go to. Borrowed: it must outlive the writer.
    /// @param threading Whether a writer thread of its own writes.
    /// @param report Where a failure, and the recovery after one, is told; thread-safe for Background.
    SnapshotWriter(SessionSnapshotStore const& store, WriterThreading threading, DiagnosticSink report);

    /// Flushes, then joins the writer thread.
    ~SnapshotWriter();

    SnapshotWriter(SnapshotWriter const&) = delete;
    SnapshotWriter& operator=(SnapshotWriter const&) = delete;
    SnapshotWriter(SnapshotWriter&&) = delete;
    SnapshotWriter& operator=(SnapshotWriter&&) = delete;

    /// Hands @p snapshot over to be written. Latest wins: a pending, unwritten snapshot is replaced, keeping
    /// a confirmation either asked for. Inline: written before this returns.
    /// @param snapshot The workspace.
    /// @param after What to do once it is on disk; a restore is confirmed once, by the first success.
    void submit(WorkspaceSnapshot snapshot, AfterWrite after = AfterWrite::Nothing);

    /// Returns once everything submitted is on disk (or failed).
    void flush();

    /// @return How the last write went. Any thread.
    [[nodiscard]] WriteHealth health() const noexcept { return _health.load(std::memory_order_acquire); }

  private:
    /// One submitted snapshot, waiting for the writer thread.
    struct Pending
    {
        WorkspaceSnapshot snapshot;
        AfterWrite after = AfterWrite::Nothing;
        std::uint64_t generation = 0; ///< Its submit's number: the writer has written everything up to it.
    };

    /// Whether the restore a write may confirm has been.
    enum class RestoreConfirmation : std::uint8_t
    {
        Unconfirmed = 0,
        Retrying, ///< Its removal failed once, and was reported.
        Confirmed,
    };

    /// Writes @p pending and reports what is new about how writes go. The writer thread (Inline: the caller).
    void write(Pending const& pending);

    /// The writer thread: writes what is pending until stopped with nothing pending.
    void run(std::stop_token const& stop);

    SessionSnapshotStore const& _store;
    DiagnosticSink _report;
    // Touched only by the thread that writes (see write()).
    std::optional<StoreStep> _failingStep {}; ///< The failure last reported, until a write succeeds.
    RestoreConfirmation _restore = RestoreConfirmation::Unconfirmed;
    std::atomic<WriteHealth> _health { WriteHealth::Healthy };
    // Guarded by _mutex: the hand-over between the submitting thread and the writer thread.
    std::mutex _mutex;
    std::condition_variable_any _changed; ///< Something was submitted, or written.
    std::optional<Pending> _pending {};
    std::uint64_t _submitted = 0; ///< The last submit's generation.
    std::uint64_t _written = 0;   ///< The generation of the last snapshot written (or failed).
    /// Last member: started once everything it reads exists, and joined before any of it goes. Not started
    /// for Inline.
    std::jthread _thread;
};

/// Keeps the snapshot file current as the workspace changes, as the schedule decides, and writes the last one
/// as the daemon ends.
///
/// Loop thread only (see the file's threading note). A write an event decides is posted, so a capture never
/// runs inside the host's fan-out; what the schedule must know at once (a kill) is recorded inline, so a
/// shutdown posted right after it already sees it. Holds no client ticket: it never keeps an idle daemon
/// alive.
class WorkspacePersistence final: public vtworkspace::ModelEvents, public SessionStreamEvents
{
  public:
    /// Subscribes to @p host and arms the periodic write. The workspace counts as changed until the first
    /// write.
    /// @param loop The loop whose clock and thread the timers run on.
    /// @param host The host whose workspace is written. Borrowed: it must outlive this.
    /// @param writer Where the snapshots go. Borrowed: it must outlive this.
    /// @param config The capture options and the timings.
    /// @param wallClock What a snapshot's time is read from.
    WorkspacePersistence(core::net::EventLoop& loop,
                         SessionHost& host,
                         SnapshotWriter& writer,
                         WorkspacePersistenceConfig config,
                         core::platform::IWallClock const& wallClock);

    ~WorkspacePersistence() override;

    WorkspacePersistence(WorkspacePersistence const&) = delete;
    WorkspacePersistence& operator=(WorkspacePersistence const&) = delete;
    WorkspacePersistence(WorkspacePersistence&&) = delete;
    WorkspacePersistence& operator=(WorkspacePersistence&&) = delete;

    /// Seals (later events are ignored, timers disarmed) and writes the last snapshot: the live workspace for
    /// Keep (unless the schedule says Skip), an empty one for Discard. Blocks until written. Loop thread
    /// only. Idempotent: a second call does nothing.
    /// @param disposition What the last snapshot holds.
    void finalize(ShutdownWorkspace disposition);

    // SessionStreamEvents
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void sessionScreenUpdated(vtworkspace::SessionId session) override;
    void sessionEnded(vtworkspace::SessionId session, SessionEnd cause) override;

    // vtworkspace::ModelEvents: every change is a layout change.
    void tabAdded(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabClosed(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabMoved(vtworkspace::WindowId window, vtworkspace::TabId tab, int fromIndex, int toIndex) override;
    void activeTabChanged(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void paneSplit(vtworkspace::TabId tab,
                   vtworkspace::PaneId splitNode,
                   vtworkspace::PaneId newLeaf) override;
    void paneClosed(vtworkspace::TabId tab,
                    vtworkspace::PaneId closed,
                    vtworkspace::PaneId survivor) override;
    void activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId leaf) override;
    void paneRatioChanged(vtworkspace::TabId tab, vtworkspace::PaneId splitNode, double ratio) override;
    void paneOrientationChanged(vtworkspace::TabId tab,
                                vtworkspace::PaneId splitNode,
                                vtworkspace::SplitState state) override;
    void paneSwapped(vtworkspace::TabId tab, vtworkspace::PaneId a, vtworkspace::PaneId b) override;
    void paneZoomChanged(vtworkspace::TabId tab, std::optional<vtworkspace::PaneId> zoomedLeaf) override;
    void paneTreeRestructured(vtworkspace::TabId tab) override;
    void tabTitleChanged(vtworkspace::TabId tab) override;
    void tabColorChanged(vtworkspace::TabId tab) override;

  private:
    /// Whether events still lead to writes.
    enum class Phase : std::uint8_t
    {
        Open = 0,
        Sealed, ///< finalize() ran.
    };

    /// Where a WriteNow came from, and so whether it may capture at once.
    enum class Origin : std::uint8_t
    {
        Event = 0, ///< Inside the host's fan-out: the write is posted.
        Timer,     ///< A timer of ours: written at once.
    };

    /// What posted jobs reach this through: revoked as this goes.
    struct Gate
    {
        WorkspacePersistence* self = nullptr;
    };

    /// Asks the schedule about @p trigger and acts on the answer.
    void consider(SnapshotTrigger trigger, Origin origin);

    /// The schedule's state as a write sees it: a failing writer leaves the file stale, so it counts as
    /// changed.
    [[nodiscard]] ScheduleState scheduleState() const noexcept;

    /// Captures the workspace and submits it; the workspace is then unchanged.
    void writeSnapshot();

    /// Arms the deferred deadline at @p at, unless an earlier one is armed.
    void deferUntil(core::platform::SteadyTimePoint at);

    void armPeriodic();
    static void periodicExpired(void* self);
    static void deferredExpired(void* self);

    /// The trigger a session's end feeds the schedule.
    [[nodiscard]] static SnapshotTrigger triggerOf(SessionEnd cause) noexcept;

    core::net::EventLoop& _loop;
    SessionHost& _host;
    SnapshotWriter& _writer;
    WorkspacePersistenceConfig _config;
    core::platform::IWallClock const& _wallClock;
    ScheduleState _state { .snapshot = SnapshotState::Dirty };
    Phase _phase = Phase::Open;
    std::shared_ptr<Gate> _gate;
    std::optional<core::net::DeadlineTimer> _periodic;
    std::optional<core::net::DeadlineTimer> _deferred;
    core::platform::SteadyTimePoint _deferredAt {}; ///< While _deferred is armed: when it fires.
    /// Last members: registered only once everything above is ready, and unregistered first.
    ScopedModelSubscription _modelSubscription;
    ScopedStreamSubscription _streamSubscription;
};

/// Everything a daemon keeps its workspace with once its host exists: it closes the restore the host was
/// given (SessionSnapshotStore::endRestore), then writes the workspace as it changes and as the daemon ends.
/// What runDaemon() builds after its host and before any listener binds; destroying it stops the writes,
/// the writer last.
class DaemonPersistence
{
  public:
    /// @param loop The daemon's loop.
    /// @param host The host whose workspace is kept. Borrowed: it must outlive this.
    /// @param store The snapshot file. Borrowed: it must outlive this.
    /// @param restoredFrom Which file the host's SessionHostConfig::restore came from; nullopt when the host
    ///        was given none.
    /// @param config The capture options and the timings.
    /// @param wallClock What a snapshot's time is read from. Borrowed: it must outlive this.
    /// @param threading Where the snapshots are written: Background for a daemon.
    /// @param report Where a one-line diagnostic goes; thread-safe for Background.
    DaemonPersistence(core::net::EventLoop& loop,
                      SessionHost& host,
                      SessionSnapshotStore const& store,
                      std::optional<RestoreSource> restoredFrom,
                      WorkspacePersistenceConfig config,
                      core::platform::IWallClock const& wallClock,
                      WriterThreading threading,
                      DiagnosticSink const& report);

    /// Writes the last snapshot, as workspaceFor(@p cause) says, and blocks until it is on disk. Loop
    /// thread only; a second call does nothing.
    /// @param cause What ends the daemon.
    void finalize(ShutdownCause cause) { _persistence.finalize(workspaceFor(cause)); }

  private:
    SnapshotWriter _writer;
    WorkspacePersistence _persistence; ///< After the writer, so it is destroyed first.
};

/// Loads the snapshot, logs its verdict through @p report, and returns what SessionHostConfig::restore takes.
/// A restorable snapshot is moved aside first (SessionSnapshotStore::beginRestore), so a restore never loses
/// it; the caller closes the restore with SessionSnapshotStore::endRestore once the host has realized it.
/// @param store The snapshot file.
/// @param config What a restored pane may re-run.
/// @param report Where the verdict's line goes (none for an absent file).
/// @param wall Today's date for the divider, as it reads in the daemon's local time zone.
/// @return The restore; nullopt when there is nothing to restore.
[[nodiscard]] std::optional<WorkspaceRestore> prepareRestore(SessionSnapshotStore const& store,
                                                             WorkspacePersistenceConfig const& config,
                                                             DiagnosticSink const& report,
                                                             core::platform::WallClockRef wall);

/// @param takenAt When a snapshot was taken.
/// @return @p takenAt in the daemon's local time zone; in UTC where none is known.
[[nodiscard]] std::chrono::local_seconds localTimeOf(std::chrono::sys_seconds takenAt);

} // namespace vthost
