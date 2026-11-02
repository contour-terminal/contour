// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/McpTools.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>

#include <vtcontrol/Describe.hpp>
#include <vtcontrol/Json.hpp>

using vtcontrol::ControlGrants;
using vtcontrol::Permission;

namespace contour::cli
{

namespace
{
    // One row per tool; adding a tool is adding a row (and, for a composite, its handler in the server).
    constexpr auto Rows = std::to_array<ToolRow>({
        { "workspace_list",
          ToolKind::Catalog,
          "workspace.list",
          ControlGrants::of({ Permission::Read }),
          {},
          {} },
        { "pane_capture",
          ToolKind::Catalog,
          "pane.capture",
          ControlGrants::of({ Permission::Screen }),
          {},
          {} },
        { "pane_close", ToolKind::Catalog, "pane.close", ControlGrants::of({ Permission::Write }), {}, {} },
        { "tab_close", ToolKind::Catalog, "tab.close", ControlGrants::of({ Permission::Write }), {}, {} },
        { "object_setLabel",
          ToolKind::Catalog,
          "object.setLabel",
          ControlGrants::of({ Permission::Write }),
          {},
          {} },
        { "block_list", ToolKind::Catalog, "block.list", ControlGrants::of({ Permission::Screen }), {}, {} },
        { "block_read", ToolKind::Catalog, "block.read", ControlGrants::of({ Permission::Screen }), {}, {} },
        { "daemon_info", ToolKind::Catalog, "daemon.info", ControlGrants::of({ Permission::Read }), {}, {} },
        { "pane_split",
          ToolKind::Composite,
          {},
          ControlGrants::of({ Permission::Write }),
          "Split a pane and optionally run a command in the new pane.",
          R"({"type":"object","properties":{)"
          R"("pane":{"type":"string","description":"Pane to split; omit for your own pane."},)"
          R"("direction":{"type":"string","enum":["right","down"],"description":"Where the new pane goes."},)"
          R"("command":{"type":"array","items":{"type":"string"},"description":"Program and arguments to run in the new pane."},)"
          R"("cwd":{"type":"string","description":"Working directory of the new pane."}},)"
          R"("additionalProperties":false})" },
        { "pane_run",
          ToolKind::Composite,
          {},
          ControlGrants::of({ Permission::Input, Permission::Screen }),
          "Type a command into a pane, wait for it to finish, and return its output and exit status.",
          R"({"type":"object","properties":{)"
          R"("command":{"type":"string","description":"The command line to run."},)"
          R"("timeoutMs":{"type":"integer","minimum":0,"description":"How long to wait for it to finish."},)"
          R"("pane":{"type":"string","description":"Pane label, id or hex id suffix; omit for your own pane."}},"required":["command"],"additionalProperties":false})" },
        { "pane_type",
          ToolKind::Composite,
          {},
          ControlGrants::of({ Permission::Input }),
          "Type text into a pane without waiting for anything.",
          R"({"type":"object","properties":{)"
          R"("text":{"type":"string","description":"The text to type."},)"
          R"("pane":{"type":"string","description":"Pane label, id or hex id suffix; omit for your own pane."}},"required":["text"],"additionalProperties":false})" },
        { "tab_create",
          ToolKind::Composite,
          {},
          ControlGrants::of({ Permission::Write }),
          "Open a new tab, optionally running a command in it.",
          R"({"type":"object","properties":{)"
          R"("command":{"type":"array","items":{"type":"string"},"description":"Program and arguments to run."},)"
          R"("cwd":{"type":"string","description":"Working directory of the new tab."}},)"
          R"("additionalProperties":false})" },
    });

    constexpr auto PermissionNames = std::to_array<std::pair<std::string_view, Permission>>({
        { "read", Permission::Read },
        { "screen", Permission::Screen },
        { "input", Permission::Input },
        { "write", Permission::Write },
    });

    /// @return The permissions named by @p names; unknown names are ignored.
    ControlGrants grantsOf(std::vector<std::string> const& names)
    {
        auto grants = ControlGrants::none();
        for (auto const& name: names)
            for (auto const& [text, permission]: PermissionNames)
                if (name == text)
                    grants = grants.with(permission);
        return grants;
    }

