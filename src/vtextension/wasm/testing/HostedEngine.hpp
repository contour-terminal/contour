// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an engine a host process drives, of whichever backend the test binary links. Declared here,
// defined by that backend's directory, so that the host's tests drive a real engine without naming one.
#pragma once

#include <memory>

#include <vtextension/wasm/host/IHostedEngine.hpp>

namespace vtextension::wasm::testing
{

/// @return A new engine of the linked backend, trapping in software. Every engine it returns shares the test
///         binary's one kind state, as a process's engines do.
[[nodiscard]] std::unique_ptr<host::IHostedEngine> makeHostedEngine();

} // namespace vtextension::wasm::testing
