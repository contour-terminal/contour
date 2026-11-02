// SPDX-License-Identifier: Apache-2.0
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
          R"("pane":{"type":"string","description":"Pane: id, label or hex id suffix; defaults to the pane this agent runs in"},)"
          R"("direction":{"type":"string","enum":["right","below"],"description":"Where the new pane goes."},)"
          R"("ratioPercent":{"type":"integer","minimum":1,"maximum":100,"description":"Share of the space, in percent, the new pane takes."},)"
          R"("cwd":{"type":"string","description":"Working directory of the new pane."},)"
          R"("command":{"type":"array","items":{"type":"string"},"description":"Program and arguments to run in the new pane."},)"
          R"("label":{"type":"string","description":"Label to give the new pane."}},)"
          R"("required":["direction"],"additionalProperties":false})" },
        { "pane_run",
          ToolKind::Composite,
          {},
          ControlGrants::of({ Permission::Input, Permission::Screen }),
          "Type a command into a pane, wait for it to finish, and return its output and exit status.",
          R"({"type":"object","properties":{)"
          R"("pane":{"type":"string","description":"Pane: id, label or hex id suffix; defaults to the pane this agent runs in"},)"
          R"("command":{"type":"string","description":"The command line to run."},)"
          R"("timeoutSeconds":{"type":"integer","minimum":0,"default":120,"description":"Seconds to wait for it to finish; default 120."},)"
          R"("maxOutputBytes":{"type":"integer","minimum":0,"default":65536,"description":"Bytes of output to return at most; default 65536."}},)"
          R"("required":["command"],"additionalProperties":false})" },
        { "pane_type",
          ToolKind::Composite,
          {},
          ControlGrants::of({ Permission::Input }),
          "Type text into a pane without waiting for anything.",
          R"({"type":"object","properties":{)"
          R"("pane":{"type":"string","description":"Pane: id, label or hex id suffix; defaults to the pane this agent runs in"},)"
          R"("text":{"type":"string","description":"The text to type."}},)"
          R"("required":["text"],"additionalProperties":false})" },
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
        constexpr auto MaxInlineRounds = 16;
        constexpr auto RefPrefix = std::string_view { R"({"$ref":"#/$defs/)" };

        /// @param text JSON text.
        /// @param open Index of a '{' or '[' in @p text.
        /// @return The index one past the bracket that closes the one at @p open (strings are skipped), or
        ///         npos when it is not closed.
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
            return std::string_view::npos;
        }

        using Definitions = std::vector<std::pair<std::string, std::string>>;

        /// @param body The inside of a `$defs` object: `"name":{...},"name":{...}`.
        /// @return The named definitions, or nothing when @p body is malformed.
        std::optional<Definitions> parseDefinitions(std::string_view body)
        {
            auto defs = Definitions {};
            for (auto at = size_t { 0 }; at < body.size();)
            {
                if (body[at] != '"')
                    return std::nullopt;
                auto const nameEnd = body.find('"', at + 1);
                if (nameEnd == std::string_view::npos || nameEnd + 2 >= body.size()
                    || body[nameEnd + 1] != ':' || body[nameEnd + 2] != '{')
                    return std::nullopt;
                auto const valueEnd = endOfValue(body, nameEnd + 2);
                if (valueEnd == std::string_view::npos)
                    return std::nullopt;
                defs.emplace_back(std::string { body.substr(at + 1, nameEnd - at - 1) },
                                  std::string { body.substr(nameEnd + 2, valueEnd - nameEnd - 2) });
                at = valueEnd;
                if (at < body.size())
                {
                    if (body[at] != ',')
                        return std::nullopt;
                    ++at;
                }
            }
            return defs;
        }

        /// Replaces each `{"$ref":"#/$defs/name"}` of @p text by its definition.
        /// @return Whether anything was replaced.
        bool substituteOnce(std::string& text, Definitions const& defs)
        {
            auto replaced = false;
            for (auto const& [name, value]: defs)
            {
                auto const reference = std::format(R"({}{}"}})", RefPrefix, name);
                for (auto at = text.find(reference); at != std::string::npos;
                     at = text.find(reference, at + value.size()))
                {
                    text.replace(at, reference.size(), value);
                    replaced = true;
                }
            }
            return replaced;
        }
    } // namespace

    std::expected<std::string, std::string> inlineSchemaRefs(std::string_view schema)
    {
        auto text = std::string { schema };
        auto defs = Definitions {};

        constexpr auto DefsKey = std::string_view { R"("$defs":{)" };
        if (auto const keyAt = text.find(DefsKey); keyAt != std::string::npos)
        {
            auto const open = keyAt + DefsKey.size() - 1;
            auto const end = endOfValue(text, open);
            if (end == std::string_view::npos)
                return std::unexpected("unterminated $defs");
            auto parsed = parseDefinitions(std::string_view { text }.substr(open + 1, end - open - 2));
            if (!parsed)
                return std::unexpected("malformed $defs");
            defs = *std::move(parsed);

            // Drop the member together with one neighbouring comma, so it may be first, middle or last.
            auto from = keyAt;
            auto to = end;
            if (from > 0 && text[from - 1] == ',')
                --from;
            else if (to < text.size() && text[to] == ',')
                ++to;
            text.erase(from, to - from);
        }

        // A definition may name another, in any order: substitute until no reference is left.
        for (auto round = 0; text.contains(RefPrefix); ++round)
        {
            if (round == MaxInlineRounds)
                return std::unexpected("$ref nesting too deep or cyclic");
            if (!substituteOnce(text, defs))
                return std::unexpected("$ref to an unknown definition");
        }
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
    return Rows;
}

std::string toolsListJson(std::optional<std::vector<std::string>> const& grants,
                          McpAccess access,
                          TabScope scope)
{
    auto json = std::string { R"({"tools":[)" };
    auto separator = std::string_view {};
    for (auto const& row:
         Rows | std::views::filter([&](auto const& r) { return isOffered(r, grants, access, scope); }))
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