    bool holdsAll(ControlGrants const& held, ControlGrants const& needed)
    {
        return std::ranges::all_of(PermissionNames, [&](auto const& row) {
            return !needed.contains(row.second) || held.contains(row.second);
        });
    }

    bool isListed(ToolRow const& row,
                  std::optional<ControlGrants> const& held,
                  McpAccess access,
                  TabScope scope)
    {
        if (row.name == "tab_create" && scope != TabScope::AllTabs)
            return false;
        if (access == McpAccess::ReadOnly
            && (row.needs.contains(Permission::Input) || row.needs.contains(Permission::Write)))
            return false;
        return !held || holdsAll(*held, row.needs);
    }

    /// @param text JSON text.
    /// @param open Index of a '{' or '[' in @p text.
    /// @return The index one past the bracket that closes the one at @p open; strings are skipped.
    size_t endOfValue(std::string_view text, size_t open)
    {
        auto depth = 0;
        auto inString = false;
        for (auto at = open; at < text.size(); ++at)
        {
            auto const ch = text[at];
            if (inString)
            {
                if (ch == '\\')
                    ++at;
                else if (ch == '"')
                    inString = false;
            }
            else if (ch == '"')
                inString = true;
            else if (ch == '{' || ch == '[')
                ++depth;
            else if ((ch == '}' || ch == ']') && --depth == 0)
                return at + 1;
        }
        return text.size();
    }

    /// Replaces every `{"$ref":"#/$defs/X"}` by the definition `X` and drops the `$defs` member, because
    /// some MCP clients reject references. Glaze writes `$defs` as the last member of the schema object, a
    /// flat object of named schemas, none of which refers to another.
    std::string inlineRefs(std::string schema)
    {
        constexpr auto DefsKey = std::string_view { R"(,"$defs":{)" };
        auto const defsAt = schema.find(DefsKey);
        if (defsAt == std::string::npos)
            return schema;
        auto const defsOpen = defsAt + DefsKey.size() - 1;
        auto const defsEnd = endOfValue(schema, defsOpen);
        auto const defs = schema.substr(defsOpen + 1, defsEnd - defsOpen - 2);
        schema.erase(defsAt, defsEnd - defsAt);

        // Entries are `"name":{...}`; step over each one.
        for (auto at = size_t { 0 }; at < defs.size();)
        {
            auto const nameEnd = defs.find('"', at + 1);
            auto const valueOpen = nameEnd + 2;
            auto const valueEnd = endOfValue(defs, valueOpen);
            auto const reference =
                std::format(R"({{"$ref":"#/$defs/{}"}})", defs.substr(at + 1, nameEnd - at - 1));
            auto const value = defs.substr(valueOpen, valueEnd - valueOpen);
            for (auto found = schema.find(reference); found != std::string::npos;
                 found = schema.find(reference, found + value.size()))
                schema.replace(found, reference.size(), value);
            at = valueEnd + 1; // skip the comma
        }
        return schema;
    }
} // namespace

std::span<ToolRow const> mcpToolRows()
{
    return Rows;
}

std::string toolsListJson(std::optional<std::vector<std::string>> const& grants,
                          McpAccess access,
                          TabScope scope)
{
    auto const held = grants ? std::optional { grantsOf(*grants) } : std::nullopt;
    auto json = std::string { R"({"tools":[)" };
    auto separator = std::string_view {};
    for (auto const& row:
         Rows | std::views::filter([&](auto const& r) { return isListed(r, held, access, scope); }))
    {
        auto summary = std::string { row.summary };
        auto schema = std::string { row.inputSchema };
        if (row.kind == ToolKind::Catalog)
        {
            auto const description = vtcontrol::describeApi(row.catalogMethod);
            if (!description)
                continue;
            summary = description->summary;
            schema = inlineRefs(description->inputSchema.text);
        }
        json += separator;
        json += std::format(R"({{"name":"{}","description":{},"inputSchema":{}}})",
                            row.name,
                            vtcontrol::quoteJsonString(summary),
                            schema);
        separator = ",";
    }
    json += "]}";
    return json;
}

} // namespace contour::cli
