// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <vtpty/CommandCapture.hpp>

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/FileSystem.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtextension/CommandLine.hpp>
#include <vtextension/CommandPolicy.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/Placement.hpp>

namespace vtextension
{

/// One published frame of an extension. @see vtbackend::StatusLineExtensionSnapshot
using StatusLineSnapshot = vtbackend::StatusLineExtensionSnapshot;

/// Where a runner stands.
enum class RunnerState : uint8_t
{
    Starting,   ///< Instantiating and loading the extension.
    Running,    ///< The instance is up and receives events.
    BackingOff, ///< The instance faulted; a new one is created once the back-off has passed.
    Disabled,   ///< The extension faulted too often; it shows a notice and runs no more.
};

/// @param name What the extension is called in the configuration.
/// @param error Why it was disabled.
/// @return The one-line notice a disabled extension shows instead of its output.
[[nodiscard]] std::string disabledNotice(std::string_view name, ExtensionError error);

class Surface;

/// Draws a notice on a surface, so that a take-over status line shows it too.
/// @param notice The notice, one line.
/// @param surface Where to draw it; its previous frame is replaced.
/// @param generation The snapshot's generation.
/// @return The surface's frame, with `sgrText` exactly @p notice.
[[nodiscard]] StatusLineSnapshot drawnNotice(std::string const& notice,
                                             Surface& surface,
                                             uint64_t generation);

/// What a runner hands back when it is done with them, so that its owner releases them: the last
/// reference to an Engine or a CompiledExtension must not die on a scheduler's worker, where
/// destroying the engine would tear the runtime down under the worker's feet.
///
/// A runner allocates its RetiredExtension when it is constructed, on its owner's thread, so that
/// retiring on the lane allocates nothing; `next` lets a sink chain them without allocating either.
struct RetiredExtension
{
    std::shared_ptr<CompiledExtension> compiled; ///< Released first: it holds the engine too.
    std::shared_ptr<Engine> engine;
    std::unique_ptr<RetiredExtension> next; ///< For the sink's own list.
};

/// Takes what runners retire, and releases it on an owner's thread, never on the lane that retired it.
class IRetirementSink
{
  public:
    IRetirementSink() = default;
    IRetirementSink(IRetirementSink const&) = delete;
    IRetirementSink(IRetirementSink&&) = delete;
    IRetirementSink& operator=(IRetirementSink const&) = delete;
    IRetirementSink& operator=(IRetirementSink&&) = delete;
    virtual ~IRetirementSink() = default;

    /// Takes over a runner's module and engine. Called on the runner's lane; must not release them
    /// there, must not allocate, and must not block for long.
    /// @param retired What the runner is done with; never null.
    virtual void retire(std::unique_ptr<RetiredExtension> retired) noexcept = 0;
};

/// What one runner is: fixed for its whole life.
struct RunnerConfig
{
    std::string name;   ///< What the configuration calls the extension; what its notices say.
    std::string hostId; ///< The installation's host-assigned identity, which grants are keyed on.
    ConfigMap config;   ///< The extension's settings, passed to its `load`.
    Placement placement = Placement::Segment;               ///< Where its surface is shown.
    CommandPermission runCommand = CommandPermission::Deny; ///< What the user allows its commands.
    vtbackend::ImageSize cellPixelSize {};                  ///< One cell of the surface, in pixels.
    ExtensionLimits limits {};                              ///< What the extension may cost.
};

/// What a runner reaches outside itself. Every reference must outlive the scheduler's processing of
/// the runner's last job, which runs after the Runner is destroyed: in practice, these are the
/// process's (or the session service's) and the scheduler is destroyed (joined) before them.
///
/// The callbacks may touch shorter-lived objects: none is called once the Runner's destructor has
/// returned. Each is called on the runner's lane, must be cheap and must not throw; none may call
/// back into the runner. The destructor waits for a callback that is running, so a Runner must not be
/// destroyed while holding a lock that one of its callbacks takes. `callControl` is the exception: it
/// waits, so the destructor does not wait for it, and a call that began before may still be running
/// after the destructor returned.
struct RunnerServices
{
    IExtensionScheduler& scheduler;                 ///< The lane the runner lives on, its timers and clock.
    IGrantStore& grants;                            ///< Where command permissions are looked up.
    vtpty::CommandCapture& capture;                 ///< Runs the commands the extension asks for.
    ShellDialect shellDialect = NativeShellDialect; ///< The shell @ref capture runs command lines with.
    core::platform::IWallClock const& wallClock;    ///< The time `state` answers with.
    vtbackend::LocalTimeConverter const& localTime; ///< The time zone `state`'s UTC offset is read in.
    core::Environment const& environment;           ///< What the extension's private terminal reads.
    core::platform::FileSystem const& fileSystem;   ///< Where the private terminal looks paths up.
    IRetirementSink& retirement; ///< Where the runner's engine and module go when it is torn down.

    /// Called whenever latest(), state() or subscriptions() changed. Must not take the Terminal's state
    /// mutex (the same contract as vtbackend::StatusLineCommandCache's onRunCompleted).
    std::function<void()> onSnapshotPublished;

