// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

/// The settings document Claude Code's `--settings` takes, as Permissions.cpp writes and reads it; nothing
/// else includes this header. Glaze reflects only types with linkage, so these cannot sit in
/// Permissions.cpp's anonymous namespace. The members are spelt as Claude Code spells its keys.
namespace orchestrator::permissions::detail
{

/// The `permissions` object.
struct JsonPermissions
{
    std::string defaultMode;        ///< Its mode, as Claude Code spells it.
    std::vector<std::string> allow; ///< Allowed rules.
    std::vector<std::string> deny;  ///< Denied rules.
};

/// The whole settings document.
struct JsonSettings
{
    JsonPermissions permissions; ///< The only key the orchestrator sets.
};

} // namespace orchestrator::permissions::detail
