// SPDX-License-Identifier: Apache-2.0
//
// Test-only: what the default backend knows about the calling thread's preparation and the neutral
// interface does not tell. Declared here, defined by the selected backend's directory, and compiled into
// vtextension_test only, so that runtime-agnostic tests observe it without naming a runtime.
#pragma once

#include <cstdint>

namespace vtextension::wasm::testing
{

/// @return The runtime generation the calling thread's state was set up for by the default backend; 0 if
///         none. Unlike IWasmBackend::isCurrentThreadPrepared(), this tells a renewed preparation apart
///         from a merely usable thread. @see IWasmBackend::runtimeGeneration
[[nodiscard]] uint64_t preparedGeneration() noexcept;

} // namespace vtextension::wasm::testing
