// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The engine the extension host executable runs modules on. Declared here, neutrally, and defined by the
/// directory of the engine the executable links, so that the host names no engine.

#include <memory>

#include <vtextension/wasm/host/IHostedEngine.hpp>

namespace vtextension::wasm::host
{

/// @return A new engine for the extension host process; null if none can be created. Every engine it returns
///         shares the process's one kind state.
[[nodiscard]] std::unique_ptr<IHostedEngine> makeHelperEngine();

} // namespace vtextension::wasm::host
