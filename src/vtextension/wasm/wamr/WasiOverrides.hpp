// SPDX-License-Identifier: Apache-2.0
//
// Internal to the WAMR backend: the WASI functions whose answer WAMR's own WASI would give otherwise than
// every engine must, answered by the shim (../wasi/Preview1Shim.hpp) instead.
#pragma once

#include <array>
#include <string_view>

namespace vtextension::wasm::wamr
{

/// One WASI function the shim answers in WAMR's place, and its signature in WAMR's notation.
struct WasiOverride
{
    char const* name;      ///< The function, as AllowedWasiFunctions names it, NUL-terminated for WAMR.
    char const* signature; ///< Its signature in WAMR's notation, NUL-terminated.
};

/// The overridden functions. WAMR reads the CPU-time clocks (preview 1's ids 2 and 3) where the shim
/// withholds them, as the time a process or thread spent running tells of every guest it runs beside, and the
/// extension host's filter refuses clock_getres() for them.
inline constexpr auto WasiOverrides = std::array {
    WasiOverride { .name = "clock_time_get", .signature = "(iIi)i" },
    WasiOverride { .name = "clock_res_get", .signature = "(ii)i" },
};

namespace detail
{
    /// Registers the overrides with the running runtime, once per bring-up (WAMR drops every registration as
    /// it goes): newer than WAMR's own WASI, so that a module importing one links to the shim's. Call while
    /// holding the runtime up.
    /// @return Whether WAMR took them.
    [[nodiscard]] bool registerWasiOverrides();
} // namespace detail

} // namespace vtextension::wasm::wamr
