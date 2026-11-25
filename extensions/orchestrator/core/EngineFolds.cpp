// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Json.hpp>

namespace orchestrator
{

namespace
{
    /// The input is a string_view: nothing terminates it.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false };

    /// @return Whether @p group is a key a fold may have: bounded, and printable ASCII.
    [[nodiscard]] bool isFoldKey(std::string_view group) noexcept
    {
        return !group.empty() && group.size() <= MaxFoldKeyBytes
               && std::ranges::all_of(group, [](char c) { return c > ' ' && c < '\x7f'; });
    }

    /// One choice SetFold keeps, and the suffix its group's key carries for it.
    struct ChoiceSuffix
    {
        FoldChoice choice;     ///< The choice.
        std::string_view text; ///< Appended to the group's key.
    };

    /// Every choice, one row each, indexed by its value.
    constexpr auto ChoiceSuffixes = std::array {
        ChoiceSuffix { .choice = FoldChoice::Open, .text = "#open" },
        ChoiceSuffix { .choice = FoldChoice::Folded, .text = "#folded" },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ChoiceSuffixes.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(ChoiceSuffixes[index].choice) == index;
                                      }),
                  "ChoiceSuffixes is indexed by the FoldChoice's value");

    /// The most bytes a suffix adds to a group's key.
    constexpr auto LongestSuffix = std::ranges::max(
        ChoiceSuffixes | std::views::transform([](ChoiceSuffix const& row) { return row.text.size(); }));

    /// @return The choice @p key keeps for @p group; std::nullopt: it keeps none for it.
    [[nodiscard]] std::optional<FoldChoice> choiceIn(std::string_view key, std::string_view group) noexcept
    {
        if (!key.starts_with(group))
            return std::nullopt;
        auto const suffix = key.substr(group.size());
        auto const row = std::ranges::find(ChoiceSuffixes, suffix, &ChoiceSuffix::text);
        return row != ChoiceSuffixes.end() ? std::optional { row->choice } : std::nullopt;
    }
} // namespace

bool Engine::isFolded(std::string_view group) const noexcept
{
    return std::ranges::contains(_folds, group);
}

std::optional<FoldChoice> Engine::foldChoiceOf(std::string_view group) const noexcept
{
    for (auto const& key: _folds)
        if (auto const choice = choiceIn(key, group))
            return choice;
    return std::nullopt;
}

void Engine::loadFolds(std::string_view json)
{
    auto const read = json::readDocument<ReadOptions, std::vector<std::string>>(json);
    if (!read)
        return;
    for (auto const& group: *read)
        if (isFoldKey(group) && !isFolded(group) && _folds.size() < MaxFolds)
            _folds.push_back(group);
}

void Engine::toggleFold(std::string_view group, std::vector<Effect>& out)
{
    if (!isFoldKey(group))
        return;
    if (auto const folded = std::ranges::find(_folds, group); folded != _folds.end())
        _folds.erase(folded);
    else if (_folds.size() < MaxFolds)
        _folds.emplace_back(group);
    else
        return;
    ++_revision;
    storeFolds(out);
}

void Engine::setFold(std::string_view group, FoldChoice choice, std::vector<Effect>& out)
{
    if (!isFoldKey(group) || group.size() + LongestSuffix > MaxFoldKeyBytes)
        return;
    // Its toggle from before choices, and its earlier choice, give way to this one.
    auto const replaced = std::erase_if(
        _folds, [&](std::string const& key) { return key == group || choiceIn(key, group).has_value(); });
    if (replaced == 0 && _folds.size() >= MaxFolds)
        return;
    _folds.push_back(std::format("{}{}", group, ChoiceSuffixes[std::to_underlying(choice)].text));
    ++_revision;
    storeFolds(out);
}

void Engine::storeFolds(std::vector<Effect>& out) const
{
    auto value = std::string {};
    if (glz::write_json(_folds, value))
        return;
    out.emplace_back(StoreSet { .key = std::string { FoldsKey }, .value = std::move(value) });
}

} // namespace orchestrator
