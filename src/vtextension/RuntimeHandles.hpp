// SPDX-License-Identifier: Apache-2.0
//
// Owning handles on WAMR's opaque objects, so that headers need not pull wasm_export.h in.
#pragma once

#include <memory>

struct WASMModuleCommon;
struct WASMModuleInstanceCommon;
struct WASMExecEnv;

namespace vtextension::detail
{

/// Unloads a module.
struct ModuleUnloader
{
    void operator()(WASMModuleCommon* module) const noexcept;
};

/// Deinstantiates a module instance.
struct InstanceDeinstantiator
{
    void operator()(WASMModuleInstanceCommon* instance) const noexcept;
};

/// Destroys an execution environment.
struct ExecEnvDestroyer
{
    void operator()(WASMExecEnv* execEnv) const noexcept;
};

/// A loaded module.
using ModuleHandle = std::unique_ptr<WASMModuleCommon, ModuleUnloader>;

/// A module instance.
using InstanceHandle = std::unique_ptr<WASMModuleInstanceCommon, InstanceDeinstantiator>;

/// An execution environment.
using ExecEnvHandle = std::unique_ptr<WASMExecEnv, ExecEnvDestroyer>;

} // namespace vtextension::detail
