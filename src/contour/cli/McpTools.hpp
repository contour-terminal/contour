// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp`: the tools an agent is offered. A table of rows; the catalog rows take their
// description and schema from the control API itself, the composite rows carry their own.
#pragma once

#include <contour/cli/McpScope.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Catalog.hpp>

namespace contour::cli
{

/// What the agent's operator allows it to do.
enum class McpAccess : uint8_t
{
    Full = 0, ///< Every tool the daemon's grants permit.
    ReadOnly, ///< Only tools that neither type nor change the workspace.
};

/// How a tool is served.
enum class ToolKind : uint8_t
{
    Catalog = 0, ///< One control method, called as is.
    Composite,   ///< Several calls, or a call with a resolved target.
};

/// One tool.
struct ToolRow
{
    std::string_view name;          ///< The tool's name: the method's, with '.' written as '_'.
    ToolKind kind;                  ///< How it is served.
    std::string_view catalogMethod; ///< The control method (catalog rows); empty for composites.
    vtcontrol::ControlGrants needs; ///< Every permission the tool needs.
    std::string_view summary;       ///< Description (composite rows); catalog rows read it from the API.
    std::string_view inputSchema;   ///< JSON Schema (composite rows); catalog rows read it from the API.
};

/// @return Every tool in table order.
[[nodiscard]] std::span<ToolRow const> mcpToolRows();

/// @param grants Permission names from `daemon.info`; nothing when the daemon sent none (all are listed).
/// @param access Whether the agent may type or change the workspace at all.
/// @param scope Which tabs the agent may reach; `tab_create` is listed only under `AllTabs`.
/// @return The `result` of `tools/list`: `{"tools":[...]}`.
[[nodiscard]] std::string toolsListJson(std::optional<std::vector<std::string>> const& grants,
                                        McpAccess access,
                                        TabScope scope);

} // namespace contour::cli
