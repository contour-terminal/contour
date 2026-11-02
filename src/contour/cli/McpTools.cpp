// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/McpTools.hpp>
#include <contour/cli/McpToolsDetail.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <vector>

#include <vtcontrol/Describe.hpp>
#include <vtcontrol/Json.hpp>

using vtcontrol::ControlGrants;
using vtcontrol::Permission;

namespace contour::cli
{

namespace
{
    // One row per tool; adding a tool is adding a row (and, for a composite, its handler in the server).
    /// `pane_run`'s schema; its timeout bounds are the CLI's, so they are spelled from the one constant.
    std::string const& runSchema()
    {
        static auto const schema =
            std::string {
                R"({"type":"object","properties":{)"
                R"("pane":{"type":"string","description":"Pane: id, label or hex id suffix; required, and never the pane this agent runs in."},)"
                R"("command":{"type":"string","description":"One command line to run; join several with && or ;."},)"
                R"("timeoutSeconds":{"type":"integer","minimum":1,"maximum":)"
            }
            + std::to_string(MaxTimeoutSeconds)
            + R"(,"default":120,"description":"Seconds to wait for it to finish; default 120."},)"
              R"("maxOutputBytes":{"type":"integer","minimum":0,"default":65536,"description":"Bytes of output to return at most; default 65536."}},)"
              R"("required":["pane","command"],"additionalProperties":false})";
        return schema;
    }

    /// @return The tool table, built on first use.
    std::span<ToolRow const> rows()
    {
        static auto const table = std::to_array<ToolRow>({
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
            { "pane_close",
              ToolKind::Catalog,
              "pane.close",
              ControlGrants::of({ Permission::Write }),
              {},
              {} },
            { "tab_close",
              ToolKind::Catalog,
              "tab.close",
              ControlGrants::of({ Permission::Write }),
              {},
              {},
              TabScope::AllTabs },
            { "object_setLabel",
              ToolKind::Catalog,
              "object.setLabel",
              ControlGrants::of({ Permission::Write }),
              {},
              {} },
            { "block_list",
              ToolKind::Catalog,
              "block.list",
              ControlGrants::of({ Permission::Screen }),
              {},
              {} },
            { "block_read",
              ToolKind::Catalog,
              "block.read",
              ControlGrants::of({ Permission::Screen }),
              {},
              {} },
            { "daemon_info",
              ToolKind::Catalog,
              "daemon.info",
              ControlGrants::of({ Permission::Read }),
              {},
              {} },
            { "pane_split",
              ToolKind::Composite,
              {},
              ControlGrants::of({ Permission::Write }),
              "Split a pane and optionally run a command in the new pane.",
              R"({"type":"object","properties":{)"
              R"("pane":{"type":"string","description":"Pane: id, label or hex id suffix; defaults to the pane this agent runs in"},)"
              R"("direction":{"type":"string","enum":["right","below"],"description":"Where the new pane goes."},)"
              R"("ratioPercent":{"type":"integer","minimum":1,"maximum":100,"description":"Share of the space, in percent, the new pane takes."},)"
              R"("cwd":{"type":"string","description":"Working directory of the new pane."},)"
              R"("command":{"type":"array","items":{"type":"string"},"description":"Program and arguments to run in the new pane."},)"
              R"("label":{"type":"string","description":"Label to give the new pane."}},)"
              R"("required":["direction"],"additionalProperties":false})" },
            {
                "pane_run",
                ToolKind::Composite,
                {},
                ControlGrants::of({ Permission::Input, Permission::Screen }),
                "Type one command line into a shell pane, wait for it to finish, and return its output and "
                "exit "
                "status. The pane needs OSC 133 shell integration. If the result says timedOut, the command "
                "is "
                "still running; fetch its output later with block_read. The output is the last 64 KiB unless "
                "maxOutputBytes says otherwise. Refuses the pane this agent runs in; use pane_split to get a "
                "shell pane first.",
                runSchema(),
            },
            { "pane_type",
              ToolKind::Composite,
              {},
              ControlGrants::of({ Permission::Input }),
              "Type text into a pane without waiting for anything. Refuses the pane this agent runs in.",
              R"({"type":"object","properties":{)"
              R"("pane":{"type":"string","description":"Pane: id, label or hex id suffix; required, and never the pane this agent runs in."},)"
              R"("text":{"type":"string","description":"The text to type."}},)"
              R"("required":["pane","text"],"additionalProperties":false})" },
            { "tab_create",
              ToolKind::Composite,
              {},
              ControlGrants::of({ Permission::Write }),
              "Open a new tab, optionally running a command in it.",
              R"({"type":"object","properties":{)"
              R"("cwd":{"type":"string","description":"Working directory of the new tab."},)"
              R"("command":{"type":"array","items":{"type":"string"},"description":"Program and arguments to run."},)"
              R"("label":{"type":"string","description":"Label to give the new tab."}},)"
              R"("additionalProperties":false})",
              TabScope::AllTabs },
        });
        return table;
    }

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

} // namespace

namespace detail
{

