// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/LocalTime.hpp>

#include <core/platform/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonLimits.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionPresence.hpp>
#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/ExtensionStore.hpp>
#include <vtextension/FileReadService.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/PaneService.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/Runner.hpp>
#include <vtextension/ServedTabService.hpp>

namespace vtextension
{

/// What one daemon-world runner is: fixed for its whole life.
struct DaemonRunnerConfig
{
    std::string name;                             ///< What the configuration calls the extension.
    std::string configJson;                       ///< Its settings, as JSON, passed to every `load`.
    std::vector<ResolvedRoot> roots;              ///< Its roots, resolved: what `load` tells it of the host.
    std::string home;                             ///< The user's home directory, as `load` tells it.
    ExtensionLimits limits = daemonWorldLimits(); ///< What it may cost; the engine's memory and stack caps.
    DurableBudget durableBudget {};               ///< What one callback may spend in durable host calls.
    DaemonRunGeneration generation = 0; ///< Which run of the extension this is: what it tells its presence.
    std::vector<SelfProgram> selfPrograms = {}; ///< What `load` tells it the host runs as its own executable.
};

/// What a daemon-world runner reaches outside itself. Every reference must outlive the scheduler's processing
/// of the runner's last job, which runs after the runner is destroyed. `onLog` may touch shorter-lived
/// objects: it is not called once the runner's destructor has returned, which waits for a call in progress.
struct DaemonServices
{
    IProcessService& processes;     ///< The extension's declared processes.
    IExtensionStore& store;         ///< Its durable store.
    IExtensionPresence& presence;   ///< Where its summary is shown.
    IServedTabService& tabs;        ///< The tabs it serves, and the actions its user invoked.
    IFileReadService& files;        ///< The files it may read.
    IPaneService& panes;            ///< The agent panes it opens.
    IExtensionScheduler& scheduler; ///< The lane it lives on, its timers, and its monotonic clock.
    core::platform::IWallClock const& wallClock; ///< The wall-clock time `now` answers with.
    /// The time zone: `load` tells the UTC offset at the wall-clock time of each load.
    vtbackend::LocalTimeConverter const& localTime;
    IRetirementSink& retirement; ///< Where its engine and module go when it is torn down.

    /// Called for every message the extension logs (at most ExtensionLimits::maxLogsPerCall per call), and
    /// once with `"<name> disabled: <reason>"` when it is disabled. Called on the lane; must be cheap.
    std::function<void(std::string_view name, LogLevel level, std::string_view message)> onLog;
};

/// Where a daemon-world runner stands.
enum class DaemonRunnerState : uint8_t
{
    Starting,   ///< Instantiating and loading the extension.
    Running,    ///< The instance is up and receives hints.
    BackingOff, ///< The instance faulted; a new one is created once the back-off has passed.
    Disabled,   ///< The extension faulted too often; it runs no more until restart().
};

/// A daemon-world runner's state, read at once.
struct DaemonRunnerStatus
{
    DaemonRunnerState state = DaemonRunnerState::Starting; ///< Where it stands.
    std::optional<ExtensionError> lastFault;               ///< Its latest fault, if it ever faulted.
    std::size_t faultsInWindow = 0; ///< Its faults within ExtensionLimits::faultWindow at the latest one.
};

/// Runs one daemon-world extension, with no session or window: owns its instance, serves its imports, feeds
/// it hints, and recovers it from faults.
///
/// - **Threads.** Everything the runner does with its instance runs on one lane of its scheduler. hint(),
///   restart() and the queries may be called from any thread and never wait for the extension.
/// - **Hints.** One coalescing slot per DaemonHint: a hint raised while one of its kind is pending is the
///   same hint. Pending hints are delivered in DaemonHint's order, one `update` per kind; hints raised while
///   no instance runs wait for the next one.
/// - **`wake`** arms one lane timer, replacing the previous one, after `max(after, MinWakeInterval)`.
/// - **`now`** reads the scheduler's clock and the wall clock.
/// - **Processes and store** forward to their services; their errors reach the guest through
///   ProcessErrorToHostError and StoreErrorToHostError. Durable calls are bounded per callback by the
///   config's DurableBudget.
/// - **Presence.** The runner tells its presence where the extension stands, with its config's generation:
///   Running as it is made and when a disabled one is restarted, Disabled once it faulted too often.
/// - **Tabs and actions** (`tab-open` … `tab-write`, `actions-take`) forward to the IServedTabService, which
///   answers in the WIT's own host-error.
/// - **`summary-set`** refuses a summary over MaxSummaryShortChars characters or a detail over
///   MaxSummaryDetailBytes bytes with `limit`, publishing nothing; otherwise it publishes both through
///   neutralizedForDisplay(), the detail line by line so that its line breaks remain.
/// - **Faults** drop the instance; a new one is created after a back-off that starts at `initialBackoff`
///   and doubles with each fault in `faultWindow`. The `faultsBeforeDisable`th fault in that window
///   disables the extension and logs why.
class DaemonExtensionRunner
{
  public:
    /// Pins the runner to a lane and posts the extension's first load there.
    /// @param engine The World::Daemon engine @p compiled was loaded on; it enforces the deadlines.
    /// @param compiled The extension's module.
    /// @param config What the runner is; its limits' memory and stack caps must be the engine's.
    /// @param services What it reaches.
    DaemonExtensionRunner(std::shared_ptr<Engine> engine,
                          std::shared_ptr<CompiledExtension const> compiled,
                          DaemonRunnerConfig config,
                          DaemonServices services);

    /// Detaches the runner: no callback is called once this returns, and the instance is destroyed (and the
    /// engine retired) by the last job on its lane. Never waits for the extension, only for a callback that
    /// is running.
    ~DaemonExtensionRunner();

    DaemonExtensionRunner(DaemonExtensionRunner const&) = delete;
    DaemonExtensionRunner(DaemonExtensionRunner&&) = delete;
    DaemonExtensionRunner& operator=(DaemonExtensionRunner const&) = delete;
    DaemonExtensionRunner& operator=(DaemonExtensionRunner&&) = delete;

    /// Raises a hint for the extension. Never blocks.
    /// @param hint Why it is to be called.
    void hint(DaemonHint hint) noexcept;

    /// Starts a disabled extension again, with a fresh fault window. Does nothing unless it is Disabled.
    void restart() noexcept;

    /// @return Where the runner stands.
    [[nodiscard]] DaemonRunnerStatus status() const noexcept;

    /// @return What the configuration calls the extension.
    [[nodiscard]] std::string const& name() const noexcept;

  private:
    class Core;
    std::shared_ptr<Core> _core;
};

} // namespace vtextension
