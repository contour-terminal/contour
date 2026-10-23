// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PageSize.hpp>

#include <core/platform/Clock.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// An extension's configuration: its settings as name and value pairs, in the user's order.
using ConfigMap = std::vector<std::pair<std::string, std::string>>;

/// One running extension: an instance of a CompiledExtension, sandboxed, speaking the statusline
/// world through typed calls.
///
/// - **Deadlines.** Every call is guarded by the engine's watchdog: `load` (with the module's own
///   initialization) by `loadDeadline`, `update` and `render` by `callDeadline`. A call that runs
///   past it is stopped and fails with `DeadlineExceeded`.
/// - **Memory.** Linear memory is capped at the engine's `memoryBytes`, the interpreter stack at the
///   engine's `stackBytes`: both are the engine's runtime's limits, fixed when it was created, and an
///   instance's own limits must name the same. A call into the guest that fails because its memory
///   refused to grow fails with `OutOfMemory`, on a backend that tells such a trap apart (otherwise
///   `Trapped`); so does a host call whose allocation in the guest failed that way. A refusal the guest
///   survived (its call returned) is not reported by the runtime, so a later failure in the same `load`,
///   `update` or `render` is `Trapped`.
/// - **Host calls are bounded.** What the host copies out of the guest for one host call is capped
///   at `maxHostCallBytes`; a guest that asks for more traps. No exception thrown in a host call
///   crosses into the runtime: it faults the call.
/// - **Host calls do not nest.** A host function called while another is in progress (from the
///   guest's `cabi_realloc`, which the host calls to store a result) faults the call as `Trapped`.
/// - **Faults are final.** A call that fails for any reason (trap, deadline, memory, an output over
///   `maxRenderBytes`, or a guest that breaks the Canonical ABI) leaves the instance unusable: every
///   later call fails with the same error without entering the guest. The host creates a new
///   instance to recover.
/// - **Threads.** Calls must not overlap. A thread other than the one that created the first engine
///   holds a thread guard of the engine's backend while it calls in; calling in without one is a
///   precondition violation. The host services are called on the calling thread, from inside the call.
class Instance final: private wasm::IHostContext
{
    struct PrivateTag
    {
    };

  public:
    /// Instantiates a compiled extension.
    /// @param engine The engine; it must be the one @p compiled was loaded on.
    /// @param compiled The module; the instance keeps it alive.
    /// @param services What the guest's imports call; it must outlive the instance.
    /// @param clock What measures how much of a call's deadline is spent; it must outlive the
    ///        instance. The watchdog itself enforces deadlines in real time.
    /// @param limits The limits this instance is held to. Its `memoryBytes` and `stackBytes` must be the
    ///        engine's (@p engine's limits()), whose runtime enforces them; the rest are the instance's.
    /// @return The instance; `OutOfMemory` if the module needs more memory than @p limits allow,
    ///         `InvalidComponent` if the runtime cannot instantiate it, or the error its module
    ///         initialization failed with.
    [[nodiscard]] static std::expected<std::unique_ptr<Instance>, ExtensionError> create(
        Engine& engine,
        CompiledExtension const& compiled,
        IHostServices& services,
        core::platform::IClock& clock,
        ExtensionLimits limits);

    /// Use create(); public only so that std::make_unique can reach it.
    Instance(PrivateTag,
             std::shared_ptr<CompiledExtension const> compiled,
             IHostServices& services,
             core::platform::IClock& clock,
             ExtensionLimits limits);

    ~Instance() override;

    Instance(Instance const&) = delete;
    Instance(Instance&&) = delete;
    Instance& operator=(Instance const&) = delete;
    Instance& operator=(Instance&&) = delete;

    /// Calls the extension's `load` with its configuration.
    /// @param config The extension's settings.
    /// @return The event kinds to deliver to it: those it subscribed to, and those always delivered.
    [[nodiscard]] std::expected<EventKinds, ExtensionError> load(ConfigMap const& config);

    /// Calls the extension's `update` with an event.
    /// @param event The event.
    /// @return Whether the extension wants `render` called.
    [[nodiscard]] std::expected<RenderRequest, ExtensionError> update(Event const& event);

