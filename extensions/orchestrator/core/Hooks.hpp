// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

/// What `contour agent setup --dry-run --json` says about Claude Code's status hooks. The JSON reader stays
/// in Hooks.cpp: this header must not pull it in (scripts/include-budget.txt).
namespace orchestrator
{

/// Whether Claude Code's status hooks are installed, which every run needs to report its agent's status.
enum class HooksState : uint8_t
{
    Unknown, ///< Not checked yet, or the check failed.
    Present, ///< Installed: a dry run would leave them unchanged.
    Missing, ///< Not installed, or Claude Code's configuration is absent or unreadable.
};

/// Reads the output of the declared `agent-setup-check` command.
/// @param setupDryRunJson What `contour agent setup --dry-run --json` printed: one outcome per agent.
/// @return Present when the `claude-code` outcome's hooks are `unchanged`; Missing when the outcome is
/// absent,
///         skipped, or its hooks would change or cannot be edited; Unknown when the output is not that JSON.
[[nodiscard]] HooksState hooksStateOf(std::string_view setupDryRunJson);

} // namespace orchestrator
