// SPDX-License-Identifier: Apache-2.0
//
// Wasmtime's memory creator for an engine whose instances run in memory Contour gives them
// (MemorySharing::Provided): every linear memory is the provision the instantiating thread offers
// (host/MemoryProvision.hpp), placed where the extension host mapped Contour's object, never moved, and grown
// only once Contour granted it.
#pragma once

#include <cstdint>
#include <optional>

struct wasm_config_t;

namespace vtextension::wasm::wasmtime
{

/// Has every linear memory an engine configured with @p config creates made of the provision its
/// instantiating thread offers. Creating one fails, and with it the instantiation, where the thread offers
/// none, or offers one that was taken already (a second memory), or whose mapping is not what Wasmtime asks
/// for: its open bytes must be the memory's minimum, its object Wasmtime's reservation, and its guard at
/// least Wasmtime's.
/// @param config The configuration; the creator is copied into it.
void provideMemories(wasm_config_t* config) noexcept;

} // namespace vtextension::wasm::wasmtime

namespace vtextension::wasm::wasmtime::testing
{

/// What Wasmtime asked the memory creator for, in bytes.
struct MemoryRequest
{
    uint64_t minimum = 0;  ///< The memory's initial size.
    uint64_t reserved = 0; ///< The reservation it must stay within, never moving.
    uint64_t guard = 0;    ///< The inaccessible bytes that must follow the reservation.

    bool operator==(MemoryRequest const&) const = default;
};

/// Test-only: what Wasmtime last asked the memory creator for on the calling thread.
/// @return The request; nothing if none was made on this thread.
[[nodiscard]] std::optional<MemoryRequest> lastMemoryRequest() noexcept;

} // namespace vtextension::wasm::wasmtime::testing
