// SPDX-License-Identifier: Apache-2.0
#include <contour/session/InProcessDirectory.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/ObjectName.hpp>
#include <vtworkspace/Tab.hpp>
#include <vtworkspace/WorkspaceState.hpp>

namespace contour::session
{

namespace
{
    /// @return The layout tree of @p node as the daemon's wire form states it, so that an in-process
    ///         directory is built by the very function a daemon's is (window::directoryFromLayouts).
    vthost::proto::WirePane wirePaneOf(vtworkspace::PaneTreeState const& node)
    {
        auto wire = vthost::proto::WirePane {};
        wire.paneId = node.id.value;
        wire.split = std::to_underlying(node.split);
        wire.session = node.session.value;
        for (auto const& child: node.children)
            wire.children.push_back(wirePaneOf(child));
        return wire;
    }

    /// @param model Where the tabs' names and colours are: a tree captures neither.
    vthost::proto::LayoutState layoutOf(vtworkspace::SessionModel const& model,
                                        vtworkspace::WorkspaceState const& workspace)
    {
        auto layout = vthost::proto::LayoutState {};
        layout.workspace = workspace.id.value;
        layout.activeTab = static_cast<uint32_t>(std::max(workspace.activeTabIndex, 0));
        for (auto const& tab: workspace.tabs)
        {
            auto wire = vthost::proto::WireTab {};
            wire.tabId = tab.id.value;
            if (auto const* const known = model.findTab(tab.id))
            {
                wire.name = known->name();
                if (auto const color = known->color())
                {
                    wire.hasColor = 1;
                    wire.color = color->value();
                }
            }
            wire.root = wirePaneOf(tab.root);
            layout.tabs.push_back(std::move(wire));
        }
        return layout;
    }

    /// What an in-process workspace states of itself: its name, its colour and its git state; it has no
    /// control id.
    class MetaOfProcess final: public window::WorkspaceMetaLookup
    {
      public:
        MetaOfProcess(vtworkspace::SessionModel const& model,
                      WorkspaceNames const& names,
                      vthost::WorkspaceGitTracker const* git):
            _model { model }, _names { names }, _git { git }
        {
        }

        [[nodiscard]] window::WorkspaceMetadata metaOf(uint64_t workspace) const override
        {
            auto const id = vtworkspace::WorkspaceId { workspace };
            auto stated = window::WorkspaceMetadata { .name = _names.nameOf(id) };
            if (auto const* const known = _model.workspace(id))
                stated.color = known->color();
            if (_git != nullptr)
                if (auto const info = _git->infoOf(id))
                    stated.git = window::DirectoryGit {
                        .branch = info->branch,
                        .kind = info->kind,
                        .ahead = info->ahead,
                        .behind = info->behind,
                    };
            return stated;
        }

      private:
        vtworkspace::SessionModel const& _model;
        WorkspaceNames const& _names;
        vthost::WorkspaceGitTracker const* _git;
    };
} // namespace

void WorkspaceNames::set(vtworkspace::WorkspaceId workspace, std::string name)
{
    name = vtworkspace::normalizedName(name);
    if (name.empty())
        _names.erase(workspace.value);
    else
        _names.insert_or_assign(workspace.value, std::move(name));
}

std::string WorkspaceNames::nameOf(vtworkspace::WorkspaceId workspace) const
{
    auto const found = _names.find(workspace.value);
    return found != _names.end() ? found->second : std::string {};
}

void WorkspaceNames::forget(vtworkspace::WorkspaceId workspace)
{
    _names.erase(workspace.value);
}

InProcessDirectory::InProcessDirectory(vtworkspace::SessionModel const& model,
                                       InProcessWindow& owner,
                                       window::PaneStatusLookup const& status,
                                       WorkspaceNames const& names,
                                       vthost::WorkspaceGitTracker const* git,
                                       window::DirectoryChanged changed):
    _model { model },
    _owner { owner },
    _status { status },
    _names { names },
    _git { git },
    _changed { std::move(changed) }
{
}

window::WorkspaceDirectory InProcessDirectory::directory() const
{
    auto const tree = _model.captureTree();
    auto layouts = std::vector<vthost::proto::LayoutState> {};
    // In the model's order, which is the order a reorder of workspaces changes.
    auto const owned = _owner.ownedWorkspaces();
    for (auto const& workspace: tree.workspaces)
        if (std::ranges::contains(owned, workspace.id))
            layouts.push_back(layoutOf(_model, workspace));
    auto const meta = MetaOfProcess { _model, _names, _git };
    return window::directoryFromLayouts(layouts, _status, meta, _owner.displayedWorkspace().value);
}

window::SelectResult InProcessDirectory::select(uint64_t workspace)
{
    auto const target = vtworkspace::WorkspaceId { workspace };
    if (!std::ranges::contains(_owner.ownedWorkspaces(), target))
        return window::SelectResult::Refused;
    _owner.displayWorkspace(target);
    return window::SelectResult::Requested;
}

void InProcessDirectory::notifyChanged() const
{
    if (_changed)
        _changed();
}

} // namespace contour::session
