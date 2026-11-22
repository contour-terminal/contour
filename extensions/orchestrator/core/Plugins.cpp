// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Plugins.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/PluginsJson.hpp>

namespace orchestrator
{

namespace
{
    using plugins::detail::JsonPlugin;

    /// The input is a string_view: nothing terminates it, and a newer `claude` may add keys.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };
} // namespace

std::vector<PluginRequirement> defaultPluginRequirements()
{
    return { PluginRequirement { .id = "contour-workflows@contour-terminal",
                                 .marketplace = "contour-terminal/claude-marketplace" } };
}

std::optional<std::vector<std::string>> enabledPluginsOf(std::string_view listJson)
{
    auto const listed = json::readDocument<ReadOptions, std::vector<JsonPlugin>>(listJson);
    if (!listed)
        return std::nullopt;
    return *listed | std::views::filter(&JsonPlugin::enabled) | std::views::transform(&JsonPlugin::id)
           | std::ranges::to<std::vector>();
}

std::vector<PluginRequirement> missingPlugins(std::span<PluginRequirement const> required,
                                              std::span<std::string const> enabled)
{
    return required | std::views::filter([&](PluginRequirement const& plugin) {
               return !std::ranges::contains(enabled, plugin.id);
           })
           | std::ranges::to<std::vector>();
}

} // namespace orchestrator