    namespace
    {
        using Object = glz::generic::object_t;
        using Array = glz::generic::array_t;

        constexpr auto MaxRefDepth = 16;
        constexpr auto RefPrefix = std::string_view { "#/$defs/" };

        /// Replaces every `{"$ref":"#/$defs/X"}` below @p node by the definition `X`.
        /// @param node The value to rewrite in place.
        /// @param defs The definitions by name.
        /// @param depth How many references deep @p node already sits.
        /// @return Nothing, or why a reference cannot be resolved.
        std::expected<void, std::string> resolveRefs(glz::generic& node, Object const& defs, int depth)
        {
            if (auto* array = node.get_if<Array>())
            {
                for (auto& element: *array)
                    if (auto const resolved = resolveRefs(element, defs, depth); !resolved)
                        return resolved;
                return {};
            }
            auto* object = node.get_if<Object>();
            if (object == nullptr)
                return {};

            if (auto const ref = object->find("$ref"); ref != object->end())
                if (auto const* target = ref->second.get_if<std::string>();
                    target != nullptr && std::string_view { *target }.starts_with(RefPrefix))
                {
                    auto const definition = defs.find(std::string_view { *target }.substr(RefPrefix.size()));
                    if (definition == defs.end())
                        return std::unexpected("$ref to an unknown definition");
                    if (depth >= MaxRefDepth)
                        return std::unexpected("$ref nesting too deep or cyclic");
                    auto replacement = definition->second;
                    // Members written beside the reference refine the definition.
                    for (auto& [key, value]: *object)
                        if (key != "$ref")
                            replacement.get<Object>()[key] = std::move(value);
                    node = std::move(replacement);
                    return resolveRefs(node, defs, depth + 1);
                }

            for (auto& [key, member]: *object)
                if (auto const resolved = resolveRefs(member, defs, depth); !resolved)
                    return resolved;
            return {};
        }
    } // namespace

    std::expected<std::string, std::string> inlineSchemaRefs(std::string_view schema)
    {
        auto document = glz::generic {};
        if (glz::read_json(document, schema))
            return std::unexpected("malformed schema");
        auto* root = document.get_if<Object>();
        if (root == nullptr)
            return std::unexpected("schema is not an object");

        auto defs = Object {};
        if (auto const node = root->find("$defs"); node != root->end())
        {
            auto* definitions = node->second.get_if<Object>();
            if (definitions == nullptr || !std::ranges::all_of(*definitions, [](auto const& entry) {
                    return entry.second.template get_if<Object>() != nullptr;
                }))
                return std::unexpected("malformed $defs");
            defs = std::move(*definitions);
            root->erase(node);
        }
        if (auto const resolved = resolveRefs(document, defs, 0); !resolved)
            return std::unexpected(resolved.error());

        auto text = std::string {};
        if (glz::write_json(document, text))
            return std::unexpected("schema cannot be written");
        return text;
    }

} // namespace detail

bool isOffered(ToolRow const& row,
               std::optional<std::vector<std::string>> const& grants,
               McpAccess access,
               TabScope scope)
{
    if (row.requiredScope == TabScope::AllTabs && scope != TabScope::AllTabs)
        return false;
    if (access == McpAccess::ReadOnly
        && (row.needs.contains(Permission::Input) || row.needs.contains(Permission::Write)))
        return false;
    return !grants || holdsAll(grantsOf(*grants), row.needs);
}

std::span<ToolRow const> mcpToolRows()
{
    return rows();
}

std::string toolsListJson(std::optional<std::vector<std::string>> const& grants,
                          McpAccess access,
                          TabScope scope)
{
    auto json = std::string { R"({"tools":[)" };
    auto separator = std::string_view {};
    for (auto const& row:
         rows() | std::views::filter([&](auto const& r) { return isOffered(r, grants, access, scope); }))
    {
        auto summary = std::string { row.summary };
        auto schema = std::string { row.inputSchema };
        if (row.kind == ToolKind::Catalog)
        {
            auto const description = vtcontrol::describeApi(row.catalogMethod);
            if (!description)
                continue;
            summary = description->summary;
            auto inlined = detail::inlineSchemaRefs(description->inputSchema.text);
            if (!inlined)
                continue; // a catalog schema that cannot be made self-contained is not offered
            schema = *std::move(inlined);
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
