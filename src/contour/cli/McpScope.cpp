// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/McpScope.hpp>

#include <algorithm>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <vtcontrol/Json.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Target.hpp>

namespace contour::cli
{

namespace
{
    constexpr auto OutsideHint =
        std::string_view { "; start `contour mcp` with --allow-all-tabs to reach it" };

    /// Where the caller's pane sits.
    struct CallerLocation
    {
        vtcontrol::WorkspaceInfo const* workspace = nullptr;
        vtcontrol::TabInfo const* tab = nullptr;
    };

    [[nodiscard]] std::optional<CallerLocation> locate(vtcontrol::ListReply const& reply,
                                                       std::string_view callerPane)
    {
        if (callerPane.empty())
            return std::nullopt;
        for (auto const& workspace: reply.workspaces)
            for (auto const& tab: workspace.tabs)
                for (auto const& pane: tab.panes)
                    if (pane.id == callerPane)
                        return CallerLocation { .workspace = &workspace, .tab = &tab };
        return std::nullopt;
    }

    void collect(std::vector<vtcontrol::TargetCandidate>& out, vtcontrol::TabInfo const& tab)
    {
        for (auto const& pane: tab.panes)
            if (auto const ref = vtcontrol::parseObjectRef(pane.id))
                out.push_back({ .ref = *ref, .label = pane.label, .position = std::nullopt });
    }

    [[nodiscard]] ScopeError fromControl(vtcontrol::ControlError const& error)
    {
        using Kind = ScopeError::Kind;
        auto const kind =
            error.code == vtcontrol::ControlErrorCode::Ambiguous ? Kind::Ambiguous : Kind::NotFound;
        return { .kind = kind, .message = error.message };
    }
} // namespace

std::expected<std::string, ScopeError> resolvePane(vtcontrol::ListReply const& reply,
                                                   std::string_view callerPane,
                                                   std::string_view target,
                                                   TabScope scope)
{
    using Kind = ScopeError::Kind;
    if (target.empty() && callerPane.empty())
        return std::unexpected(ScopeError { .kind = Kind::NoCaller,
                                            .message = "no caller pane is known (CONTOUR_PANE is not set), "
                                                       "so there is no default pane; name one" });
    auto const caller = locate(reply, callerPane);
    if (!callerPane.empty() && !caller)
        return std::unexpected(
            ScopeError { .kind = Kind::CallerGone,
                         .message = std::string(callerPane) + " is not in the workspace any more" });
    if (target.empty())
        return std::string(callerPane);

    auto own = std::vector<vtcontrol::TargetCandidate> {};
    auto all = std::vector<vtcontrol::TargetCandidate> {};
    for (auto const& workspace: reply.workspaces)
        for (auto const& tab: workspace.tabs)
        {
            collect(all, tab);
            if (caller && &tab == caller->tab)
                collect(own, tab);
        }

    if (scope == TabScope::AllTabs)
    {
        auto const found = vtcontrol::resolveTarget(vtcontrol::ObjectKind::Pane, target, all);
        if (!found)
            return std::unexpected(fromControl(found.error()));
        return vtcontrol::toString(*found);
    }

    // A caller-less OwnTab lookup cannot get here: an empty callerPane with a target leaves `own` empty,
    // so everything that exists is outside the (unknown) tab.
    auto const inTab = vtcontrol::resolveTarget(vtcontrol::ObjectKind::Pane, target, own);
    if (inTab)
        return vtcontrol::toString(*inTab);
    if (inTab.error().code == vtcontrol::ControlErrorCode::Ambiguous)
        return std::unexpected(fromControl(inTab.error()));

    // Not in the own tab: only now look further, to tell "elsewhere" from "nowhere".
    auto const elsewhere = vtcontrol::resolveTarget(vtcontrol::ObjectKind::Pane, target, all);
    if (elsewhere || elsewhere.error().code == vtcontrol::ControlErrorCode::Ambiguous)
        return std::unexpected(ScopeError { .kind = Kind::OutsideTab,
                                            .message = "'" + std::string(target) + "' is in another tab"
                                                       + std::string(OutsideHint) });
    return std::unexpected(fromControl(inTab.error()));
}

std::optional<std::string> ownTabOf(vtcontrol::ListReply const& reply, std::string_view callerPane)
{
    if (auto const where = locate(reply, callerPane))
        return where->tab->id;
    return std::nullopt;
}

std::string annotateWorkspace(vtcontrol::ListReply const& reply, std::string_view callerPane)
{
    auto document = glz::generic {};
    if (glz::read_json(document, vtcontrol::encodeJson(reply)))
        return vtcontrol::encodeJson(reply);

    auto const caller = locate(reply, callerPane);
    auto* rootPtr = document.get_if<glz::generic::object_t>();
    if (rootPtr == nullptr)
        return vtcontrol::encodeJson(reply);
    auto& rootObject = *rootPtr;
    auto const ownTabId = caller ? std::string_view { caller->tab->id } : std::string_view {};

    auto const idOf = [](glz::generic const& node) -> std::string_view {
        auto const* object = node.get_if<glz::generic::object_t>();
        if (object == nullptr || !object->contains("id"))
            return {};
        auto const* id = object->at("id").get_if<std::string>();
        return id ? std::string_view { *id } : std::string_view {};
    };

    if (auto const workspaces = rootObject.find("workspaces"); workspaces != rootObject.end())
        if (auto* workspaceList = workspaces->second.get_if<glz::generic::array_t>())
            for (auto& workspace: *workspaceList)
            {
                auto* workspaceObject = workspace.get_if<glz::generic::object_t>();
                if (workspaceObject == nullptr || !workspaceObject->contains("tabs"))
                    continue;
                auto* tabList = workspaceObject->at("tabs").get_if<glz::generic::array_t>();
                if (tabList == nullptr)
                    continue;
                for (auto& tab: *tabList)
                {
                    auto* tabObject = tab.get_if<glz::generic::object_t>();
                    if (tabObject == nullptr)
                        continue;
                    if (idOf(tab) != ownTabId || ownTabId.empty())
                        continue;
                    (*tabObject)["isOwnTab"] = true;
                    if (auto* paneList = tabObject->at("panes").get_if<glz::generic::array_t>())
                        for (auto& pane: *paneList)
                            if (idOf(pane) == callerPane)
                                pane.get<glz::generic::object_t>()["isSelf"] = true;
                }
                std::ranges::stable_partition(*tabList, [&](glz::generic const& tab) {
                    return !ownTabId.empty() && idOf(tab) == ownTabId;
                });
            }

    if (caller)
    {
        auto self = glz::generic::object_t {};
        self["pane"] = std::string(callerPane);
        self["tab"] = caller->tab->id;
        self["workspace"] = caller->workspace->id;
        rootObject["self"] = self;
    }

    auto text = std::string {};
    if (glz::write_json(document, text))
        return vtcontrol::encodeJson(reply);
    return text;
}

} // namespace contour::cli
