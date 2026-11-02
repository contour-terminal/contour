// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp` tool table: which tools a connection's grants and the operator's access list.

#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/McpTools.hpp>
#include <contour/cli/McpToolsDetail.hpp>

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

/// @return Every permission, as a grant list.
Grants allGrants()
{
    return std::vector<std::string> { "read", "screen", "input", "write" };
}
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

TEST_CASE("McpTools.no_grants_lists_all_but_tab_create_and_tab_close", "[mcp-tools]")
{
    auto const own = toolsListJson(std::nullopt, McpAccess::Full, TabScope::OwnTab);
    for (auto const& row: contour::cli::mcpToolRows())
        CHECK((lists(own, row.name) == (row.name != "tab_create" && row.name != "tab_close")));

    CHECK(lists(toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs), "tab_create"));
    CHECK(lists(toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs), "tab_close"));
}

TEST_CASE("McpTools.read_only_drops_input_and_write_tools", "[mcp-tools]")
{
    auto const json = toolsListJson(allGrants(), McpAccess::ReadOnly, TabScope::AllTabs);
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
    INFO(json);
    CHECK_FALSE(json.contains("$ref"));
    CHECK_FALSE(json.contains("$defs"));
}

TEST_CASE("McpTools.definitions_are_inlined", "[mcp-tools]")
{
    auto const json = toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs);
    CHECK(json.contains(R"("scrollback":{"type":"integer")"));
    CHECK_FALSE(bool(glz::validate_json(json)));
}

TEST_CASE("McpTools.every_catalog_tool_is_offered", "[mcp-tools]")
{
    auto const json = toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs);
    for (auto const& row: contour::cli::mcpToolRows())
        CHECK(lists(json, row.name));
}

namespace
{
/// @return The tool's `inputSchema` as a glaze generic value.
glz::generic composite(std::string const& json, std::string_view tool)
{
    auto list = glz::generic {};
    REQUIRE_FALSE(bool(glz::read_json(list, json)));
    for (auto& entry: list["tools"].get_array())
        if (entry["name"].get_string() == tool)
            return entry["inputSchema"];
    FAIL("tool not listed: " << tool);
    return {};
}

std::vector<std::string> propertyNames(glz::generic const& schema)
{
    auto names = std::vector<std::string> {};
    for (auto const& [name, value]: schema["properties"].get_object())
        names.push_back(name);
    std::ranges::sort(names);
    return names;
}

std::vector<std::string> requiredNames(glz::generic const& schema)
{
    auto names = std::vector<std::string> {};
    if (schema.contains("required"))
        for (auto const& name: schema["required"].get_array())
            names.push_back(name.get_string());
    return names;
}
} // namespace

TEST_CASE("McpTools.composite_schemas_match_the_spec", "[mcp-tools]")
{
    using Names = std::vector<std::string>;
    auto const json = toolsListJson(std::nullopt, McpAccess::Full, TabScope::AllTabs);

    auto const split = composite(json, "pane_split");
    CHECK(propertyNames(split) == Names { "command", "cwd", "direction", "label", "pane", "ratioPercent" });
    CHECK(requiredNames(split) == Names { "direction" });
    auto directions = Names {};
    for (auto const& d: split["properties"]["direction"]["enum"].get_array())
        directions.push_back(d.get_string());
    CHECK(directions == Names { "right", "below" });

    auto const run = composite(json, "pane_run");
    CHECK(propertyNames(run) == Names { "command", "maxOutputBytes", "pane", "timeoutSeconds" });
    CHECK(requiredNames(run) == Names { "pane", "command" });
    CHECK(run["properties"]["timeoutSeconds"]["default"].as<int>() == 120);
    CHECK(run["properties"]["timeoutSeconds"]["minimum"].as<int>() == 1);
    CHECK(run["properties"]["timeoutSeconds"]["maximum"].as<int>() == contour::cli::MaxTimeoutSeconds);
    CHECK(run["properties"]["maxOutputBytes"]["default"].as<int>() == 65536);

    auto const type = composite(json, "pane_type");
    CHECK(propertyNames(type) == Names { "pane", "text" });
    CHECK(requiredNames(type) == Names { "pane", "text" });

    auto const tab = composite(json, "tab_create");
    CHECK(propertyNames(tab) == Names { "command", "cwd", "label" });
    CHECK(requiredNames(tab).empty());

    auto const paneDescription = split["properties"]["pane"]["description"].get_string();
    CHECK(paneDescription.contains("defaults to the pane this agent runs in"));
    auto const typedDescription = type["properties"]["pane"]["description"].get_string();
    CHECK(run["properties"]["pane"]["description"].get_string() == typedDescription);
    CHECK_FALSE(typedDescription.contains("defaults"));
    CHECK(typedDescription.contains("never the pane this agent runs in"));
}

