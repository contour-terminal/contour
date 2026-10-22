// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/Placement.hpp>
#include <vtextension/Runner.hpp>

namespace vtextension
{

/// Hands out the process's Engine: the one that is alive, or a new one if none is.
class IEngineProvider
{
  public:
    IEngineProvider() = default;
    IEngineProvider(IEngineProvider const&) = delete;
    IEngineProvider(IEngineProvider&&) = delete;
    IEngineProvider& operator=(IEngineProvider const&) = delete;
    IEngineProvider& operator=(IEngineProvider&&) = delete;
    virtual ~IEngineProvider() = default;

    /// @return The engine; `RuntimeUnavailable` if none can be created.
    [[nodiscard]] virtual std::expected<std::shared_ptr<Engine>, ExtensionError> acquire() = 0;

    /// Releases, on the calling (owner) thread, what runners have retired since the last call.
    /// @see IRetirementSink
    virtual void releaseRetired() noexcept = 0;
};

/// The production IEngineProvider: shares one Engine among all its users while any of them holds
/// it, and creates it on the first acquire(), so that a process without extensions never starts the
/// runtime. It keeps only a weak reference: the engine goes away with its last user.
///
/// It is also the runners' IRetirementSink: what a runner retires on its lane waits here until an owner
/// thread calls acquire() or releaseRetired() (an ExtensionHost does on construction and destruction),
/// or the provider is destroyed. So an engine whose last pane closed lives until then.
class EngineProvider final: public IEngineProvider, public IRetirementSink
{
  public:
    /// @param limits What engines are created with.
    explicit EngineProvider(ExtensionLimits limits): _limits { limits } {}

    /// Releases what is still retired, on the destroying thread.
    ~EngineProvider() override;

    EngineProvider(EngineProvider const&) = delete;
    EngineProvider(EngineProvider&&) = delete;
    EngineProvider& operator=(EngineProvider const&) = delete;
    EngineProvider& operator=(EngineProvider&&) = delete;

    [[nodiscard]] std::expected<std::shared_ptr<Engine>, ExtensionError> acquire() override;
    void releaseRetired() noexcept override;
    /// Chains @p retired onto the list (allocation-free) under the provider's mutex.
    void retire(std::unique_ptr<RetiredExtension> retired) noexcept override;

  private:
    /// @return What was retired, taken out from under the lock.
    [[nodiscard]] std::unique_ptr<RetiredExtension> takeRetired() noexcept;

    /// Releases a list of retired extensions, one by one.
    static void release(std::unique_ptr<RetiredExtension> list) noexcept;

