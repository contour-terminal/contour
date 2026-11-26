// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <variant>

#include <orchestrator/core/Model.hpp>

namespace orchestrator
{

/// The kind of item a task works on.
enum class AppliesTo : uint8_t
{
    PullRequest, ///< A change request; spelled `pr`.
    Issue,       ///< An issue; spelled `issue`.
    Topic,       ///< A topic the user typed; spelled `topic`.
};

/// @param item An item.
/// @return The kind of task it takes: a change request's, an issue's (forge or Jira), or a topic's.
[[nodiscard]] constexpr AppliesTo appliesToOf(ItemRef const& item) noexcept
{
    if (std::holds_alternative<ChangeRequestRef>(item))
        return AppliesTo::PullRequest;
    return std::holds_alternative<TopicRef>(item) ? AppliesTo::Topic : AppliesTo::Issue;
}

} // namespace orchestrator
