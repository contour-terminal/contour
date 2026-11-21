// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <orchestrator/core/Uuid.hpp>

namespace orchestrator::guest
{

/// @return The source of the agents' session UUIDs: WASI's `random_get`, the guest's only read of
///         randomness. WebAssembly only.
[[nodiscard]] IUuidSource& witEntropy();

} // namespace orchestrator::guest
