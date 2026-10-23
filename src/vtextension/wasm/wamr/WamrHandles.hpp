// SPDX-License-Identifier: Apache-2.0
//
// Owning handles on WAMR's opaque objects, so that headers need not pull wasm_export.h in, and the null
// device the guests' WASI standard streams are bound to.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>

struct WASMModuleCommon;
struct WASMModuleInstanceCommon;
struct WASMExecEnv;

namespace vtextension::wasm::wamr::detail
{

/// Unloads a module.
struct ModuleUnloader
{
    /// Unloads @p module.
    void operator()(WASMModuleCommon* module) const noexcept;
};

/// Deinstantiates a module instance.
struct InstanceDeinstantiator
{
    /// Deinstantiates @p instance.
    void operator()(WASMModuleInstanceCommon* instance) const noexcept;
};

/// Destroys an execution environment.
struct ExecEnvDestroyer
{
    /// Destroys @p execEnv.
    void operator()(WASMExecEnv* execEnv) const noexcept;
};

/// A loaded module.
using ModuleHandle = std::unique_ptr<WASMModuleCommon, ModuleUnloader>;

/// A module instance.
using InstanceHandle = std::unique_ptr<WASMModuleInstanceCommon, InstanceDeinstantiator>;

/// An execution environment.
using ExecEnvHandle = std::unique_ptr<WASMExecEnv, ExecEnvDestroyer>;

/// Opens the null device, which guests' WASI standard streams are bound to: their writes reach nobody.
/// @return Its raw handle (a file descriptor, or a HANDLE on Windows); nothing if it cannot be opened.
[[nodiscard]] std::optional<int64_t> openNullDevice() noexcept;

/// Closes a handle openNullDevice() returned.
/// @param handle The handle to close.
void closeNullDevice(int64_t handle) noexcept;

} // namespace vtextension::wasm::wamr::detail
