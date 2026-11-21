// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <string>
#include <string_view>
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
} // namespace

bool Engine::isFolded(std::string_view group) const noexcept
{
    return std::ranges::contains(_folds, group);
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
    auto value = std::string {};
    if (glz::write_json(_folds, value))
        return;
    out.emplace_back(StoreSet { .key = std::string { FoldsKey }, .value = std::move(value) });
}

} // namespace orchestrator
