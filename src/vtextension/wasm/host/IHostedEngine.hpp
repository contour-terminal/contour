// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the extension host process runs modules on: a WebAssembly backend, and how it shares an instance's
/// memory with Contour.

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
};

/// Creates the engine one host runs on; null if none can be.
using EngineFactory = std::function<std::unique_ptr<IHostedEngine>()>;

} // namespace vtextension::wasm::host
