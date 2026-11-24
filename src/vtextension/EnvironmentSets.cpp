// SPDX-License-Identifier: Apache-2.0
#include <vtextension/EnvironmentSets.hpp>

#include <algorithm>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <vtextension/ArgumentKind.hpp>

namespace vtextension
{

std::expected<SetSelection, ArgumentFault> selectEnvironmentSet(
    std::optional<EnvironmentSetBinding> const& binding,
    ConfigMap const& args,
    std::span<EnvironmentSet const> sets,
    vtpty::PathStyle style)
{
    auto const isSetArgument = [&](ConfigMap::value_type const& argument) {
        return binding && argument.first == binding->argument;
    };
    auto const named = std::ranges::find_if(args, isSetArgument);
    if (named == args.end())
        return SetSelection { .arguments = args, .environment = {} };
    if (std::ranges::count_if(args, isSetArgument) != 1)
        return std::unexpected(ArgumentFault::NotAPlaceholder);
    if (!argumentAccepts(ArgumentKind::Name, named->second, style))
        return std::unexpected(ArgumentFault::NotItsKind);
    auto const set = std::ranges::find(sets, named->second, &EnvironmentSet::name);
    if (set == sets.end())
        return std::unexpected(ArgumentFault::NotItsKind);

    auto selection = SetSelection { .arguments = {}, .environment = {} };
    for (auto const& variable: binding->variables)
    {
        auto const held = std::ranges::find(set->variables, variable, &EnvironmentEntry::name);
        if (held == set->variables.end())
            continue;
        if (!isNormalisedUserValue(held->name, held->value, style))
            return std::unexpected(ArgumentFault::NotItsKind);
        selection.environment.push_back(*held);
    }
    std::ranges::copy(
        args | std::views::filter([&](auto const& argument) { return !isSetArgument(argument); }),
        std::back_inserter(selection.arguments));
    return selection;
}

std::vector<EnvironmentEntry> withSetEnvironment(std::span<EnvironmentEntry const> declared,
                                                 std::span<EnvironmentEntry const> selected)
{
    auto layer = std::vector<EnvironmentEntry> {};
    layer.reserve(declared.size() + selected.size());
    layer.append_range(declared);
    layer.append_range(selected);
    return layer;
}

} // namespace vtextension
