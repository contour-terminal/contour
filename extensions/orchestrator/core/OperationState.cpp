// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/OperationState.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <ranges>
#include <string_view>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    struct OperationRow
    {
        Operation operation;
        std::string_view name;
    };

    constexpr auto OperationNames = std::array {
        OperationRow { .operation = Operation::Rebase, .name = "rebase"sv },
        OperationRow { .operation = Operation::Merge, .name = "merge"sv },
        OperationRow { .operation = Operation::CherryPick, .name = "cherry-pick"sv },
        OperationRow { .operation = Operation::Revert, .name = "revert"sv },
        OperationRow { .operation = Operation::Bisect, .name = "bisect"sv },
        OperationRow { .operation = Operation::Am, .name = "am"sv },
    };

    /// A line git's long status prints, in the C locale, while an operation is in progress.
    struct StateLine
    {
        std::string_view prefix;
        Operation operation;
    };

    /// Measured with git 2.56, every row; to be measured again when git is upgraded. A conflicting
    /// `stash pop` or `checkout -m` prints none of these, but leaves unmerged entries that `git status
    /// --porcelain` reports. On a detached HEAD git words the `edit` and split stops "… during a rebase.",
    /// which no row names: the rebase is found by its header line, which comes first.
    constexpr auto StateLines = std::array {
        StateLine { .prefix = "interactive rebase in progress; onto "sv, .operation = Operation::Rebase },
        StateLine { .prefix = "rebase in progress; onto "sv, .operation = Operation::Rebase },
        StateLine { .prefix = "You are currently rebasing"sv, .operation = Operation::Rebase },
        StateLine { .prefix = "You are currently editing a commit while rebasing"sv,
                    .operation = Operation::Rebase },
        StateLine { .prefix = "You are currently splitting a commit while rebasing"sv,
                    .operation = Operation::Rebase },
        StateLine { .prefix = "You have unmerged paths."sv, .operation = Operation::Merge },
        StateLine { .prefix = "All conflicts fixed but you are still merging."sv,
                    .operation = Operation::Merge },
        StateLine { .prefix = "You are currently cherry-picking commit "sv,
                    .operation = Operation::CherryPick },
        StateLine { .prefix = "Cherry-pick currently in progress."sv, .operation = Operation::CherryPick },
        StateLine { .prefix = "You are currently reverting commit "sv, .operation = Operation::Revert },
        StateLine { .prefix = "Revert currently in progress."sv, .operation = Operation::Revert },
        StateLine { .prefix = "You are currently bisecting"sv, .operation = Operation::Bisect },
        StateLine { .prefix = "You are in the middle of an am session."sv, .operation = Operation::Am },
    };

    /// How the long status starts, as recorded with git 2.56: the branch line, or a rebase's header
    /// above it. A first line of any other shape means a git this table was not measured with.
    constexpr auto FirstLines = std::array {
        "On branch "sv,
        "HEAD detached at "sv,
        "HEAD detached from "sv,
        "Not currently on any branch."sv,
        "interactive rebase in progress; onto "sv,
        "rebase in progress; onto "sv,
    };
} // namespace

std::string_view operationName(Operation operation) noexcept
{
    auto const row = std::ranges::find(OperationNames, operation, &OperationRow::operation);
    return row != OperationNames.end() ? row->name : std::string_view {};
}

std::expected<std::optional<Operation>, OperationStateError> operationInProgress(
    std::string_view longStatus) noexcept
{
    auto const firstLine = longStatus.substr(0, longStatus.find('\n'));
    if (std::ranges::none_of(FirstLines,
                             [firstLine](std::string_view shape) { return firstLine.starts_with(shape); }))
        return std::unexpected { OperationStateError::Unrecognised };
    for (auto const part: std::views::split(longStatus, '\n'))
    {
        auto const line = std::string_view(part.begin(), part.end());
        auto const match = std::ranges::find_if(
            StateLines, [line](StateLine const& state) { return line.starts_with(state.prefix); });
        if (match != StateLines.end())
            return match->operation;
    }
    return std::nullopt;
}

} // namespace orchestrator
