// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/GuestCall.hpp>
#include <vtextension/IDaemonHostServices.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// One running daemon-world extension: an instance of a CompiledExtension loaded on a World::Daemon
/// engine, sandboxed, speaking `terminal-extensions:daemon@0.1.0` through typed calls.
///
/// It keeps the status-line Instance's rules, with the daemon world's imports and exports:
///
/// - **Deadlines.** Every call is guarded by the watchdog it is given: a WASI reactor's `_initialize`
///   and `load` each by `loadDeadline`, `update` by `callDeadline`. A call that runs past it is stopped
///   and fails with `DeadlineExceeded`.
/// - **Durable host calls pause the deadline.** `store-set` and `store-remove` write durably; the
///   deadline bounds the guest's own work, not the disk, so it stands still across the service call and
///   resumes with what was left of it, as a status-line command does.
/// - **Imports not served yet.** Every import of DaemonImportRows whose row is
///   ImportAvailability::NotSupportedYet answers `err(not-supported-yet)` (`file-read`: its own
///   `file-error`'s case) and calls no service: a capability the guest does not have, never a fault.
/// - **Memory, host-call budget, nesting, faults, threads:** as Instance's. What the host copies out of
///   the guest for one host call is capped at `maxHostCallBytes`; host calls do not nest; a call that
///   fails for any reason leaves the instance unusable; calls must not overlap.
class DaemonInstance final: private wasm::IHostContext
{
    struct PrivateTag
    {
    };

  public:
    /// Instantiates a compiled daemon-world extension, and runs a WASI reactor's `_initialize`.
    /// @param compiled The module, loaded on a World::Daemon engine; the instance keeps it alive.
    /// @param services What the guest's served imports call; it must outlive the instance.
    /// @param watchdog What enforces the deadlines: the module's engine in production. It must outlive
    ///        the instance.
    /// @param limits The limits this instance is held to. Its `memoryBytes` and `stackBytes` must be the
    ///        engine's, whose runtime enforces them; the rest are the instance's.
    /// @return The instance; `OutOfMemory` if the module needs more memory than @p limits allow,
    ///         `InvalidComponent` if the runtime cannot instantiate it, or the error `_initialize` failed
    ///         with.
    [[nodiscard]] static std::expected<std::unique_ptr<DaemonInstance>, ExtensionError> create(
        std::shared_ptr<CompiledExtension const> compiled,
        IDaemonHostServices& services,
        IDeadlineWatchdog& watchdog,
        ExtensionLimits limits);

    /// Use create(); public only so that std::make_unique can reach it.
    DaemonInstance(PrivateTag,
                   std::shared_ptr<CompiledExtension const> compiled,
                   IDaemonHostServices& services,
                   IDeadlineWatchdog& watchdog,
                   ExtensionLimits limits);

    ~DaemonInstance() override;

    DaemonInstance(DaemonInstance const&) = delete;
    DaemonInstance(DaemonInstance&&) = delete;
    DaemonInstance& operator=(DaemonInstance const&) = delete;
    DaemonInstance& operator=(DaemonInstance&&) = delete;

    /// Calls the extension's `load`, under `loadDeadline`.
    /// @param configJson The extension's configuration, as JSON.
    /// @param info What the host tells the extension once: its roots, the home directory, the UTC offset.
    /// @return Nothing once `load` returned; otherwise why the call failed, which faults the instance.
    [[nodiscard]] std::expected<void, ExtensionError> load(std::string_view configJson,
                                                           DaemonHostInfo const& info);

    /// Calls the extension's `update`, under `callDeadline`.
    /// @param hint Why it is called.
    /// @return Nothing once `update` returned; otherwise why the call failed, which faults the instance.
    [[nodiscard]] std::expected<void, ExtensionError> update(DaemonHint hint);

    /// @return The error that made the instance unusable; `std::nullopt` while it is usable.
    [[nodiscard]] std::optional<ExtensionError> fault() const noexcept { return _guest.fault(); }

  private:
    struct HostCalls;
    friend struct HostCalls;

    /// The guest's exports, looked up once; `cabi_realloc` is the GuestCall's.
    struct Exports
    {
        wasm::ExportFunction load;
        wasm::ExportFunction update;
    };

    /// Runs the daemon world's import at @p functionIndex of daemonHostFunctions(), which is its
    /// DaemonImport. Called by the runtime, synchronously, from inside a call into the guest.
    /// @return Nothing on success; otherwise the error that faults the call, which makes the guest trap.
    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept override;

    /// Instantiates the module, looks its exports up, and runs `_initialize`. Called once, by create().
    [[nodiscard]] std::expected<void, ExtensionError> instantiate();

    std::shared_ptr<CompiledExtension const> _compiled;
    IDaemonHostServices& _services;
    ExtensionLimits _limits;
    detail::GuestCall _guest; ///< The instance, its deadline and its faults.
    Exports _exports;
};

} // namespace vtextension
