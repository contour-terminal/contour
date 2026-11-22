// SPDX-License-Identifier: Apache-2.0
//
// The Claude Code plugins the tasks' prompts need -- the skills a `/contour-workflows:fix-ci` names -- and
// whether the `claude` the agents run has them.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator
{

/// A plugin the agents need: one row of the configuration's `plugins`.
struct PluginRequirement
{
    std::string id;          ///< `<plugin>@<marketplace>`, as `claude plugin install` takes it.
    std::string marketplace; ///< What `claude plugin marketplace add` takes to know the marketplace, such as
                             ///< `contour-terminal/claude-marketplace`; empty: one Claude Code knows already.
    [[nodiscard]] bool operator==(PluginRequirement const&) const = default;
};

/// The plugins the agents need unless `plugins` says otherwise: the one the default tasks' prompts name.
[[nodiscard]] std::vector<PluginRequirement> defaultPluginRequirements();

/// Reads the output of the declared `claude-plugins` command (`claude plugin list --json`).
/// @param listJson What it printed.
/// @return The ids of the plugins Claude Code has installed and enabled; std::nullopt when the output is not
///         such a list.
[[nodiscard]] std::optional<std::vector<std::string>> enabledPluginsOf(std::string_view listJson);

/// @param required The plugins the agents need.
/// @param enabled The plugins installed and enabled (enabledPluginsOf()).
/// @return Those of @p required that @p enabled lacks, in @p required's order.
[[nodiscard]] std::vector<PluginRequirement> missingPlugins(std::span<PluginRequirement const> required,
                                                            std::span<std::string const> enabled);

} // namespace orchestrator
