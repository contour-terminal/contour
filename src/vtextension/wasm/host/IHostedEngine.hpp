// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the extension host process runs modules on: a WebAssembly backend, and how it shares an instance's
/// memory with Contour.

#include <cstdint>
#include <functional>
#include <memory>

#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::host
{

/// A backend a host process drives. The helper's own interface: the parent never sees one.
class IHostedEngine: public IWasmBackend
{
  public:
    /// @return How the engine's instances share their linear memory with the parent: what the host states in
    ///         its Hello, and so how the parent reaches that memory.
    [[nodiscard]] virtual remote::proto::MemoryMode memoryMode() const noexcept = 0;

    /// @return Under MemoryMode::Shared, how many inaccessible bytes must follow each instance's memory
    /// object
    ///         where the host maps it: the guard the engine's compiled code relies on (MemoryProvision); 0
    ///         for an engine that copies memory.
    [[nodiscard]] virtual uint64_t memoryGuardBytes() const noexcept = 0;
};

/// Creates the engine one host runs on; null if none can be.
using EngineFactory = std::function<std::unique_ptr<IHostedEngine>()>;

} // namespace vtextension::wasm::host
