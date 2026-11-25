// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The engine the extension host executable runs modules on. Declared here, neutrally, and defined by the
/// directory of the engine the executable links, so that the host names no engine.

#include <memory>

#include <vtextension/wasm/host/IHostedEngine.hpp>

namespace vtextension::wasm::host
{

/// @return A new engine for the extension host process; null if none can be created. Its instances run in the
///         memory object Contour grants each of them where this system can map one
///         (remote::transport::canMapGuestMemory()), and copy memory over the protocol where it cannot. Every
///         engine it returns shares the process's one kind state.
[[nodiscard]] std::unique_ptr<IHostedEngine> makeHelperEngine();

/// @return A new engine as makeHelperEngine() makes it, but whose instances copy memory over the protocol
///         wherever they run: what a host that cannot map memory runs, on a system that can.
[[nodiscard]] std::unique_ptr<IHostedEngine> makeCopyingHelperEngine();

} // namespace vtextension::wasm::host
