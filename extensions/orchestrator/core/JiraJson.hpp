// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

/// What `acli jira workitem search --json` prints, as Jira.cpp reads it; nothing else includes this header.
/// Glaze reflects only types with linkage, so these cannot sit in Jira.cpp's anonymous namespace. Only the
/// keys the mapping reads are members: every other key is skipped.
namespace orchestrator::jira::detail
{

/// A status's `statusCategory`.
struct JsonStatusCategory
{
    std::string key; ///< `new`, `indeterminate` or `done`.
};

/// A work item's `fields.status`.
struct JsonStatus
{
    std::string name;                                 ///< The project's own name for it.
    std::optional<JsonStatusCategory> statusCategory; ///< Its category.
};

/// A work item's `fields`.
struct JsonFields
{
    std::string summary;
    std::optional<JsonStatus> status; ///< Absent when `--fields` does not ask for it.
};

/// One work item.
struct JsonWorkItem
{
    std::string key;
    JsonFields fields;
};

} // namespace orchestrator::jira::detail
