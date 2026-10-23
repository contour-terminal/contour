// SPDX-License-Identifier: Apache-2.0
//
// Internal to the WAMR backend: the Runtime, the Module and Instance factories, and how host functions
// are registered with WAMR.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <wasm_export.h>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::wamr
{

class HostRegistrations;

/// One hold on WAMR's process-wide runtime, with the host functions registered for the modules it
/// loads, and the null device their WASI standard streams are bound to.
class WamrRuntime final: public IWasmRuntime, public std::enable_shared_from_this<WamrRuntime>
{
    struct PrivateTag
    {
    };

  public:
    /// Brings the runtime up (or joins it), opens a null device and registers @p hostFunctions.
    /// @param limits The limits every instance of the runtime is held to.
    /// @param hostFunctions The host functions modules may import.
    /// @return The runtime; `RuntimeUnavailable` if WAMR could not initialize, the null device could
    ///         not be opened, or WAMR refused the host functions.
    [[nodiscard]] static std::expected<std::shared_ptr<WamrRuntime>, ExtensionError> create(
        RuntimeLimits limits, std::span<HostFunction const> hostFunctions);

    /// Use create(); public only so that std::make_shared can reach it.
    WamrRuntime(PrivateTag,
                RuntimeLimits limits,
                int64_t nullDevice,
                std::unique_ptr<HostRegistrations> hostRegistrations) noexcept;

    /// Unregisters the host functions, closes the null device, then releases the hold on the runtime.
    ~WamrRuntime() override;

    WamrRuntime(WamrRuntime const&) = delete;
    WamrRuntime(WamrRuntime&&) = delete;
    WamrRuntime& operator=(WamrRuntime const&) = delete;
    WamrRuntime& operator=(WamrRuntime&&) = delete;

    [[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> load(
        std::vector<std::byte> bytes) override;

    /// @return The limits every instance of the runtime is held to.
    [[nodiscard]] RuntimeLimits const& limits() const noexcept { return _limits; }

    /// @return The raw handle of the null device the guests' WASI standard streams are bound to.
    [[nodiscard]] int64_t nullDevice() const noexcept { return _nullDevice; }

  private:
    RuntimeLimits _limits;
    int64_t _nullDevice;                                   ///< Owned; closed on destruction.
    std::unique_ptr<HostRegistrations> _hostRegistrations; ///< Unregistered before the runtime is released.
};

/// Loads a module into WAMR and describes it.
/// @param runtime The runtime it is loaded on; the module keeps it alive.
/// @param bytes The module's bytes; the module keeps them, as WAMR refers into them.
/// @return The module; `InvalidComponent` if the bytes are not a valid core module.
[[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> loadModule(
    std::shared_ptr<WamrRuntime const> runtime, std::vector<std::byte> bytes);

/// Instantiates a loaded module under the limits of the runtime it was loaded on.
/// @param owner The module being instantiated; the instance keeps it (and with it the runtime) alive.
/// @param module WAMR's handle on @p owner's module.
/// @param limits The limits of the runtime @p owner was loaded on.
/// @param exports @p owner's exports.
/// @param context What the instance's host function calls reach.
/// @return The instance; `OutOfMemory` if a memory's initial size exceeds the cap, `InvalidComponent`
///         if WAMR cannot instantiate the module, `RuntimeUnavailable` on a resource failure.
[[nodiscard]] std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiateModule(
    std::shared_ptr<IWasmModule const> owner,
    WASMModuleCommon* module,
    RuntimeLimits const& limits,
    std::span<ExportDescription const> exports,
    IHostContext& context);

/// The most i32 parameters a host function can take: a trampoline exists for each count up to it.
inline constexpr auto MaxHostParameters = uint32_t { 5 };

/// The most host functions one runtime can be created with.
inline constexpr auto MaxHostFunctions = std::size_t { 32 };

/// What a host function is registered with WAMR as, in the raw calling convention.
///
/// Every part lives as long as the process: WAMR's native registry is process-wide and is searched by
/// name, newest registration first, so a module may link against another runtime's registration of the
/// same host function, and keep what it linked to after that runtime has unregistered it.
struct HostNative
{
    void* function = nullptr;        ///< The trampoline for the host function's parameter count.
    char const* signature = nullptr; ///< Its signature in WAMR's notation: `(` an `i` per i32 `)`.
    void* attachment = nullptr;      ///< Names the host function's index to the trampoline.
};

/// @return What the host function at @p index of a runtime's table, taking @p parameterCount i32s, is
///         registered as; nothing if it takes more than MaxHostParameters, or @p index is not below
///         MaxHostFunctions.
[[nodiscard]] std::optional<HostNative> hostNative(uint32_t parameterCount, std::size_t index) noexcept;

namespace detail
{
    /// Installs the backend's handler of WAMR's one process-wide memory-growth callback, once per
    /// process. It marks the failing call of an instance of this backend as MemoryGrowthRefused; the
    /// backend is the callback's only user.
    void installMemoryGrowthHandler();
} // namespace detail

} // namespace vtextension::wasm::wamr
