// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ChildEnvironment.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtextension
{

namespace
{
    using Environment = std::vector<std::pair<std::string, std::string>>;

    /// Sets @p name to @p value: in place where it is already set, appended otherwise.
    void assign(Environment& environment, std::string_view name, std::string_view value)
    {
        auto const existing = std::ranges::find(
            environment, name, [](auto const& entry) -> std::string_view { return entry.first; });
        if (existing != environment.end())
            existing->second = value;
        else
            environment.emplace_back(name, value);
    }
} // namespace

Inheritance inheritanceOf(std::string_view name) noexcept
{
    auto const exact = std::ranges::find_if(InheritanceRules, [&](InheritanceRule const& rule) {
        return rule.match == NameMatch::Exact && rule.name == name;
    });
    if (exact != InheritanceRules.end())
        return exact->inheritance;

    auto inheritance = Inheritance::Kept;
    auto longest = std::size_t { 0 };
    for (auto const& rule: InheritanceRules)
        if (rule.match == NameMatch::Prefix && name.starts_with(rule.name) && rule.name.size() > longest)
        {
            inheritance = rule.inheritance;
            longest = rule.name.size();
        }
    return inheritance;
}

std::vector<std::pair<std::string, std::string>> childEnvironment(
    std::span<std::string const> inherited,
    std::span<EnvironmentEntry const> commandEnvironment,
    std::span<EnvironmentEntry const> perCommand)
{
    auto environment = Environment {};
    for (std::string_view const entry: inherited)
    {
        auto const equals = entry.find('=');
        if (equals == std::string_view::npos)
            continue;
        auto const name = entry.substr(0, equals);
        if (inheritanceOf(name) == Inheritance::Kept)
            assign(environment, name, entry.substr(equals + 1));
    }
    for (auto const& layer: { commandEnvironment, perCommand })
        for (auto const& [name, value]: layer)
            assign(environment, name, value);
    return environment;
}

} // namespace vtextension
