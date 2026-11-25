// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace orchestrator
{

/// What a task is for, as the start dialog suggests it.
enum class TaskRole : uint8_t
{
    None = 0,         ///< Nothing in particular: the dialog suggests nothing.
    FixCi,            ///< Spelled `fix_ci`.
    ResolveConflicts, ///< Spelled `resolve_conflicts`.
    AddressReview,    ///< Spelled `address_review`.
    WorkOnIssue,      ///< Spelled `work_on_issue`.
};

/// One row of the table of task roles: the spelling a configuration uses for a role.
struct TaskRoleRow
{
    TaskRole role;             ///< The role it spells.
    std::string_view spelling; ///< Its spelling in the configuration, such as `fix_ci`.
};

/// The spellings a task's `role` may take, in the order the configuration's message lists them.
inline constexpr auto TaskRoleRows = std::array {
    TaskRoleRow { .role = TaskRole::FixCi, .spelling = "fix_ci" },
    TaskRoleRow { .role = TaskRole::ResolveConflicts, .spelling = "resolve_conflicts" },
    TaskRoleRow { .role = TaskRole::AddressReview, .spelling = "address_review" },
    TaskRoleRow { .role = TaskRole::WorkOnIssue, .spelling = "work_on_issue" },
};

/// @param spelling A configuration's spelling of a role, such as `fix_ci`.
/// @return The role @p spelling names; std::nullopt for none.
[[nodiscard]] constexpr std::optional<TaskRole> taskRoleNamed(std::string_view spelling) noexcept
{
    auto const row = std::ranges::find(TaskRoleRows, spelling, &TaskRoleRow::spelling);
    if (row == TaskRoleRows.end())
        return std::nullopt;
    return row->role;
}

} // namespace orchestrator
