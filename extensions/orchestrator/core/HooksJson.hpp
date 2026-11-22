// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

/// What `contour agent setup --json` prints, as Hooks.cpp reads it; nothing else includes this header. Glaze
/// reflects only types with linkage, so these cannot sit in Hooks.cpp's anonymous namespace. Only the keys
/// the orchestrator reads are members: the rest are skipped.
namespace orchestrator::hooks::detail
{

/// An agent's hook configuration file.
struct JsonHooks
{
    std::string state;
};

/// One agent's outcome.
struct JsonOutcome
{
    std::string agent;
    std::optional<JsonHooks> hooks;
};

} // namespace orchestrator::hooks::detail
