// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <orchestrator/guest/HostCalls.hpp>

namespace orchestrator::guest
{

/// @return The daemon world's imports, through the bindings `wit-bindgen c` generated (bindings/daemon.h).
///         WebAssembly only.
[[nodiscard]] IGuestHost& witHost();

} // namespace orchestrator::guest
