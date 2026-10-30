// SPDX-License-Identifier: Apache-2.0
#include <contour/command/LauncherEntries.hpp>

#include <algorithm>
#include <ranges>

namespace contour::command
{

std::vector<LauncherEntry> launcherEntries(LauncherSurface surface,
                                           std::map<std::string, config::HostEntry> const& hosts,
                                           std::string_view configPath)
{
    auto const& row = *std::ranges::find(LauncherSurfaceRows, surface, &LauncherSurfaceRow::surface);

    // The picker: `connect`, with no target.
    auto picker =
        LauncherEntry { .title = std::string { row.pickerTitle }, .detail = {}, .arguments = { "connect" } };
    if (!configPath.empty())
        picker.arguments = { "connect", "--config", std::string { configPath } };
    auto entries = std::vector<LauncherEntry> { std::move(picker) };

    for (auto const& [name, entry]: hosts | std::views::take(row.maxHosts))
        entries.push_back(LauncherEntry {
            .title = name,
            .detail = config::toString(entry.url),
            .arguments = config::connectArguments(
                config::LaunchTarget { .kind = config::LaunchTargetKind::SavedHost, .name = name },
                configPath) });
    return entries;
}

} // namespace contour::command
