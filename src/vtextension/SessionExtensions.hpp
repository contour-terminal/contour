// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <vtpty/CommandCapture.hpp>

#include <core/Environment.hpp>
#include <core/async/IExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/AtomicSharedPtr.hpp>
#include <vtextension/ExtensionComposition.hpp>
#include <vtextension/ExtensionGrantStore.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/StatusLineAdapter.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// What every pane's extensions reach. Each must outlive every pane's extensions; the build executor must be
/// destroyed (joined) before the retirement sink and what the builds read, and the scheduler before what its
/// jobs reach: the grants, the capture, the environment, the filesystem and the retirement sink.
/// @see RunnerServices
struct SessionExtensionServices
{
    IEngineProvider& engines;                       ///< Where the engine comes from.
    IRetirementSink& retirement;                    ///< Where runners and abandoned builds hand engines back.
    IExtensionScheduler& scheduler;                 ///< The lanes the runners live on.
    core::async::IExecutor& builds;                 ///< Where modules are found, read and loaded.
    IGrantStore& grants;                            ///< Where command permissions are looked up.
    vtpty::CommandCapture& capture;                 ///< Runs the commands extensions ask for.
    core::platform::IWallClock const& wallClock;    ///< The time `state` answers with.
    vtbackend::LocalTimeConverter const& localTime; ///< The time zone `state`'s UTC offset is read in.
    core::Environment const& environment;           ///< What the extensions' private terminals read.
    core::platform::FileSystem const& fileSystem;   ///< Where those terminals look paths up.
    ExtensionModuleLookup lookup;                   ///< Where modules are found.
    ExtensionLimits limits;                         ///< What each extension may cost.
};

/// What a pane hears from its extensions.
struct SessionExtensionCallbacks
{
    /// An extension's command was refused as pending; the session asks the user. Called on the extension's
    /// lane: cheap, not throwing, and never waiting for the thread that destroys the pane.
    std::function<void(std::string_view hostId, Manifest const& manifest)> onPermissionNeeded;

    /// An extension logged a message. Called on its lane, as onPermissionNeeded is.
    std::function<void(std::string_view name, LogLevel level, std::string_view message)> onLog;

    /// Other extensions are running now: the terminal must be told
    /// (vtbackend::Terminal::statusLineExtensionsReplaced()). Called on the owner's thread, holding no lock.
    std::function<void()> onReplaced;
};

/// Where work finished on another thread is handed back to the thread that owns a pane.
class IOwnerThread
{
  public:
    IOwnerThread() = default;
    IOwnerThread(IOwnerThread const&) = delete;
    IOwnerThread(IOwnerThread&&) = delete;
    IOwnerThread& operator=(IOwnerThread const&) = delete;
    IOwnerThread& operator=(IOwnerThread&&) = delete;
    virtual ~IOwnerThread() = default;

    /// Runs @p job on the owner's thread later, or never once the owner is gone. Callable from any thread.
    /// @param job What to run there.
    virtual void post(std::function<void()> job) = 0;
};

/// A pane's running extensions as one IStatusLineExtensions: the host and its adapter.
class HostedExtensions
{
  public:
    /// Starts @p prepared on the calling (owner) thread; loads nothing.
    /// @param prepared The pane's extensions, their modules loaded.
    /// @param services What they reach.
    HostedExtensions(PreparedExtensions prepared, ExtensionHostServices services):
        _host { std::move(prepared), std::move(services) }
    {
    }

    /// @return The terminal's view of the host.
    [[nodiscard]] vtbackend::IStatusLineExtensions& extensions() noexcept { return _adapter; }

  private:
    ExtensionHost _host;
    StatusLineAdapter _adapter { _host };
};

/// The IStatusLineExtensions a terminal is built with, which forwards to the pane's current extensions: a
/// configuration reload replaces those without the terminal, which keeps its pointer for life.
///
/// Calls arrive from any thread (the parser and render threads under the terminal's lock, an extension's
/// worker without it) and take no lock but the pointer's own. replace() publishes the new extensions, then
/// waits for the calls still in the old ones -- each one non-blocking -- before it hands them back, so that
/// the last reference to a host is never dropped on a worker, which would destroy a runner on its own lane.
class ReplaceableStatusLineExtensions final: public vtbackend::IStatusLineExtensions
{
  public:
    /// @param initial What calls reach first; null for nothing.
    explicit ReplaceableStatusLineExtensions(std::shared_ptr<HostedExtensions> initial);