TEST_CASE("McpTools.inlineSchemaRefs", "[mcp-tools]")
{
    using contour::cli::detail::inlineSchemaRefs;

    SECTION("flat, last member")
    {
        CHECK(inlineSchemaRefs(R"({"a":{"$ref":"#/$defs/X"},"$defs":{"X":{"type":"integer"}}})").value()
              == R"({"a":{"type":"integer"}})");
    }
    SECTION("first member")
    {
        CHECK(inlineSchemaRefs(R"({"$defs":{"X":{"type":"integer"}},"a":{"$ref":"#/$defs/X"}})").value()
              == R"({"a":{"type":"integer"}})");
    }
    SECTION("nested definitions, referrer first")
    {
        auto const out = inlineSchemaRefs(
            R"({"a":{"$ref":"#/$defs/Outer"},"$defs":{"Outer":{"p":{"$ref":"#/$defs/Inner"}},"Inner":{"type":"string"}}})");
        REQUIRE(out);
        CHECK(*out == R"({"a":{"p":{"type":"string"}}})");
    }
    SECTION("nested definitions, referrer last")
    {
        auto const out = inlineSchemaRefs(
            R"({"a":{"$ref":"#/$defs/Outer"},"$defs":{"Inner":{"type":"string"},"Outer":{"p":{"$ref":"#/$defs/Inner"}}}})");
        REQUIRE(out);
        CHECK(*out == R"({"a":{"p":{"type":"string"}}})");
    }
    SECTION("no $defs")
    {
        CHECK(inlineSchemaRefs(R"({"type":"object"})").value() == R"({"type":"object"})");
        CHECK_FALSE(inlineSchemaRefs(R"({"a":{"$ref":"#/$defs/X"}})"));
    }
    SECTION("malformed")
    {
        CHECK_FALSE(inlineSchemaRefs(R"({"a":1,"$defs":{"X":{"type":)"));
        CHECK_FALSE(inlineSchemaRefs(R"({"$defs":{X:{}}})"));
        CHECK_FALSE(inlineSchemaRefs(R"({"$defs":{"X":5}})"));
        CHECK_FALSE(inlineSchemaRefs(R"({"a":{"$ref":"#/$defs/Y"},"$defs":{"X":{}}})"));
    }
    SECTION("members beside a reference refine the definition")
    {
        auto const out = inlineSchemaRefs(
            R"({"a":{"$ref":"#/$defs/X","description":"d"},"b":[{"$ref":"#/$defs/X"}],"$defs":{"X":{"type":"integer"}}})");
        REQUIRE(out);
        CHECK(*out == R"({"a":{"type":"integer","description":"d"},"b":[{"type":"integer"}]})");
    }
    SECTION("cycle terminates with an error")
    {
        CHECK_FALSE(
            inlineSchemaRefs(R"({"a":{"$ref":"#/$defs/X"},"$defs":{"X":{"p":{"$ref":"#/$defs/X"}}}})"));
    }
}
