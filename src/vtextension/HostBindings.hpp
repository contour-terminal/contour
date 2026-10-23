// SPDX-License-Identifier: Apache-2.0
//
// Internal to vtextension: the statusline world's host functions, as every runtime is created with them.
#pragma once

#include <span>
#include <string_view>

#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::detail
{

/// The import module name the world's host functions live under: `<package>/<interface>@<version>`.
inline constexpr auto HostImportModule = std::string_view { "terminal-extensions:statusline/host@0.1.0" };

/// The world's host functions, in the order Instance dispatches them by: `request-frame`, `log`,
/// `state`, `run-command`, each with the i32 parameter count the Canonical ABI flattens it to.
///
/// Every runtime is created from this one table. A backend's registry of host functions may be
/// process-wide (WAMR's is), so runtimes alive at once must agree on every function's index and arity:
/// a second table, however alike, would be a second chance to disagree.
/// @return The table; it lives as long as the process.
[[nodiscard]] std::span<wasm::HostFunction const> statuslineHostFunctions() noexcept;

} // namespace vtextension::detail