    /// Calls the extension's `render` for a surface of @p size.
    /// @param size The surface's size in cells; negative extents count as zero.
    /// @return The VT bytes that draw the surface; `OutputTooLarge` if there are more than
    ///         `maxRenderBytes`.
    [[nodiscard]] std::expected<std::string, ExtensionError> render(vtpty::PageSize size);

    /// @return The error that made the instance unusable; `std::nullopt` while it is usable.
    [[nodiscard]] std::optional<ExtensionError> fault() const noexcept { return _fault; }

  private:
    class GuestMemory;
    struct HostCalls;
    friend struct HostCalls;

    /// An optional function export: one the guest may lack.
    using OptionalFunction = std::optional<wasm::ExportFunction>;

    /// The guest's exports, looked up once.
    struct Exports
    {
        wasm::ExportFunction load;
        wasm::ExportFunction update;
        wasm::ExportFunction render;
        wasm::ExportFunction realloc;
        OptionalFunction postLoad;
        OptionalFunction postUpdate;
        OptionalFunction postRender;
        OptionalFunction initialize; ///< A WASI reactor's initializer.
    };

    /// Whether a host call is running in this instance, which no other host call may enter.
    enum class HostCallState : uint8_t
    {
        Idle,
        InProgress,
    };

    /// Runs the world's host function at @p functionIndex of the table every runtime is created with
    /// (detail::statuslineHostFunctions()). Called by the runtime, synchronously, from inside a call
    /// into the guest.
    /// @return Nothing on success; otherwise the error that faults the call, which makes the guest trap.
    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept override;

    /// Instantiates the module and looks its exports up. Called once, by create().
    [[nodiscard]] std::expected<void, ExtensionError> instantiate();

    /// Runs @p body under a deadline of @p timeout, and records any failure as the instance's fault,
    /// an exception escaping @p body included (as `Trapped`). The deadline is disarmed however the
    /// body ends.
    template <typename Body>
    [[nodiscard]] auto guarded(std::chrono::milliseconds timeout, Body const& body) -> decltype(body());

    /// @return What names this instance, and its deadline's state, to the watchdog.
    [[nodiscard]] WatchToken watchToken() noexcept;

    /// @return Whether the watchdog found a deadline of this instance passed.
    [[nodiscard]] bool isDeadlineExpired() const noexcept;

    /// Calls an export with @p argv as its parameters and, on return, its results.
    [[nodiscard]] std::expected<void, ExtensionError> callExport(wasm::ExportFunction function,
                                                                 std::span<uint32_t> argv);

    /// Calls an export's post-return function, if the guest has one, with the export's results.
    [[nodiscard]] std::expected<void, ExtensionError> callPostReturn(OptionalFunction function,
                                                                     uint32_t result);

    /// @return Why the call that just failed in the guest failed, given what the runtime reported.
    [[nodiscard]] ExtensionError guestFailure(wasm::CallFailure failure) const noexcept;

    /// Records an error a host function hit: the call it faults fails with it.
    /// @return The error, for the runtime.
    [[nodiscard]] std::unexpected<ExtensionError> failHostCall(ExtensionError error) noexcept;

    std::shared_ptr<CompiledExtension const> _compiled;
    std::shared_ptr<Engine> _engine;
    IHostServices& _services;
    core::platform::IClock& _clock;
    ExtensionLimits _limits;
    /// Written by the watchdog, from its own thread, before it interrupts the instance. Once Expired it
    /// stays so: the instance is faulted for good by then.
    std::atomic<DeadlineState> _deadlineState = DeadlineState::Running;
    std::unique_ptr<wasm::IWasmInstance> _instance; ///< Null only until instantiate() succeeded.
    Exports _exports;
    std::unique_ptr<GuestMemory> _memory;
    std::optional<ExtensionError> _fault;
    std::optional<ExtensionError> _hostCallError; ///< What a host function failed with, this call.
    HostCallState _hostCall = HostCallState::Idle;
    core::platform::SteadyTimePoint _deadline; ///< When the current call's deadline expires, by _clock.
};

} // namespace vtextension
