// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// What `acli jira workitem search --json` prints, as Jira.cpp reads it; nothing else includes this header.
/// Glaze reflects only types with linkage, so these cannot sit in Jira.cpp's anonymous namespace. Only the
/// keys the mapping reads are members: every other key is skipped.
namespace orchestrator::jira::detail
{

/// A work item's `fields`.
struct JsonFields
{
    std::string summary;
};

/// One work item.
struct JsonWorkItem
{
    std::string key;
    JsonFields fields;
};

} // namespace orchestrator::jira::detail
