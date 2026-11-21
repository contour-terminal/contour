// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Model.hpp>

#include <algorithm>
#include <span>
#include <variant>
#include <vector>

namespace orchestrator
{

std::vector<Issue> unclaimed(std::vector<Issue> issues, std::span<ChangeRequest const> mine)
{
    auto const claimed = [mine](Issue const& issue) {
        auto const* ref = std::get_if<IssueRef>(&issue.item);
        return ref != nullptr && std::ranges::any_of(mine, [ref](ChangeRequest const& changeRequest) {
                   return std::ranges::contains(changeRequest.closesIssues, ref->number);
               });
    };
    std::erase_if(issues, claimed);
    return issues;
}

} // namespace orchestrator
