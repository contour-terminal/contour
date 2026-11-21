// SPDX-License-Identifier: Apache-2.0
//
// Internal to vtextension: the host functions each world's runtimes are created with, and the exact list of
// imports a module of each world may have.
#pragma once

#include <span>
#include <string_view>

#include <vtextension/World.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::detail
{

/// The import module name the status-line world's host functions live under:
/// `<package>/<interface>@<version>`.
inline constexpr auto HostImportModule = std::string_view { "terminal-extensions:statusline/host@0.1.0" };

/// The status-line world's host functions, in the order Instance dispatches them by: the world's
/// `request-frame`, `log`, `state`, `run-command`, then the control interface's (controlHostFunctions(),
/// under ControlImportModule), each with the i32 parameter count the Canonical ABI flattens it to.
/// @return The table; it lives as long as the process.
[[nodiscard]] std::span<wasm::HostFunction const> statuslineHostFunctions() noexcept;

/// The host functions every runtime of @p world is created with: the status line's
/// (statuslineHostFunctions()), or the daemon's (daemonHostFunctions()), whose row index is the index the
/// world's instance class dispatches by.
///
/// A backend's registry of host functions may be process-wide (WAMR's is, searched by module and field
/// name), so runtimes alive at once must agree on every function they both register. The two worlds share
/// no import module name, so a module of one world never links to the other's functions, and a runtime
/// unregistering its world's functions leaves the other world's in place; within a world, every runtime is
/// created from this one table.
/// @return The table; it lives as long as the process.
[[nodiscard]] std::span<wasm::HostFunction const> hostFunctions(World world) noexcept;

/// One function a module may import from the host: an import's module and field name.
struct WorldImport
{
    std::string_view module; ///< The import's module name.
    std::string_view name;   ///< The import's field name.

    [[nodiscard]] constexpr bool operator==(WorldImport const&) const noexcept = default;
};

/// Every function a module of @p world may import besides WASI's allow-list: the status line's four and the
/// control verbs, or the daemon's DaemonImportRows. Exact (module, name) pairs: a function of another
/// world, another version, or another interface is not on it, whatever the runtime linked it to.
/// @return The list, in hostFunctions()'s order; it lives as long as the process.
[[nodiscard]] std::span<WorldImport const> worldImports(World world) noexcept;

} // namespace vtextension::detail
