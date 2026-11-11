// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The environment a spawned child gets: the parent's with some variables set on top. The merge is a pure
/// function over strings of either character width, so that the Windows block (UTF-16, sorted, case-blind)
/// is testable on every platform.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vthost
{

/// What a spawned child's environment differs in from its parent's.
struct SpawnEnvironment
{
    /// Variables the child gets on top of the parent's environment, in order; a later entry of the same
    /// name wins, and any of them replaces the parent's value. A name is non-empty and holds no `=`
    /// (see invalidEnvironmentName()).
    std::vector<std::pair<std::string, std::string>> set;
};

/// @param environment What a spawn was asked to set.
/// @return The first name in @p environment that cannot name a variable (empty, or holding `=`); nullopt
///         when every name can.
[[nodiscard]] inline std::optional<std::string> invalidEnvironmentName(SpawnEnvironment const& environment)
{
    auto const bad = std::ranges::find_if(
        environment.set, [](auto const& pair) { return pair.first.empty() || pair.first.contains('='); });
    if (bad == environment.set.end())
        return std::nullopt;
    return bad->first;
}

/// How variable names are told apart.
enum class NameComparison : std::uint8_t
{
    CaseSensitive = 0, ///< POSIX.
    CaseInsensitive,   ///< Windows; folded to upper case for ASCII letters only, the order Windows sorts by.
};

/// The order of the merged entries.
enum class EntryOrder : std::uint8_t
{
    AsFound = 0,  ///< The parent's entries as they came, then the set ones.
    SortedByName, ///< By name under the NameComparison, as CreateProcess documents for its block.
};

namespace detail
{
    /// @return @p name, its ASCII letters upper-cased when @p comparison is case-insensitive.
    template <typename Char>
    [[nodiscard]] std::basic_string<Char> foldedName(std::basic_string_view<Char> name,
                                                     NameComparison comparison)
    {
        auto folded = std::basic_string<Char> { name };
        if (comparison == NameComparison::CaseInsensitive)
            for (auto& character: folded)
                if (character >= Char { 'a' } && character <= Char { 'z' })
                    character = static_cast<Char>(character - Char { 'a' } + Char { 'A' });
        return folded;
    }

    /// @return The name of a `NAME=VALUE` entry; a name may itself begin with `=` (Windows' per-drive
    ///         directories), so the separator is looked for past the first character.
    template <typename Char>
    [[nodiscard]] std::basic_string_view<Char> entryName(std::basic_string_view<Char> entry)
    {
        return entry.substr(0, entry.find(Char { '=' }, 1));
    }
} // namespace detail

/// Merges a parent's environment with the variables to set on top of it.
/// @param inherited The parent's `NAME=VALUE` entries.
/// @param overrides The variables to set, in order; of two with one name the later wins.
/// @param comparison How names are told apart.
/// @param order The order of the result.
/// @return The `NAME=VALUE` entries: the parent's without those @p overrides replaces, then @p overrides.
template <typename Char>
[[nodiscard]] std::vector<std::basic_string<Char>> mergeEnvironment(
    std::span<std::basic_string<Char> const> inherited,
    std::span<std::pair<std::basic_string<Char>, std::basic_string<Char>> const> overrides,
    NameComparison comparison,
    EntryOrder order)
{
    using String = std::basic_string<Char>;
    auto const folded = [&](std::basic_string_view<Char> name) {
        return detail::foldedName<Char>(name, comparison);
    };

    // The last of each name, in the order they were given.
    auto winners = std::vector<std::pair<String, String>> {};
    for (auto const& [name, value]: overrides)
    {
        auto const key = folded(name);
        std::erase_if(winners, [&](auto const& kept) { return folded(kept.first) == key; });
        winners.emplace_back(name, value);
    }

    auto merged = std::vector<String> {};
    for (auto const& entry: inherited)
    {
        auto const key = folded(detail::entryName<Char>(entry));
        if (!std::ranges::any_of(winners, [&](auto const& pair) { return folded(pair.first) == key; }))
            merged.push_back(entry);
    }
    for (auto const& [name, value]: winners)
    {
        auto entry = name;
        entry.push_back(Char { '=' });
        entry.append(value);
        merged.push_back(std::move(entry));
    }

    if (order == EntryOrder::SortedByName)
        std::ranges::stable_sort(merged, [&](String const& a, String const& b) {
            return folded(detail::entryName<Char>(a)) < folded(detail::entryName<Char>(b));
        });
    return merged;
}

/// Frames merged entries as the environment block CreateProcessW takes.
/// @param entries `NAME=VALUE` entries.
/// @return Each entry followed by a NUL, and one more NUL to end the block; two NULs for no entry at all.
template <typename Char>
[[nodiscard]] std::basic_string<Char> environmentBlock(std::span<std::basic_string<Char> const> entries)
{
    auto block = std::basic_string<Char> {};
    for (auto const& entry: entries)
    {
        block.append(entry);
        block.push_back(Char {});
    }
    block.push_back(Char {});
    if (entries.empty())
        block.push_back(Char {});
    return block;
}

} // namespace vthost
