// SPDX-License-Identifier: Apache-2.0
//
// Internal to the WAMR backend: the one lock over the process-wide state WAMR changes without a lock of its
// own. WAMR keeps the host functions every runtime of this process registers on one global list
// (wasm_native.c), which registering prepends to, unregistering unlinks from and frees, and loading a module
// walks to link its imports; and its loader writes the interpreter's global handle table on every load.
// Runtimes are created, loaded into and dropped on different threads at once (extensions load in parallel),
// so every one of those calls in this process goes through here.
#pragma once

#include <expected>
#include <mutex>
#include <shared_mutex>
#include <span>

#include <wasm_export.h>

#include <vtextension/ExtensionError.hpp>

namespace vtextension::wasm::wamr::detail
{

/// Registers @p symbols under @p module (raw calling convention), holding WAMR's globals to itself while it
/// does. WAMR keeps @p symbols, which must outlive the registration.
/// @param module The import module name, NUL-terminated; WAMR keeps the pointer.
/// @param symbols The functions.
/// @return Nothing once WAMR holds them; `RuntimeUnavailable` if WAMR refused them.
[[nodiscard]] std::expected<void, ExtensionError> registerNativesRaw(char const* module,
                                                                     std::span<NativeSymbol> symbols);

/// Unregisters what registerNativesRaw() registered, holding WAMR's globals to itself while it does.
/// @param module The import module name it was registered under.
/// @param symbols The same table.
void unregisterNatives(char const* module, NativeSymbol* symbols) noexcept;

/// Holds WAMR's globals unchanged for as long as the returned lock lives, while others may read them too:
/// taken around a WAMR call that reads them only (wasm_runtime_instantiate*, which resolves what loading
/// left unlinked).
/// @return The shared lock.
[[nodiscard]] std::shared_lock<std::shared_mutex> readingWamrGlobals();

/// Holds WAMR's globals to the caller alone for as long as the returned lock lives: taken around a WAMR call
/// that writes them -- wasm_runtime_load(), which also writes the handle table as it links, and
/// wasm_runtime_init() and wasm_runtime_destroy(), which register WAMR's own host functions and drop every
/// registration.
/// @return The exclusive lock.
[[nodiscard]] std::unique_lock<std::shared_mutex> changingWamrGlobals();

} // namespace vtextension::wasm::wamr::detail
