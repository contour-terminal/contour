// SPDX-License-Identifier: Apache-2.0
#include <vtpty/EnvironmentBlock.hpp>

#include <algorithm>
#include <format>

namespace vtpty
{

std::vector<std::string> buildEnvironmentBlock(std::span<std::string_view const> inherited,
                                               std::map<std::string, std::string> const& overrides,
                                               std::span<std::string const> removed)
{
    auto entries = std::vector<std::string> {};
    for (auto const line: inherited)
    {
        auto const separator = line.find('=');
        auto const name = std::string(separator != std::string_view::npos ? line.substr(0, separator) : line);
        if (!overrides.contains(name) && std::ranges::find(removed, name) == removed.end())
            entries.emplace_back(line);
    }
    for (auto const& [name, value]: overrides)
        entries.emplace_back(std::format("{}={}", name, value));
    return entries;
}

} // namespace vtpty