    /// Forwards to @p next from now on.
    /// @param next What calls reach from now on; null for nothing.
    /// @return What they reached before, which no call reaches any more; the caller destroys it, holding no
    ///         lock a runner's callback takes.
    [[nodiscard]] std::shared_ptr<HostedExtensions> replace(std::shared_ptr<HostedExtensions> next);

    /// @return Whether it forwards to nothing. Read on the owner's thread, or on the render thread while the
    ///         owner's thread is blocked in Qt's scene-graph sync (TerminalDisplay::createRenderer); written
    ///         on the owner's thread only.
    [[nodiscard]] bool empty() const noexcept { return _owned == nullptr; }

    [[nodiscard]] vtbackend::StatusLineEventKinds subscriptions() const noexcept override;
    void post(vtbackend::StatusLineEvent event) noexcept override;
    void resize(vtbackend::PageSize statusLineSize) noexcept override;
    [[nodiscard]] std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> snapshot(
        std::string_view name) const noexcept override;
    [[nodiscard]] std::optional<std::string> takeOverName() const noexcept override;

  private:
    /// Owned here as well, so that the published copy is never the last one. GUI thread only.
    std::shared_ptr<HostedExtensions> _owned;
    AtomicSharedPtr<HostedExtensions> _current;
};

/// One pane's status-line extensions, and the two things its terminal is built with: the forwarder it
/// reads, and the sink their workers tell of a published frame through. Starts empty: nothing runs, nothing
/// is subscribed, and the extension runtime is not needed until the first rebuild() that names extensions.
///
/// **Building.** rebuild() returns at once: the modules are found, read and loaded on the build executor,
/// then handed back to the owner's thread, where they replace the running extensions and onReplaced is
/// called. A rebuild supersedes one still in flight, whose result is dropped. What a build holds (the engine
/// and the loaded modules) is released on the owner's thread however the build ends: when it is never
/// handed back (the pane went away) it goes to the retirement sink.
///
/// **Lifetime.** Declared before the pane's Terminal, so that it outlives it: ~Terminal revokes the sink,
/// after which a frame published reaches nobody; then this destroys the extensions (each runner detaches
/// without waiting for its extension), and the sink last. A build still in flight holds nothing of this:
/// only the owner thread's handle, which drops what it is handed once the pane is gone.
class SessionExtensions
{
  public:
    /// @param callbacks What the pane hears from its extensions.
    /// @param owner Where builds are handed back to; shared with the builds in flight.
    SessionExtensions(SessionExtensionCallbacks callbacks, std::shared_ptr<IOwnerThread> owner);

    ~SessionExtensions();
    SessionExtensions(SessionExtensions const&) = delete;
    SessionExtensions(SessionExtensions&&) = delete;
    SessionExtensions& operator=(SessionExtensions const&) = delete;
    SessionExtensions& operator=(SessionExtensions&&) = delete;

    /// @return What the pane's terminal is built with. Its user-variable bound is the default limits': the
    ///         terminal is built before any rebuild() hands over the services, and every runtime runs with
    ///         the default ExtensionLimits.
    [[nodiscard]] vtbackend::StatusLineExtensionServices terminalServices() noexcept
    {
        return { .extensions = _forwarder,
                 .published = _published,
                 .maxUserVars = ExtensionLimits {}.maxUserVars };
    }

    /// Starts building @p declared's extensions, to run in place of the current ones once built. Returns at
    /// once. Without extensions, stops the running ones right away and needs no services.
    /// @param declared What the profile declares.
    /// @param cellPixelSize One cell of the status line, in pixels; empty while unknown.
    /// @param services What the extensions reach; must outlive them. Unused when @p declared has none.
    void rebuild(DeclaredExtensions declared,
                 vtbackend::ImageSize cellPixelSize,
                 std::optional<SessionExtensionServices> services);

    /// @return Whether no extension runs: true too while the first build for a declared set is still in
    /// flight,
    ///         as nothing is swapped in before it lands. Owner's thread only.
    [[nodiscard]] bool empty() const noexcept { return _forwarder.empty(); }