    /// Called once per runner the first time a command is refused as `PermissionPending`, so that the
    /// host can ask the user. The call itself has already failed; a later one succeeds once granted.
    std::function<void(std::string_view hostId, Manifest const& manifest)> onPermissionNeeded;

    /// Called for every message the extension logs, at most ExtensionLimits::maxLogsPerCall per call.
    std::function<void(std::string_view name, LogLevel level, std::string_view message)> onLog;

    /// Serves a control verb the extension calls, as its pane (IHostServices::callControl). The one callback
    /// that waits: it blocks the lane until the host answers, at most the timeout it is given, which is the
    /// rest of the guest call's deadline. Unlike the other callbacks it is copied under the gate and called
    /// outside it, so the destructor does not wait for it: the host may destroy the Runner while serving the
    /// call (an extension closing its own pane) and still answer it. The copy may therefore run after the
    /// Runner is gone, and must own or outlive what it reaches. Null where no control API is served: every
    /// verb is then `unsupported`.
    std::function<std::expected<std::string, vtcontrol::ControlError>(std::string_view method,
                                                                      std::string_view params,
                                                                      vtcontrol::ControlGrants granted,
                                                                      std::chrono::milliseconds timeout)>
        callControl {};
};

/// Runs one extension for one pane: owns its instance and private terminal, feeds it events, renders
/// it, publishes its frames, and recovers it from faults.
///
/// - **Threads.** Everything the runner does with its instance runs on one lane of its scheduler.
///   post(), resize() and the queries may be called from any thread and never wait for the extension.
/// - **Events.** Posted events queue (@see EventQueue) and are drained in batches: the updates of one
///   batch run back to back, followed by at most one render. Every event is recorded for `state`;
///   only the kinds the extension subscribed to are delivered to it.
/// - **Frames.** A render is due after load, after a resize, and when an update asks for one. Renders
///   are at least `minFrameInterval` apart, and a frame the extension requests is clamped to it. With
///   zero columns or rows nothing is rendered. Each render's output replaces the private terminal's
///   frame; the replies it produced are posted back to the extension as ReplyBytes.
/// - **Faults.** A fault (trap, deadline, memory, output size, ...) drops the instance; a new one is
///   created after a back-off that starts at `initialBackoff` and doubles with each fault in
///   `faultWindow`. The `faultsBeforeDisable`th fault in that window disables the extension, whose
///   frame becomes disabledNotice().
/// - **Commands.** `run-command` resolves through a CommandPolicy (no manifest: nothing is declared)
///   and runs through the CommandCapture for at most vtbackend::MaxCommandRuntime, blocking only this
///   runner's lane.
/// - **Control verbs.** Granted with the commands (CommandPolicy::controlConsent()), and asked for the same
///   way; served by RunnerServices::callControl, blocking this runner's lane for at most the rest of the
///   guest call's deadline.
class Runner
{
  public:
    /// Pins the runner to a lane and starts the extension there.
    /// @param engine The engine @p compiled was loaded on.
    /// @param compiled The extension's module.
    /// @param config What the runner is.
    /// @param services What it reaches.
    Runner(std::shared_ptr<Engine> engine,
           std::shared_ptr<CompiledExtension> compiled,
           RunnerConfig config,
           RunnerServices services);

    /// Detaches the runner: no callback is called once this returns, a command in flight is stopped,
    /// and the instance is destroyed on its lane. Never waits for the extension, only for a callback
    /// that is running -- but never for a control call in flight (RunnerServices::callControl).
    ~Runner();

    Runner(Runner const&) = delete;
    Runner(Runner&&) = delete;
    Runner& operator=(Runner const&) = delete;
    Runner& operator=(Runner&&) = delete;

    /// Hands an event to the extension. Never blocks; an event that cannot be queued is dropped and
    /// counted.
    /// @param event The event.
    void post(Event const& event) noexcept;

    /// @copydoc post(Event const&)
    void post(Event&& event) noexcept;

    /// Tells the extension its surface's new size, as a Resized event. Never blocks.
    /// @param size The size in cells; negative extents count as zero.
    void resize(vtbackend::PageSize size) noexcept;

    /// @return The frame last published; null before the first.
    [[nodiscard]] std::shared_ptr<StatusLineSnapshot const> latest() const noexcept;

    /// @return Where the runner stands.
    [[nodiscard]] RunnerState state() const noexcept;

    /// @return The event kinds the extension wants delivered; none until it is loaded, and none once
    ///         it is disabled.
    [[nodiscard]] EventKinds subscriptions() const noexcept;

    /// @return How many events post() and resize() have dropped over the runner's life, because its queue
    ///         was full: what a stalled extension costs instead of a waiting poster.
    [[nodiscard]] uint64_t dropCount() const noexcept;

    /// @return What the configuration calls the extension.
    [[nodiscard]] std::string const& name() const noexcept;

    /// @return Where the extension's surface is shown.
    [[nodiscard]] Placement placement() const noexcept;

  private:
    class Core;
    std::shared_ptr<Core> _core;
};

} // namespace vtextension
