// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp` tool table: which tools a connection's grants and the operator's access list.

#include <contour/cli/McpTools.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <vtcontrol/Describe.hpp>
#include <vtcontrol/Json.hpp>

using contour::cli::McpAccess;
using contour::cli::TabScope;
using contour::cli::ToolKind;
using contour::cli::toolsListJson;

namespace
{
using Grants = std::optional<std::vector<std::string>>;

bool lists(std::string const& json, std::string_view tool)
{
    return json.contains(std::format(R"("name":"{}")", tool));
}

Grants const AllGrants = std::vector<std::string> { "read", "screen", "input", "write" };
} // namespace

TEST_CASE("McpTools.read_screen_grants", "[mcp-tools]")
{
    auto const json = toolsListJson(Grants { { "read", "screen" } }, McpAccess::Full, TabScope::OwnTab);
    CHECK(lists(json, "workspace_list"));
    CHECK(lists(json, "pane_capture"));
    CHECK(lists(json, "block_read"));
    CHECK_FALSE(lists(json, "pane_type"));
    CHECK_FALSE(lists(json, "pane_run"));
    CHECK_FALSE(lists(json, "pane_split"));
}

TEST_CASE("McpTools.pane_run_needs_input_and_screen", "[mcp-tools]")
{
    CHECK_FALSE(lists(toolsListJson(Grants { { "input" } }, McpAccess::Full, TabScope::OwnTab), "pane_run"));
    CHECK_FALSE(lists(toolsListJson(Grants { { "screen" } }, McpAccess::Full, TabScope::OwnTab), "pane_run"));
    CHECK(lists(toolsListJson(Grants { { "input", "screen" } }, McpAccess::Full, TabScope::OwnTab),
                "pane_run"));
}

TEST_CASE("McpTools.no_grants_lists_all_but_tab_create", "[mcp-tools]")
{
    auto const own = toolsListJson(std::nullopt, McpAccess::Full, TabScope::OwnTab);
    for (auto const& row: contour::cli::mcpToolRows())
        CHECK((lists(own, row.name) == (row.name != "tab_create")));

    CHECK(lists(toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs), "tab_create"));
}

TEST_CASE("McpTools.read_only_drops_input_and_write_tools", "[mcp-tools]")
{
    auto const json = toolsListJson(AllGrants, McpAccess::ReadOnly, TabScope::AllTabs);
    for (auto const tool: { "pane_type",
                            "pane_run",
                            "pane_split",
                            "tab_create",
                            "pane_close",
                            "tab_close",
                            "object_setLabel" })
        CHECK_FALSE(lists(json, tool));
    for (auto const tool: { "workspace_list", "pane_capture", "block_list", "block_read", "daemon_info" })
        CHECK(lists(json, tool));
}

TEST_CASE("McpTools.catalog_rows_exist_in_the_api", "[mcp-tools]")
{
    auto const api = vtcontrol::listApi();
    for (auto const& row: contour::cli::mcpToolRows())
    {
        if (row.kind != ToolKind::Catalog)
            continue;
        CHECK(std::ranges::any_of(api.methods, [&](auto const& m) { return m.name == row.catalogMethod; }));
        auto name = std::string { row.catalogMethod };
        std::ranges::replace(name, '.', '_');
        CHECK(name == row.name);
    }
}

TEST_CASE("McpTools.schemas_are_self_contained", "[mcp-tools]")
{
    auto const json = toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs);
    CHECK_FALSE(json.contains("$ref"));
    CHECK_FALSE(json.contains("$defs"));
    INFO(json);
}

TEST_CASE("McpTools.definitions_are_inlined", "[mcp-tools]")
{
    auto const json = toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs);
    CHECK(json.contains(R"("scrollback":{"type":"integer")"));
    CHECK_FALSE(bool(glz::validate_json(json)));
}
