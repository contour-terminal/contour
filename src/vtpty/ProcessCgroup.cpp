// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessCgroup.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <utility>

namespace vtpty
{

std::optional<std::string> readProcessCgroup(int pid)
{
    auto file = std::ifstream { std::format("/proc/{}/cgroup", pid) };
    if (!file)
        return std::nullopt;
    auto text = std::ostringstream {};
    text << file.rdbuf();
    return std::move(text).str();
}

bool isInUnit(std::string_view cgroupFile, std::string_view unit) noexcept
{
    if (unit.empty())
        return false;
    return std::ranges::any_of(cgroupFile | std::views::split('\n'), [unit](auto const& lineRange) {
        auto const line = std::string_view { lineRange.begin(), lineRange.end() };
        // hierarchy-ID:controller-list:cgroup-path; the path is everything after the second colon.
        auto const controllers = line.find(':');
        auto const path =
            controllers == std::string_view::npos ? std::string_view::npos : line.find(':', controllers + 1);
        if (path == std::string_view::npos)
            return false;
        auto const cgroup = line.substr(path + 1);
        return cgroup.size() > unit.size() && cgroup.ends_with(unit)
               && cgroup[cgroup.size() - unit.size() - 1] == '/';
    });
}

} // namespace vtpty
