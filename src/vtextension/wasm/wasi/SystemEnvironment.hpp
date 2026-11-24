// SPDX-License-Identifier: Apache-2.0
//
// The environment the WASI preview 1 shim runs with in a process: the system's clocks and its random
// source. The one place the shim reaches the operating system.
#pragma once

#include <vtextension/wasm/wasi/Preview1Shim.hpp>

namespace vtextension::wasm::wasi
{

/// @return An environment reading the system's realtime and monotonic clocks, and filling bytes from the
///         system's cryptographic random source (getrandom on Linux, arc4random_buf on the BSDs and
///         macOS, BCryptGenRandom on Windows).
[[nodiscard]] ShimEnvironment systemEnvironment();

} // namespace vtextension::wasm::wasi