    ExtensionLimits _limits;
    std::mutex _mutex;
    std::weak_ptr<Engine> _engine;              ///< Guarded by _mutex.
    std::unique_ptr<RetiredExtension> _retired; ///< Newest first, linked by `next`; guarded by _mutex.
};

/// One extension a pane runs.
struct ExtensionSpec
{
    std::string name;              ///< What the configuration calls it; how segments name it.
    std::string hostId;            ///< Its host-assigned identity, which grants are keyed on.
    std::vector<std::byte> module; ///< The module's bytes; none if it could not be had.
    /// Why there are no module bytes; read only when there are none.
    ExtensionError moduleError = ExtensionError::ModuleUnavailable;
    ConfigMap config;                                       ///< Its settings.
    Placement placement = Placement::Segment;               ///< Where it is shown.
    CommandPermission runCommand = CommandPermission::Deny; ///< What the user allows its commands.
};

/// What one pane's extensions are.
struct ExtensionHostConfig
{
    std::vector<ExtensionSpec> extensions; ///< In the configuration's order.
    vtbackend::ImageSize cellPixelSize {}; ///< One cell of the status line, in pixels.
    ExtensionLimits limits {};             ///< What each extension may cost.
};

/// One configured extension with its module loaded, or the reason it is not.
struct PreparedExtension
{
    ExtensionSpec spec; ///< What it is; its module bytes have moved into @ref compiled.
    std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> compiled; ///< Its loaded module.
};

/// One pane's extensions with their modules loaded: what ExtensionHost starts without loading anything.
///
/// It holds the engine and the loaded modules, whose last reference must be released on an owner's
/// thread, where the engine was acquired (@see IRetirementSink): never let one die on another thread.
struct PreparedExtensions
{
    std::vector<PreparedExtension> extensions; ///< In the configuration's order.
    std::shared_ptr<Engine> engine;            ///< What the modules were loaded on; null if none was.
    vtbackend::ImageSize cellPixelSize {};     ///< One cell of the status line, in pixels.
    ExtensionLimits limits {};                 ///< What each extension may cost.
};

/// Loads the modules of @p config: what the slow part of starting a pane's extensions is. Runs on any
/// thread, which it prepares for the runtime itself; runs none of the modules' code.
/// @param config The pane's extensions.
/// @param engine What to load them on: one acquired on an owner's thread; ignored (and may be an error)
///        when no extension has module bytes.
/// @return The loaded extensions, each with its module or the reason it has none.
[[nodiscard]] PreparedExtensions prepareExtensions(
    ExtensionHostConfig config, std::expected<std::shared_ptr<Engine>, ExtensionError> engine);

/// @param prepared Prepared extensions.
/// @return What a runner retires, one node per module of @p prepared and one for its engine, linked by
///         `next`: how extensions that are never started reach an owner's thread through an IRetirementSink.
[[nodiscard]] std::unique_ptr<RetiredExtension> retirementOf(PreparedExtensions prepared);

/// What the extensions of a pane reach. @see RunnerServices for the lifetime contract.
struct ExtensionHostServices
{
    IEngineProvider& engines; ///< Where the engine comes from; asked only if there is an extension.
    RunnerServices runners;   ///< What every runner reaches.
};

/// The extensions of one pane: one Runner per configured extension.
///
/// Costs nothing without extensions: no engine is acquired, no lane assigned, no thread started; nor
/// when no extension has a module. An extension whose module is missing or cannot be loaded (or when no
/// engine can be created) shows the disabled notice in its place.
class ExtensionHost
{
  public:
    /// Loads every configured extension and starts it, on the calling (owner) thread.
    /// @param config The pane's extensions.
    /// @param services What they reach.
    ExtensionHost(ExtensionHostConfig config, ExtensionHostServices services);

    /// Starts extensions whose modules were loaded already (@see prepareExtensions), on the calling (owner)
    /// thread. Loads nothing, reads nothing.
    /// @param prepared The pane's extensions.
    /// @param services What they reach.
    ExtensionHost(PreparedExtensions prepared, ExtensionHostServices services);

    /// Detaches every runner (never waiting for one), then releases what earlier runners retired.
    ~ExtensionHost();

    ExtensionHost(ExtensionHost const&) = delete;
    ExtensionHost(ExtensionHost&&) = delete;
    ExtensionHost& operator=(ExtensionHost const&) = delete;
    ExtensionHost& operator=(ExtensionHost&&) = delete;

    /// @return The event kinds any of the extensions wants delivered.
    [[nodiscard]] EventKinds subscriptions() const noexcept;

    /// @return How many events the extensions have dropped, summed over them. @see Runner::dropCount()
    [[nodiscard]] uint64_t dropCount() const noexcept;

    /// Hands an event to every extension. Never blocks.
    /// @param event The event.
    void post(Event const& event) noexcept;

    /// Tells every extension the status line's new size. Never blocks.
    /// @param size The size in cells.
    void resize(vtbackend::PageSize size) noexcept;

    /// @param name What the configuration calls the extension.
    /// @return Its latest frame; null if there is no such extension or it has not rendered yet.
    [[nodiscard]] std::shared_ptr<StatusLineSnapshot const> snapshot(std::string_view name) const noexcept;

    /// @return What the configuration calls the first extension placed as Placement::TakeOver, loaded or
    ///         not (one that could not be loaded takes the status line over with its notice); none if no
    ///         extension is.
    [[nodiscard]] std::optional<std::string> takeOverName() const;

  private:
    /// One configured extension: running, or a notice why it could not be loaded.
    struct Entry
    {
        std::string name;
        Placement placement = Placement::Segment;         ///< Where it is shown.
        std::unique_ptr<Runner> runner;                   ///< Null if it could not be loaded.
        std::shared_ptr<StatusLineSnapshot const> notice; ///< What it shows when it could not be loaded.
    };

    IEngineProvider& _engines;
    std::vector<Entry> _entries;
};

} // namespace vtextension