    /// @return The event kinds the running extensions want.
    [[nodiscard]] vtbackend::StatusLineEventKinds subscriptions() const noexcept
    {
        return _forwarder.subscriptions();
    }

  private:
    /// Runs a finished build on the owner's thread, unless a later rebuild superseded it.
    void adopt(uint64_t generation, PreparedExtensions prepared, SessionExtensionServices services);

    /// Puts @p next in place of the running extensions and tells the owner.
    void install(std::shared_ptr<HostedExtensions> next);

    SessionExtensionCallbacks _callbacks;
    std::shared_ptr<IOwnerThread> _owner;
    uint64_t _generation = 0;                    ///< Of the latest rebuild(); owner's thread only.
    vtbackend::StatusLinePublishSink _published; ///< Outlives the extensions, which publish through it.
    ReplaceableStatusLineExtensions _forwarder { nullptr };
};

/// What the application's extension runtime is.
struct ExtensionRuntimeConfig
{
    std::filesystem::path grantsFile;                      ///< @see ExtensionGrantsFileName
    std::vector<std::filesystem::path> bundledDirectories; ///< Where bundled extensions are looked for.
    std::size_t workerCount = 1;                           ///< @see extensionWorkerCount
    ExtensionLimits limits {};                             ///< What each extension may cost.
};

/// What the application's extension runtime borrows; each must outlive it.
struct ExtensionRuntimeServices
{
    wasm::IWasmBackend& backend;                    ///< Where engines and lane guards come from.
    vtpty::CommandCapture& capture;                 ///< Runs the commands extensions ask for.
    core::platform::IWallClock const& wallClock;    ///< The time `state` answers with.
    vtbackend::LocalTimeConverter const& localTime; ///< The time zone `state`'s UTC offset is read in.
    core::Environment const& environment;           ///< What the extensions' private terminals read.
    core::platform::FileSystem const& fileSystem;   ///< Where modules, grants and paths are read.
};

/// The application's share of status-line extensions: the grants, the engine, the worker pool every pane's
/// extensions run on, and the thread their modules are read and loaded on. Created the first time a pane has
/// extensions, so that a process without any reads no grants file, starts no runtime and no thread.
///
/// **Lifetime.** Owned by the application, declared after what it borrows (the backend, the capture, the
/// environment, the filesystem) and before the session manager, whose sessions -- and so every runner -- are
/// destroyed first. Its own members are declared so that the build thread is joined first (a build blocked on
/// a hung filesystem delays that), then the pool, whose workers run the jobs still posted (each runner's last
/// job destroys its instance on its lane and retires the engine), before the engine provider releases what
/// was retired, on this thread, and before the grants go.
class ExtensionRuntime
{
  public:
    /// @param config What it is.
    /// @param services What it borrows.
    ExtensionRuntime(ExtensionRuntimeConfig config, ExtensionRuntimeServices services);

    /// @return What a pane's extensions reach.
    [[nodiscard]] SessionExtensionServices sessionServices() noexcept;

    /// @return Where the user's answers are kept.
    [[nodiscard]] IGrantStore& grants() noexcept { return _grants; }

    /// @return How many worker threads run; none until the first extension is started.
    [[nodiscard]] std::size_t runningWorkers() const noexcept { return _pool.runningWorkers(); }

    /// Releases, on the calling (GUI) thread, the engines and modules runners retired since.
    void releaseRetired() noexcept { _engines.releaseRetired(); }

    /// Joins the build thread, then the workers, as destruction does, but keeps everything they reach --
    /// the grants, the engine provider each retires to -- alive: a build it ends hands its result to the
    /// owner's thread, which drops it there while the provider still exists. Call once every pane's
    /// extensions are destroyed, so each runner's last job is posted already; no pane may be built after.
    /// Idempotent.
    void stop() noexcept
    {
        _builds.reset();
        _pool.stop();
    }

  private:
    ExtensionRuntimeServices _services;
    std::vector<std::filesystem::path> _bundledDirectories;
    ExtensionLimits _limits;
    ExtensionGrantStore _grants;
    EngineProvider _engines;
    ExtensionWorkerPool _pool;
    std::unique_ptr<core::async::IExecutor> _builds; ///< Last: joined first.
};

} // namespace vtextension
