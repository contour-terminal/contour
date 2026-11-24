// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Hooks.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/HooksJson.hpp>
#include <orchestrator/core/Json.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using hooks::detail::JsonOutcome;

    /// The input is a string_view: nothing terminates it, and a newer `contour` may add keys.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// The agent whose hooks report a run's status.
    constexpr auto ClaudeCode = "claude-code"sv;

    /// What a dry run says about hooks that are installed: it would leave them as they are.
    constexpr auto Unchanged = "unchanged"sv;
} // namespace

HooksState hooksStateOf(std::string_view setupDryRunJson)
{
    auto const outcomes = json::readDocument<ReadOptions, std::vector<JsonOutcome>>(setupDryRunJson);
    if (!outcomes)
        return HooksState::Unknown;
    auto const claude = std::ranges::find(*outcomes, ClaudeCode, &JsonOutcome::agent);
    if (claude != outcomes->end() && claude->hooks && claude->hooks->state == Unchanged)
        return HooksState::Present;
    return HooksState::Missing;
}

std::string hooksMissingProblemFor(std::span<std::string const> workers)
{
    // HooksMissingProblem, with the workers named before its colon and the directory after its advice.
    auto const colon = HooksMissingProblem.find(':');
    return std::format("{} for {}{} with that CLAUDE_CONFIG_DIR",
                       HooksMissingProblem.substr(0, colon),
                       workersNamed(workers),
                       HooksMissingProblem.substr(colon));
}

std::string workersNamed(std::span<std::string const> workers)
{
    auto named = std::string { workers.size() == 1 ? "worker " : "workers " };
    for (auto const [index, worker]: std::views::enumerate(workers))
    {
        if (index != 0)
            named += ", ";
        named += worker;
    }
    return named;
}

} // namespace orchestrator
