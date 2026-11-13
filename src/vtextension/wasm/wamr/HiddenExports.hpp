// SPDX-License-Identifier: Apache-2.0
//
// Internal to the WAMR backend: the exports WAMR would call by itself while instantiating a module, and the
// rewrite of a module's bytes that hides them from it.
//
// WAMR runs a WASI reactor's `_initialize` inside instantiation, where no deadline can interrupt it and where
// host functions refuse to run, and it has no option to leave that call out. The host calls it itself, under
// the load deadline (Instance::instantiate), and a reactor's initializer may run at most once. So before
// WAMR sees a module, its `_initialize` export is renamed to a private alias of the same length -- no
// section's size changes -- and the backend answers the host's `_initialize` with that alias.
//
// Pure and dependency-free, so that every framing rule is table-testable.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace vtextension::wasm::wamr
{

/// Why a module's bytes could not be read as far as its exports.
enum class ModuleBytesError : uint8_t
{
    NotAModule,         ///< No WebAssembly core module header.
    Truncated,          ///< A section or export entry runs past the end of the bytes.
    Malformed,          ///< An over-long LEB128 number, or an export section that does not fill its size.
    DuplicateSection,   ///< A second export section, which a reader could take for the only one.
    ReservedExportName, ///< The module already exports one of the private aliases.
};

/// Whether hideExports() renamed anything.
enum class ExportsHidden : uint8_t
{
    No,  ///< The module exports none of the hidden names; its bytes are unchanged.
    Yes, ///< At least one export was renamed to its private alias.
};

/// One export WAMR would call during instantiation, and the private alias it is renamed to for WAMR.
struct HiddenExport
{
    std::string_view name;  ///< The name the module exports, and the host asks for.
    std::string_view alias; ///< The name WAMR sees instead: as long as @c name, and no toolchain's choice.
};

/// The exports hidden from WAMR. Every alias is reserved: a module that exports one is refused.
inline constexpr auto HiddenExports = std::array {
    HiddenExport { .name = "_initialize", .alias = "~initialize" },
};

static_assert(std::ranges::all_of(HiddenExports,
                                  [](HiddenExport const& row) {
                                      return row.name.size() == row.alias.size();
                                  }),
              "an alias must be as long as its name, or the rewrite would change section sizes");

/// Renames each export of @p module named in HiddenExports to its alias, in place, whatever kind of
/// export it is: the host's world check then judges it by its name. Only export section entries are
/// touched: the same text in a data segment or a custom section stays as it is.
///
/// Reads just enough of the binary to find the export section -- the header, each section's id and
/// size, and the export entries -- and leaves validating the rest to the runtime. On an error the bytes
/// are unchanged, and the caller refuses the module: one this cannot read, WAMR could not load either.
/// @param module A WebAssembly core module's bytes.
/// @return Whether an export was renamed; why the bytes could not be read.
[[nodiscard]] std::expected<ExportsHidden, ModuleBytesError> hideExports(std::span<std::byte> module);

/// The exports WAMR calls by itself while instantiating a module (its `execute_post_instantiate_functions`),
/// before any instance exists for a deadline to stop, and where host functions fail. A module whose
/// exports, as WAMR read them, still name one is refused.
inline constexpr auto InstantiationHooks = std::to_array<std::string_view>({
    // A WASI reactor's initializer, run for a module that imports WASI. hideExports() renames it and the
    // host calls it under the load deadline; it is here as the backstop for whatever the rewrite and WAMR's
    // loader read differently -- a name WAMR cuts at a NUL, a duplicate section, a laxer system WAMR.
    "_initialize",
    // An early Emscripten initializer, run for every module. No guest exports it.
    "__post_instantiate",
    // The static constructors, run for a module that imports no WASI. No guest exports it: a Rust guest
    // imports WASI, and its constructors run from `_initialize`.
    "__wasm_call_ctors",
});

/// @param runtimeName An export's name as WAMR read it.
/// @return Whether WAMR would run it while instantiating (InstantiationHooks).
[[nodiscard]] bool isInstantiationHook(std::string_view runtimeName) noexcept;

/// @param runtimeName An export's name as WAMR knows it.
/// @return The name the module exported it as, which is the name the host asks for.
[[nodiscard]] std::string_view hostExportName(std::string_view runtimeName) noexcept;

/// @param hostName An export's name as the module exported it.
/// @return The name WAMR knows it by.
[[nodiscard]] std::string_view runtimeExportName(std::string_view hostName) noexcept;

} // namespace vtextension::wasm::wamr
