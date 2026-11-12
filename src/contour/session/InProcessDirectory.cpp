// SPDX-License-Identifier: Apache-2.0
#include <contour/session/InProcessDirectory.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

#include <vthost/proto/Pdu.hpp>
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

    vthost::proto::LayoutState layoutOf(vtworkspace::WorkspaceState const& workspace)
    {
        auto layout = vthost::proto::LayoutState {};
        layout.workspace = workspace.id.value;
        layout.activeTab = static_cast<uint32_t>(std::max(workspace.activeTabIndex, 0));
        for (auto const& tab: workspace.tabs)
        {
            auto wire = vthost::proto::WireTab {};
            wire.tabId = tab.id.value;
            wire.title = tab.view.title.value_or(std::string {});
            wire.root = wirePaneOf(tab.root);
            layout.tabs.push_back(std::move(wire));
        }
        return layout;
    }

    /// What an in-process workspace states of itself: its label and its git state; it has no control id.
    class MetaOfProcess final: public window::WorkspaceMetaLookup
    {
      public:
        MetaOfProcess(WorkspaceLabels const& labels, vthost::WorkspaceGitTracker const* git):
            _labels { labels }, _git { git }
        {
        }

        [[nodiscard]] window::WorkspaceMetadata metaOf(uint64_t workspace) const override
        {
            auto const id = vtworkspace::WorkspaceId { workspace };
            auto stated = window::WorkspaceMetadata { .label = _labels.labelOf(id) };
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
        WorkspaceLabels const& _labels;
        vthost::WorkspaceGitTracker const* _git;
    };
} // namespace

void WorkspaceLabels::set(vtworkspace::WorkspaceId workspace, std::string label)
{
    if (label.empty())
        _labels.erase(workspace.value);
    else
        _labels.insert_or_assign(workspace.value, std::move(label));
}

std::string WorkspaceLabels::labelOf(vtworkspace::WorkspaceId workspace) const
{
    auto const found = _labels.find(workspace.value);
    return found != _labels.end() ? found->second : std::string {};
}

void WorkspaceLabels::forget(vtworkspace::WorkspaceId workspace)
{
    _labels.erase(workspace.value);
}

InProcessDirectory::InProcessDirectory(vtworkspace::SessionModel const& model,
                                       InProcessWindow& owner,
                                       window::PaneStatusLookup const& status,
                                       WorkspaceLabels const& labels,
                                       vthost::WorkspaceGitTracker const* git,
                                       window::DirectoryChanged changed):
    _model { model },
    _owner { owner },
    _status { status },
    _labels { labels },
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
            layouts.push_back(layoutOf(workspace));
    auto const meta = MetaOfProcess { _labels, _git };
    return window::directoryFromLayouts(layouts, _status, meta, _owner.displayedWorkspace().value);
}

void InProcessDirectory::select(uint64_t workspace)
{
    _owner.displayWorkspace(vtworkspace::WorkspaceId { workspace });
}

void InProcessDirectory::notifyChanged() const
{
    if (_changed)
        _changed();
}

} // namespace contour::session
