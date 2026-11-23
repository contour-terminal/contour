// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ChildEnvironment.hpp>

#include <vtpty/AsciiFold.hpp>

#include <algorithm>
#include <array>
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

    /// @return @p c as it is.
    [[nodiscard]] constexpr char unchanged(char c) noexcept
    {
        return c;
    }

    /// What a comparison ignores, indexed by EnvironmentNameCase.
    constexpr auto FoldOf = std::array {
        &unchanged,         // Sensitive
        &vtpty::asciiUpper, // Insensitive
    };
    static_assert(FoldOf.size() == std::to_underlying(EnvironmentNameCase::Insensitive) + 1U);

    /// @return Whether @p a and @p b name one variable under @p rule.
    [[nodiscard]] bool namesMatch(std::string_view a, std::string_view b, EnvironmentNameCase rule) noexcept
    {
        auto const fold = FoldOf.at(std::to_underlying(rule));
        return std::ranges::equal(a, b, std::ranges::equal_to {}, fold, fold);
    }

    /// @return Whether @p name starts with @p prefix under @p rule.
    [[nodiscard]] bool startsWithName(std::string_view name,
                                      std::string_view prefix,
                                      EnvironmentNameCase rule) noexcept
    {
        return name.size() >= prefix.size() && namesMatch(name.substr(0, prefix.size()), prefix, rule);
    }

    /// @return Whether every byte of @p name is ASCII.
    [[nodiscard]] bool isAscii(std::string_view name) noexcept
    {
        return std::ranges::all_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    }

    /// Sets @p name to @p value: in place, under @p name's spelling, where a name matching it by @p rule is
    /// already set; appended otherwise.
    void assign(Environment& environment,
                std::string_view name,
                std::string_view value,
                EnvironmentNameCase rule)
    {
        auto const existing = std::ranges::find_if(
            environment, [&](auto const& entry) { return namesMatch(entry.first, name, rule); });
        if (existing != environment.end())
            *existing = { std::string { name }, std::string { value } };
        else
            environment.emplace_back(name, value);
    }
} // namespace

Inheritance inheritanceOf(std::string_view name, EnvironmentNameCase rule) noexcept
{
    if (rule == EnvironmentNameCase::Insensitive && !isAscii(name))
        return Inheritance::Withheld;

    auto const exact = std::ranges::find_if(InheritanceRules, [&](InheritanceRule const& candidate) {
        return candidate.match == NameMatch::Exact && namesMatch(candidate.name, name, rule);
    });
    if (exact != InheritanceRules.end())
        return exact->inheritance;

    auto inheritance = Inheritance::Kept;
    auto longest = std::size_t { 0 };
    for (auto const& candidate: InheritanceRules)
        if (candidate.match == NameMatch::Prefix && startsWithName(name, candidate.name, rule)
            && candidate.name.size() > longest)
        {
            inheritance = candidate.inheritance;
            longest = candidate.name.size();
        }
    return inheritance;
}

std::vector<std::pair<std::string, std::string>> childEnvironment(
    std::span<std::string const> inherited,
    std::span<EnvironmentEntry const> commandEnvironment,
    std::span<EnvironmentEntry const> perCommand,
    EnvironmentNameCase rule)
{
    auto environment = Environment {};
    for (std::string_view const entry: inherited)
    {
        auto const equals = entry.find('=');
        if (equals == std::string_view::npos)
            continue;
        auto const name = entry.substr(0, equals);
        if (inheritanceOf(name, rule) == Inheritance::Kept)
            assign(environment, name, entry.substr(equals + 1), rule);
    }
    for (auto const& layer: { commandEnvironment, perCommand })
        for (auto const& [name, value]: layer)
            assign(environment, name, value, rule);
    return environment;
}

} // namespace vtextension
