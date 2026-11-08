// SPDX-License-Identifier: Apache-2.0
#include <vtworkspace/SessionModel.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace vtworkspace
{

// {{{ Workspace

Tab* Workspace::activeTab() const noexcept
{
    return tabAt(_activeTabIndex);
}

Tab* Workspace::previousActiveTab() const noexcept
{
    return tabAt(_previousActiveTabIndex);
}

Tab* Workspace::tabAt(int index) const noexcept
{
    if (index < 0 || std::cmp_greater_equal(index, _tabs.size()))
        return nullptr;
    return _tabs[static_cast<size_t>(index)].get();
}

int Workspace::indexOf(TabId tab) const noexcept
{
    auto const it = std::ranges::find_if(_tabs, [tab](auto const& t) { return t->id() == tab; });
    return it != _tabs.end() ? static_cast<int>(std::distance(_tabs.begin(), it)) : -1;
}

int Workspace::indexOf(Tab const* tab) const noexcept
{
    auto const it = std::ranges::find_if(_tabs, [tab](auto const& t) { return t.get() == tab; });
    return it != _tabs.end() ? static_cast<int>(std::distance(_tabs.begin(), it)) : -1;
}

// }}}

namespace
{
    /// The default predefined tab-color palette (a WT-style grid of swatches): the eight standard
    /// ANSI colors plus their bright variants. Data-driven so the set can be tuned in one place.
    constexpr vtbackend::RGBColor DefaultPalette[] = {
        vtbackend::RGBColor { 0xCC, 0x33, 0x33 }, // red
        vtbackend::RGBColor { 0xCC, 0x77, 0x33 }, // orange
        vtbackend::RGBColor { 0xCC, 0xCC, 0x33 }, // yellow
        vtbackend::RGBColor { 0x33, 0xCC, 0x33 }, // green
        vtbackend::RGBColor { 0x33, 0xCC, 0xCC }, // cyan
        vtbackend::RGBColor { 0x33, 0x77, 0xCC }, // blue
        vtbackend::RGBColor { 0x77, 0x33, 0xCC }, // purple
        vtbackend::RGBColor { 0xCC, 0x33, 0xCC }, // magenta
        vtbackend::RGBColor { 0xEE, 0x66, 0x66 }, // bright red
        vtbackend::RGBColor { 0xEE, 0xAA, 0x66 }, // bright orange
        vtbackend::RGBColor { 0xEE, 0xEE, 0x66 }, // bright yellow
        vtbackend::RGBColor { 0x66, 0xEE, 0x66 }, // bright green
        vtbackend::RGBColor { 0x66, 0xEE, 0xEE }, // bright cyan
        vtbackend::RGBColor { 0x66, 0xAA, 0xEE }, // bright blue
        vtbackend::RGBColor { 0xAA, 0x66, 0xEE }, // bright purple
        vtbackend::RGBColor { 0xEE, 0x66, 0xEE }, // bright magenta
    };
} // namespace

// {{{ SessionModel ctor

SessionModel::SessionModel(ModelEvents& events, SessionAllocator allocator):
    _events { events }, _allocateSession { std::move(allocator) }
{
}

std::span<vtbackend::RGBColor const> SessionModel::colorPalette() const noexcept
{
    // View over the single constexpr table (static storage duration) — no per-instance copy/allocation.
    return DefaultPalette;
}

// }}}
// {{{ Snapshot & restore

namespace
{
    PaneTreeState captureNode(Pane const& pane)
    {
        auto state = PaneTreeState { .id = pane.id(),
                                     .split = pane.splitState(),
                                     .ratio = pane.ratio(),
                                     .session = pane.isLeaf() ? pane.session() : SessionId {},
                                     .children = {} };
        if (!pane.isLeaf())
        {
            state.children.push_back(captureNode(*pane.first()));
            state.children.push_back(captureNode(*pane.second()));
        }
        return state;
    }

    std::unique_ptr<Pane> buildNode(PaneTreeState const& state)
    {
        if (state.split == SplitState::None)
            return std::make_unique<Pane>(state.id, state.session);
        return Pane::join(
            state.id, state.split, state.ratio, buildNode(state.children[0]), buildNode(state.children[1]));
    }

    /// What restore() checks a state against, and the ids it has met so far.
    struct RestoreValidator
    {
        ModelIdState ids;
        std::unordered_set<uint64_t> workspaces;
        std::unordered_set<uint64_t> tabs;
        std::unordered_set<uint64_t> panes;
        std::unordered_set<uint64_t> sessions;

        /// Notes @p id as met in @p seen, below @p counter.
        static std::expected<void, ModelRestoreError> note(std::unordered_set<uint64_t>& seen,
                                                           uint64_t id,
                                                           uint64_t counter)
        {
            if (id >= counter)
                return std::unexpected(ModelRestoreError::IdBeyondCounter);
            if (!seen.insert(id).second)
                return std::unexpected(ModelRestoreError::DuplicateId);
            return {};
        }

        /// Checks @p node and its subtree; every leaf met is added to @p leaves.
        std::expected<void, ModelRestoreError> checkNode(PaneTreeState const& node,
                                                         std::vector<PaneId>& leaves)
        {
            auto const expectedChildren =
                node.split == SplitState::None ? std::size_t { 0 } : std::size_t { 2 };
            if (node.children.size() != expectedChildren
                || (node.split != SplitState::None && !std::isfinite(node.ratio)))
                return std::unexpected(ModelRestoreError::MalformedTree);
            if (auto noted = note(panes, node.id.value, ids.nextPane.value); !noted)
                return noted;
            if (node.split == SplitState::None)
            {
                leaves.push_back(node.id);
                if (node.session == SessionId {})
                    return std::unexpected(ModelRestoreError::MissingSession);
                if (!sessions.insert(node.session.value).second)
                    return std::unexpected(ModelRestoreError::DuplicateId);
            }
            for (auto const& child: node.children)
                if (auto checked = checkNode(child, leaves); !checked)
                    return checked;
            return {};
        }

        std::expected<void, ModelRestoreError> checkTab(TabState const& tab)
        {
            if (auto noted = note(tabs, tab.id.value, ids.nextTab.value); !noted)
                return noted;
            auto leaves = std::vector<PaneId> {};
            if (auto checked = checkNode(tab.root, leaves); !checked)
                return checked;
            if (!std::ranges::contains(leaves, tab.view.activePane))
                return std::unexpected(ModelRestoreError::UnknownActivePane);
            return {};
        }

        std::expected<void, ModelRestoreError> checkWorkspace(WorkspaceState const& workspace)
        {
            if (auto noted = note(workspaces, workspace.id.value, ids.nextWorkspace.value); !noted)
                return noted;
            auto const count = static_cast<int>(workspace.tabs.size());
            auto const inRange = [count](int index) {
                return index >= -1 && index < count;
            };
            if (!inRange(workspace.activeTabIndex) || !inRange(workspace.previousActiveTabIndex)
                || (count > 0 && workspace.activeTabIndex < 0))
                return std::unexpected(ModelRestoreError::BadTabIndex);
            for (auto const& tab: workspace.tabs)
                if (auto checked = checkTab(tab); !checked)
                    return checked;
            return {};
        }
    };
} // namespace

SessionId firstUnusedSessionId(WorkspaceTreeState const& tree)
{
    auto const highest = [](this auto const& self, PaneTreeState const& node) -> uint64_t {
        auto value = node.split == SplitState::None ? node.session.value : uint64_t { 0 };
        for (auto const& child: node.children)
            value = std::max(value, self(child));
        return value;
    };
    auto value = uint64_t { 0 };
    for (auto const& workspace: tree.workspaces)
        for (auto const& tab: workspace.tabs)
            value = std::max(value, highest(tab.root));
    return SessionId { value + 1 };
}

WorkspaceTreeState SessionModel::captureTree() const
{
    auto tree = WorkspaceTreeState {};
    for (auto const& workspace: _workspaces)
    {
        auto state = WorkspaceState { .id = workspace->id(),
                                      .tabs = {},
                                      .activeTabIndex = workspace->_activeTabIndex,
                                      .previousActiveTabIndex = workspace->_previousActiveTabIndex };
        for (auto const& tab: workspace->_tabs)
            state.tabs.push_back(TabState {
                .id = tab->id(), .root = captureNode(*tab->rootPane()), .view = tab->viewState() });
        tree.workspaces.push_back(std::move(state));
    }
    return tree;
}

ModelIdState SessionModel::idState() const noexcept
{
    return ModelIdState { .nextWorkspace = _nextWorkspaceId, .nextTab = _nextTabId, .nextPane = _nextPaneId };
}

std::expected<void, ModelRestoreError> SessionModel::restore(WorkspaceTreeState const& tree,
                                                             ModelIdState const& ids)
{
    Require(_workspaces.empty());

    auto validator =
        RestoreValidator { .ids = ids, .workspaces = {}, .tabs = {}, .panes = {}, .sessions = {} };
    for (auto const& workspace: tree.workspaces)
        if (auto checked = validator.checkWorkspace(workspace); !checked)
            return checked;

    for (auto const& workspaceState: tree.workspaces)
    {
        auto workspace = std::make_unique<Workspace>(workspaceState.id);
        for (auto const& tabState: workspaceState.tabs)
            workspace->_tabs.push_back(
                std::make_unique<Tab>(tabState.id, buildNode(tabState.root), tabState.view));
        workspace->_activeTabIndex = workspaceState.activeTabIndex;
        workspace->_previousActiveTabIndex = workspaceState.previousActiveTabIndex;
        _workspaces.push_back(std::move(workspace));
    }
    _nextWorkspaceId = ids.nextWorkspace;
    _nextTabId = ids.nextTab;
    _nextPaneId = ids.nextPane;
    return {};
}

// }}}
// {{{ Workspaces

Workspace* SessionModel::createWorkspace()
{
    auto workspace = std::make_unique<Workspace>(_nextWorkspaceId++);
    _workspaces.push_back(std::move(workspace));
    return _workspaces.back().get();
}

void SessionModel::removeWorkspace(WorkspaceId workspaceId)
{
    auto* win = workspace(workspaceId);
    if (win == nullptr)
        return;

    // Close every tab (reporting sessions closed) from the back so indices stay valid.
    while (!win->_tabs.empty())
        closeTabAt(*win, static_cast<int>(win->_tabs.size()) - 1);

    std::erase_if(_workspaces, [workspaceId](auto const& w) { return w->id() == workspaceId; });
}

Workspace* SessionModel::workspace(WorkspaceId id) const noexcept
{
    auto const it = std::ranges::find_if(_workspaces, [id](auto const& w) { return w->id() == id; });
    return it != _workspaces.end() ? it->get() : nullptr;
}

// }}}
// {{{ Tabs

Tab* SessionModel::createTab(WorkspaceId workspaceId)
{
    auto* win = workspace(workspaceId);
    assert(win != nullptr);

    auto const tabId = _nextTabId++;
    auto const rootPaneId = _nextPaneId++;
    auto const session = _allocateSession();

    return insertTab(
        *win, static_cast<int>(win->_tabs.size()), std::make_unique<Tab>(tabId, rootPaneId, session));
}

Tab* SessionModel::insertTab(Workspace& win, int requestedIndex, std::unique_ptr<Tab> tab)
{
    auto const workspaceId = win.id();
    auto const tabId = tab->id();
    auto const index = std::clamp(requestedIndex, 0, static_cast<int>(win._tabs.size()));

    // Bracket the insertion so a Qt host's beginInsertRows() runs BEFORE the tab set grows (its contract
    // requires rowCount() to still report the old count at that point) and endInsertRows() after.
    _events.tabAboutToBeAdded(workspaceId, index);
    win._tabs.insert(win._tabs.begin() + index, std::move(tab));
    // Tabs at or after the new slot moved up by one; keep the active/previous slots on the same tabs.
    if (win._activeTabIndex >= index)
        ++win._activeTabIndex;
    if (win._previousActiveTabIndex >= index)
        ++win._previousActiveTabIndex;
    _events.tabAdded(workspaceId, tabId, index);

    // A new tab is a genuine activation: remember the outgoing active slot as the previous tab.
    win._previousActiveTabIndex = win._activeTabIndex;
    win._activeTabIndex = index;
    _events.activeTabChanged(workspaceId, tabId, index);

    return win._tabs[static_cast<size_t>(index)].get();
}

void SessionModel::closeTabAt(Workspace& win, int index, std::function<void()> const& beforeErase)
{
    assert(index >= 0 && std::cmp_less(index, win._tabs.size()));
    auto const tabId = win._tabs[static_cast<size_t>(index)]->id();

    auto const previousActiveIndex = win._activeTabIndex;
    // Bracket the removal: a Qt host's beginRemoveRows() must run while the tab is still present (old
    // rowCount), endRemoveRows() after the erase.
    _events.tabAboutToBeRemoved(win.id(), index);
    if (beforeErase)
        beforeErase();
    win._tabs.erase(win._tabs.begin() + index);
    _events.tabClosed(win.id(), tabId, index);

    // Translate/invalidate the "previous active tab" slot the same way as the active slot below: it must
    // never dangle at a wrong tab. If the closed tab WAS the previous one, there is no valid previous
    // anymore (-1); if it was before the previous slot, that slot shifted down by one.
    if (win._previousActiveTabIndex == index)
        win._previousActiveTabIndex = -1;
    else if (index < win._previousActiveTabIndex)
        --win._previousActiveTabIndex;

    // Keep the active index pointing at the *same* surviving tab, and notify only if it changed.
    if (win._tabs.empty())
    {
        win._activeTabIndex = -1;
        win._previousActiveTabIndex = -1;
        return;
    }

    // Erasing at @p index shifts every tab after it down by one slot. There are three cases:
    //  - the erased tab was after the active one  -> the active tab kept its slot (no change);
    //  - the erased tab was before the active one  -> the active tab shifted down by one (decrement);
    //  - the active tab itself was erased          -> adopt the tab now occupying that slot, clamped
    //                                                 to the new last index.
    auto newActive = win._activeTabIndex;
    if (index < win._activeTabIndex)
        --newActive;
    else if (index == win._activeTabIndex)
        newActive = std::min(newActive, static_cast<int>(win._tabs.size()) - 1);
    newActive = std::max(newActive, 0);

    win._activeTabIndex = newActive;
    if (newActive != previousActiveIndex || index == previousActiveIndex)
        _events.activeTabChanged(win.id(), win._tabs[static_cast<size_t>(newActive)]->id(), newActive);
}

void SessionModel::closeTab(WorkspaceId workspaceId, TabId tabId)
{
    auto* win = workspace(workspaceId);
    if (win == nullptr)
        return;
    auto const index = win->indexOf(tabId);
    if (index < 0)
        return;
    closeTabAt(*win, index);
}

void SessionModel::closeOtherTabs(WorkspaceId workspaceId, TabId keep)
{
    auto* win = workspace(workspaceId);
    if (win == nullptr)
        return;
    // Iterate back-to-front so each erasure leaves the lower indices valid.
    for (auto const i: std::views::iota(0, static_cast<int>(win->_tabs.size())) | std::views::reverse)
        if (win->_tabs[static_cast<size_t>(i)]->id() != keep)
            closeTabAt(*win, i);
}

void SessionModel::closeTabsToRight(WorkspaceId workspaceId, TabId anchor)
{
    auto* win = workspace(workspaceId);
    if (win == nullptr)
        return;
    auto const anchorIndex = win->indexOf(anchor);
    if (anchorIndex < 0)
        return;
    // Iterate back-to-front so each erasure leaves the lower indices valid.
    for (auto const i:
         std::views::iota(anchorIndex + 1, static_cast<int>(win->_tabs.size())) | std::views::reverse)
        closeTabAt(*win, i);
}

void SessionModel::activateTab(WorkspaceId workspaceId, TabId tabId)
{
    auto* win = workspace(workspaceId);
    if (win == nullptr)
        return;
    auto const index = win->indexOf(tabId);
    if (index < 0 || index == win->_activeTabIndex)
        return;
    // Switching tabs is the canonical activation: the outgoing tab becomes the "previous" one, so a
    // subsequent switch-to-previous toggles back to it.
    win->_previousActiveTabIndex = win->_activeTabIndex;
    win->_activeTabIndex = index;
    _events.activeTabChanged(workspaceId, tabId, index);
}

void SessionModel::moveTab(WorkspaceId workspaceId, TabId tabId, int toIndex)
{
    auto* win = workspace(workspaceId);
    if (win == nullptr)
        return;
    auto const from = win->indexOf(tabId);
    // Guard before clamping: on an empty/unknown-tab list indexOf returns -1, and clamping with a
    // hi bound of size()-1 == -1 would call std::clamp with lo > hi (undefined behavior; aborts
    // under hardened/_GLIBCXX_ASSERTIONS or libc++ debug builds).
    if (from < 0)
        return;
    auto const to = std::clamp(toIndex, 0, static_cast<int>(win->_tabs.size()) - 1);
    if (from == to)
        return;

    auto* activeTab = win->activeTab();
    auto const previousActiveIndex = win->_activeTabIndex;
    // Track the "previous active tab" by object too, so the reorder preserves which tab it names rather
    // than a now-stale slot (mirrors the active-tab handling below).
    auto* previousTab = win->previousActiveTab();

    // Bracket the reorder: a Qt host's beginMoveRows() must run before the vector is mutated (it reads
    // the pre-move layout to validate the destination), endMoveRows() after.
    _events.tabAboutToBeMoved(workspaceId, from, to);
    auto held = std::move(win->_tabs[static_cast<size_t>(from)]);
    win->_tabs.erase(win->_tabs.begin() + from);
    win->_tabs.insert(win->_tabs.begin() + to, std::move(held));

    // Preserve which *tab* is active (and which is the previous) across the reorder.
    if (activeTab != nullptr)
        win->_activeTabIndex = win->indexOf(activeTab);
    win->_previousActiveTabIndex = previousTab != nullptr ? win->indexOf(previousTab) : -1;

    _events.tabMoved(workspaceId, tabId, from, to);

    // The active tab object is unchanged, but the reorder can shift its index (e.g. moving a tab
    // across the active one). Observers that track the active tab by index — not by the tabMoved
    // delta — need an explicit activeTabChanged to avoid a stale active-tab index.
    if (activeTab != nullptr && win->_activeTabIndex != previousActiveIndex)
        _events.activeTabChanged(workspaceId, activeTab->id(), win->_activeTabIndex);
}

void SessionModel::moveTabToWorkspace(WorkspaceId fromId, TabId tabId, WorkspaceId toId, int toIndex)
{
    // Same-workspace request degenerates to a plain reorder — reuse the tested single-workspace path so the
    // active-index translation and beginMoveRows bracketing stay in one place.
    if (fromId == toId)
    {
        moveTab(fromId, tabId, toIndex);
        return;
    }

    auto* from = workspace(fromId);
    auto* to = workspace(toId);
    if (from == nullptr || to == nullptr)
        return;
    auto const fromIndex = from->indexOf(tabId);
    if (fromIndex < 0)
        return; // tab not in the source workspace

    // Snapshot the source's active/previous tab OBJECTS so their indices can be re-derived after the
    // erase (mirrors closeTabAt); the destination gains the tab as its new active.
    auto* srcActiveTab = from->activeTab();
    auto* srcPreviousTab = from->previousActiveTab();
    auto const dstIndex = std::clamp(toIndex, 0, static_cast<int>(to->_tabs.size()));

    _events.tabAboutToBeMovedToWorkspace(fromId, fromIndex, toId, dstIndex);

    // Transplant the Tab intact — its pane subtree and the sessions it carries travel with it; nothing
    // is torn down (unlike closeTab/removeWorkspace, which report sessions closed).
    auto held = std::move(from->_tabs[static_cast<size_t>(fromIndex)]);
    from->_tabs.erase(from->_tabs.begin() + fromIndex);
    to->_tabs.insert(to->_tabs.begin() + dstIndex, std::move(held));
    assert(from->indexOf(tabId) < 0 && "the moved tab must have left the source workspace");

    // Fix the SOURCE workspace's active/previous indices exactly as closeTabAt does after an erase.
    if (from->_previousActiveTabIndex == fromIndex)
        from->_previousActiveTabIndex = -1;
    else if (fromIndex < from->_previousActiveTabIndex)
        --from->_previousActiveTabIndex;

    bool const srcActiveWasMoved = (srcActiveTab == nullptr) || (from->indexOf(srcActiveTab) < 0);
    if (from->_tabs.empty())
    {
        from->_activeTabIndex = -1;
        from->_previousActiveTabIndex = -1;
    }
    else
    {
        // Re-derive the surviving active tab's slot; if the active tab itself was the one moved out,
        // adopt the tab now occupying its old slot (clamped), matching closeTabAt.
        if (srcActiveWasMoved)
            from->_activeTabIndex = std::min(from->_activeTabIndex, static_cast<int>(from->_tabs.size()) - 1);
        else
            from->_activeTabIndex = from->indexOf(srcActiveTab);
        from->_activeTabIndex = std::max(from->_activeTabIndex, 0);
        from->_previousActiveTabIndex = srcPreviousTab != nullptr ? from->indexOf(srcPreviousTab) : -1;
    }

    // The DESTINATION adopts the moved tab as its active tab (a genuine activation): remember the old
    // active slot as the previous tab, then point at the inserted slot.
    to->_previousActiveTabIndex = to->_activeTabIndex;
    to->_activeTabIndex = dstIndex;

    _events.tabMovedToWorkspace(fromId, tabId, fromIndex, toId, dstIndex);
    _events.activeTabChanged(toId, tabId, dstIndex);
    if (!from->_tabs.empty() && srcActiveWasMoved)
        _events.activeTabChanged(
            fromId, from->_tabs[static_cast<size_t>(from->_activeTabIndex)]->id(), from->_activeTabIndex);
}

Pane* SessionModel::mergeTabIntoSplit(
    WorkspaceId fromId, TabId sourceId, WorkspaceId toId, TabId targetId, PaneId leafId, SplitSide side)
{
    // Half and half: the grafted tab and the pane it lands beside share the space equally.
    constexpr double MergeRatio = 0.5;

    if (sourceId == targetId)
        return nullptr;
    auto* from = workspace(fromId);
    auto* to = workspace(toId);
    if (from == nullptr || to == nullptr)
        return nullptr;
    auto const sourceIndex = from->indexOf(sourceId);
    if (sourceIndex < 0 || to->indexOf(targetId) < 0)
        return nullptr;
    auto* source = from->tabAt(sourceIndex);
    auto* target = to->tabAt(to->indexOf(targetId));
    auto* leaf = target->rootPane()->findPane(leafId);
    if (leaf == nullptr || !leaf->isLeaf())
        return nullptr;

    auto const zoomBefore = target->zoomedLeafId();
    auto const splitNodeId = _nextPaneId++;
    // The source is announced as removed while it still holds its tree; its tree is taken just before it
    // is destroyed, so no session is reported closed.
    Pane* grafted = nullptr;
    closeTabAt(*from, sourceIndex, [&] {
        grafted = target->graftTab(*leaf, *source, side, splitNodeId, MergeRatio);
    });

    activateTab(toId, targetId);
    _events.paneTreeRestructured(targetId);
    _events.activePaneChanged(targetId, target->activePane()->id());
    // The merge turned a one-pane tab into a many-pane one, or cleared a zoom: retitle exactly once.
    if (!announceZoomChange(*target, zoomBefore))
        _events.tabTitleChanged(targetId);
    return grafted;
}

// }}}
// {{{ Moves

/// How much of its tab the dragged node is.
enum class SessionModel::SourceShape : uint8_t
{
    LeafOfMany = 0, //!< One pane of a tab that has other panes.
    WholeTab,       //!< A tab, with one pane or several.
};

/// The dragged node, resolved against the model.
struct SessionModel::MoveSourceNode
{
    Workspace* workspace = nullptr;
    Tab* tab = nullptr;
    Pane* leaf = nullptr; //!< The dragged leaf; null for a tab with several panes.
    SourceShape shape = SourceShape::LeafOfMany;
};

/// The drop place, resolved against the model.
struct SessionModel::MoveTargetNode
{
    Workspace* workspace = nullptr;
    Tab* tab = nullptr;
    Pane* leaf = nullptr; //!< The pane dropped beside or onto; null for a tab well or a new workspace.
};

std::expected<SessionModel::MoveSourceNode, MoveError> SessionModel::resolveMoveSource(
    MoveSource const& source) const
{
    if (auto const* pane = std::get_if<PaneSource>(&source))
    {
        auto const [workspace, tab, leaf] = findSessionLeaf(pane->session);
        if (leaf == nullptr)
            return std::unexpected(MoveError::UnknownSource);
        return MoveSourceNode { .workspace = workspace,
                                .tab = tab,
                                .leaf = leaf,
                                .shape =
                                    tab->isLastPane(leaf) ? SourceShape::WholeTab : SourceShape::LeafOfMany };
    }

    auto const& tabSource = std::get<TabSource>(source);
    auto* workspace = this->workspace(tabSource.workspace);
    auto* tab = workspace != nullptr ? workspace->tabAt(workspace->indexOf(tabSource.tab)) : nullptr;
    if (tab == nullptr)
        return std::unexpected(MoveError::UnknownSource);
    return MoveSourceNode { .workspace = workspace,
                            .tab = tab,
                            .leaf = tab->hasMultiplePanes() ? nullptr : tab->rootPane(),
                            .shape = SourceShape::WholeTab };
}

std::expected<SessionModel::MoveTargetNode, MoveError> SessionModel::resolveMoveTarget(
    MoveTarget const& target) const
{
    auto const resolveLeaf = [&](SessionId session) -> std::expected<MoveTargetNode, MoveError> {
        auto const [workspace, tab, leaf] = findSessionLeaf(session);
        if (leaf == nullptr)
            return std::unexpected(MoveError::UnknownTarget);
        return MoveTargetNode { .workspace = workspace, .tab = tab, .leaf = leaf };
    };

    if (auto const* edge = std::get_if<EdgeTarget>(&target))
        return resolveLeaf(edge->pane);
    if (auto const* swap = std::get_if<SwapTarget>(&target))
        return resolveLeaf(swap->pane);
    if (auto const* well = std::get_if<TabWellTarget>(&target))
    {
        auto* workspace = this->workspace(well->workspace);
        if (workspace == nullptr)
            return std::unexpected(MoveError::UnknownTarget);
        return MoveTargetNode { .workspace = workspace };
    }
    return MoveTargetNode {};
}

void SessionModel::announcePaneMove(Tab& tab, std::optional<PaneId> zoomedLeafBefore)
{
    if (!announceZoomChange(tab, zoomedLeafBefore))
        _events.tabTitleChanged(tab.id());
}

void SessionModel::announceSourceShrunk(Tab& tab, std::optional<PaneId> zoomedLeafBefore)
{
    _events.paneTreeRestructured(tab.id());
    _events.activePaneChanged(tab.id(), tab.activePane()->id());
    announcePaneMove(tab, zoomedLeafBefore);
}

std::expected<MoveResult, MoveError> SessionModel::moveNode(MoveSource const& source,
                                                            MoveTarget const& target)
{
    // {{{ Validate everything; nothing below this block may refuse.
    auto const src = resolveMoveSource(source);
    if (!src)
        return std::unexpected(src.error());
    auto const dst = resolveMoveTarget(target);
    if (!dst)
        return std::unexpected(dst.error());

    auto const dropsOnPane =
        std::holds_alternative<EdgeTarget>(target) || std::holds_alternative<SwapTarget>(target);
    if (dropsOnPane)
    {
        if (dst->leaf == src->leaf)
            return std::unexpected(MoveError::SelfTarget);
        if (src->shape == SourceShape::WholeTab && dst->tab == src->tab)
            return std::unexpected(MoveError::SelfTarget);
        if (std::holds_alternative<SwapTarget>(target) && src->leaf == nullptr)
            return std::unexpected(MoveError::TabIntoSwap);
    }
    if (auto const* well = std::get_if<TabWellTarget>(&target);
        well != nullptr && src->shape == SourceShape::WholeTab && dst->workspace == src->workspace)
    {
        auto const lastIndex = src->workspace->tabCount() - 1;
        if (std::clamp(well->index, 0, lastIndex) == src->workspace->indexOf(src->tab))
            return std::unexpected(MoveError::NoOp);
    }
    // A workspace of its own for what already is all of one would tear that workspace down to build it again.
    if (std::holds_alternative<NewWorkspaceTarget>(target) && fillsItsWorkspace(*src))
        return std::unexpected(MoveError::NoOp);
    // }}}

    return std::visit(
        [&](auto const& drop) -> std::expected<MoveResult, MoveError> {
            using Drop = std::decay_t<decltype(drop)>;
            if constexpr (std::is_same_v<Drop, EdgeTarget>)
                return moveBeside(*src, *dst, drop.side);
            else if constexpr (std::is_same_v<Drop, SwapTarget>)
                return swapWith(*src, *dst);
            else if constexpr (std::is_same_v<Drop, TabWellTarget>)
                return moveIntoWell(*src, *dst->workspace, drop.index);
            else
                return moveIntoWell(*src, *createWorkspace(), 0); // a workspace of its own, at its first slot
        },
        target);
}

bool SessionModel::fillsItsWorkspace(MoveSourceNode const& src) noexcept
{
    return src.shape == SourceShape::WholeTab && src.workspace->tabCount() == 1;
}

bool SessionModel::fillsItsWorkspace(MoveSource const& source) const
{
    auto const src = resolveMoveSource(source);
    return src && fillsItsWorkspace(*src);
}

std::expected<MoveResult, MoveError> SessionModel::moveBeside(MoveSourceNode const& src,
                                                              MoveTargetNode const& dst,
                                                              SplitSide side)
{
    // Half and half: the dropped node and the pane it lands beside share the space equally.
    constexpr double EdgeRatio = 0.5;

    // Captured now: the tabs and panes below are rearranged, and the objects may not survive it.
    auto const srcTabId = src.tab->id();
    auto const dstWorkspaceId = dst.workspace->id();
    auto const dstTabId = dst.tab->id();
    auto const movedSession = src.leaf != nullptr ? src.leaf->session() : src.tab->activePane()->session();

    if (src.shape == SourceShape::WholeTab)
    {
        // Refused before it changes anything, as everything moveNode() validated must be.
        if (mergeTabIntoSplit(src.workspace->id(), srcTabId, dstWorkspaceId, dstTabId, dst.leaf->id(), side)
            == nullptr)
            return std::unexpected(MoveError::UnknownTarget);
        return MoveResult { .workspace = dstWorkspaceId, .tab = dstTabId, .focused = movedSession };
    }

    auto const srcZoomBefore = src.tab->zoomedLeafId();
    auto const dstZoomBefore = dst.tab->zoomedLeafId();
    auto const dstSession = dst.leaf->session();
    auto const leafId = src.leaf->id();
    auto detached = src.tab->detachLeaf(*src.leaf);
    // Detaching folds the leaf's sibling into its parent, which may have been the drop pane's node.
    auto* dropPane = dst.tab->rootPane()->findLeaf(dstSession);
    if (dropPane == nullptr)
    {
        // Not reached: the drop pane is another leaf than the detached one, and a detach keeps every other
        // leaf's session. Were it reached, the detached pane would be lost with its session; it becomes a
        // tab of its own instead, so every session is still shown, and the move is reported as not landed.
        announceSourceShrunk(*src.tab, srcZoomBefore);
        (void) insertTab(*dst.workspace,
                         dst.workspace->tabCount(),
                         std::make_unique<Tab>(_nextTabId++, leafId, movedSession));
        return std::unexpected(MoveError::UnknownTarget);
    }
    (void) dst.tab->graftLeaf(*dropPane, std::move(detached), side, _nextPaneId++, EdgeRatio);

    if (src.tab != dst.tab)
        announceSourceShrunk(*src.tab, srcZoomBefore);
    activateTab(dstWorkspaceId, dstTabId);
    _events.paneTreeRestructured(dstTabId);
    _events.activePaneChanged(dstTabId, leafId);
    announcePaneMove(*dst.tab, dstZoomBefore);
    return MoveResult { .workspace = dstWorkspaceId, .tab = dstTabId, .focused = movedSession };
}

MoveResult SessionModel::swapWith(MoveSourceNode const& src, MoveTargetNode const& dst)
{
    auto const srcTabId = src.tab->id();
    auto const srcZoomBefore = src.tab->zoomedLeafId();
    auto const dstWorkspaceId = dst.workspace->id();
    auto const dstTabId = dst.tab->id();
    auto const dstZoomBefore = dst.tab->zoomedLeafId();
    auto const srcLeafId = src.leaf->id();
    auto const dstLeafId = dst.leaf->id();
    auto const movedSession = src.leaf->session();

    src.tab->clearZoom();
    dst.tab->clearZoom();
    src.leaf->swapLeafPayload(dst.leaf);
    dst.tab->setActivePane(dst.leaf);

    if (src.tab == dst.tab)
        _events.paneSwapped(dstTabId, srcLeafId, dstLeafId);
    else
    {
        // Sessions trade between two trees: every observer re-reads both wholesale (paneSwapped
        // names one tab only, and paneSessionReplaced means a restart in place).
        _events.paneTreeRestructured(srcTabId);
        _events.paneTreeRestructured(dstTabId);
        // The source tab's active pane now carries another session, so its title may have changed too.
        announcePaneMove(*src.tab, srcZoomBefore);
    }
    activateTab(dstWorkspaceId, dstTabId);
    _events.activePaneChanged(dstTabId, dstLeafId);
    announcePaneMove(*dst.tab, dstZoomBefore);
    return MoveResult { .workspace = dstWorkspaceId, .tab = dstTabId, .focused = movedSession };
}

MoveResult SessionModel::moveIntoWell(MoveSourceNode const& src, Workspace& workspace, int index)
{
    auto const srcTabId = src.tab->id();
    auto const srcZoomBefore = src.tab->zoomedLeafId();
    auto const workspaceId = workspace.id();
    auto const movedSession = src.leaf != nullptr ? src.leaf->session() : src.tab->activePane()->session();

    if (src.shape == SourceShape::WholeTab)
    {
        src.tab->clearZoom();
        moveTabToWorkspace(src.workspace->id(), srcTabId, workspaceId, index);
        activateTab(workspaceId, srcTabId); // a reorder within one workspace does not activate by itself
        _events.activePaneChanged(srcTabId, src.tab->activePane()->id());
        // A reorder retitles nothing; only a cleared zoom does.
        (void) announceZoomChange(*src.tab, srcZoomBefore);
        return MoveResult { .workspace = workspaceId, .tab = srcTabId, .focused = movedSession };
    }

    // One pane of several becomes a tab of its own, keeping its PaneId and session.
    auto const leafId = src.leaf->id();
    auto const newTabId = _nextTabId++;
    auto detached = src.tab->detachLeaf(*src.leaf);
    // The new tab first, then the shrunk source: an observer told of either finds the moved session in a
    // tab, as with a graft beside a pane of another tab (moveBeside).
    (void) insertTab(workspace, index, std::make_unique<Tab>(newTabId, leafId, detached->session()));
    announceSourceShrunk(*src.tab, srcZoomBefore);
    _events.activePaneChanged(newTabId, leafId);
    return MoveResult { .workspace = workspaceId, .tab = newTabId, .focused = movedSession };
}

std::expected<void, ArrangeError> SessionModel::arrangeTab(TabId tabId, PaneArrangement const& shape)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return std::unexpected(ArrangeError::UnknownTab);
    auto named = std::vector<uint64_t> {};
    auto const collect = [&named](this auto const& self, PaneArrangement const& node) -> bool {
        if (node.split == SplitState::None)
        {
            named.push_back(node.session.value);
            return node.children.empty();
        }
        return node.children.size() == 2 && self(node.children[0]) && self(node.children[1]);
    };
    if (!collect(shape))
        return std::unexpected(ArrangeError::Malformed);
    auto shown = std::ranges::to<std::vector>(tab->leafSessions()
                                              | std::views::transform([](SessionId s) { return s.value; }));
    std::ranges::sort(named);
    std::ranges::sort(shown);
    if (named != shown)
        return std::unexpected(ArrangeError::OtherSessions);
    if (tab->isArrangedAs(shape))
        return {};

    auto const zoomBefore = tab->zoomedLeafId();
    tab->arrange(shape);
    // The active pane is the leaf it was, so focus stays; only the tree around it is new.
    _events.paneTreeRestructured(tabId);
    (void) announceZoomChange(*tab, zoomBefore);
    return {};
}

// }}}
// {{{ Panes

std::pair<Workspace*, int> SessionModel::locateTab(TabId tabId) const noexcept
{
    for (auto const& win: _workspaces)
        if (auto const index = win->indexOf(tabId); index >= 0)
            return { win.get(), index };
    return { nullptr, -1 };
}

Tab* SessionModel::findTab(TabId tabId) const noexcept
{
    auto const [win, index] = locateTab(tabId);
    return win != nullptr ? win->tabAt(index) : nullptr;
}

WorkspaceId SessionModel::workspaceOfTab(TabId tabId) const noexcept
{
    auto const [win, index] = locateTab(tabId);
    return win != nullptr ? win->id() : WorkspaceId {};
}

std::tuple<Workspace*, Tab*, Pane*> SessionModel::findSessionLeaf(SessionId session) const
{
    for (auto const& win: _workspaces)
        for (auto const& tab: win->_tabs)
            if (auto* leaf = tab->rootPane()->findLeaf(session); leaf != nullptr)
                return { win.get(), tab.get(), leaf };
    return { nullptr, nullptr, nullptr };
}

Pane* SessionModel::findLeafPane(PaneId pane) const
{
    for (auto const& win: _workspaces)
        for (auto const& tab: win->_tabs)
            if (auto* leaf = tab->rootPane()->walkTree(
                    [pane](Pane& node) { return node.isLeaf() && node.id() == pane; });
                leaf != nullptr)
                return leaf;
    return nullptr;
}

Pane* SessionModel::findPane(TabId tabId, PaneId paneId) const
{
    auto* tab = findTab(tabId);
    return tab != nullptr ? tab->rootPane()->findPane(paneId) : nullptr;
}

Pane* SessionModel::splitActivePane(TabId tabId, SplitState direction, double ratio)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return nullptr;

    auto const splitNodeId = _nextPaneId++;
    auto const newLeafId = _nextPaneId++;
    auto const newSession = _allocateSession();

    auto const zoomBefore = tab->zoomedLeafId();
    auto* newLeaf = tab->splitActivePane(direction, splitNodeId, newLeafId, newSession, ratio);
    _events.paneSplit(tabId, splitNodeId, newLeafId);
    _events.activePaneChanged(tabId, newLeaf->id());
    // The split may retitle the tab (single-pane -> "Multiple panes"); so does the zoom it cleared.
    // Either way the tab is retitled exactly once.
    if (!announceZoomChange(*tab, zoomBefore))
        _events.tabTitleChanged(tabId);
    return newLeaf;
}

void SessionModel::closePane(WorkspaceId workspaceId, TabId tabId, PaneId leafId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto* leaf = tab->rootPane()->findPane(leafId);
    if (leaf == nullptr || !leaf->isLeaf())
        return;

    if (tab->isLastPane(leaf))
    {
        // Closing the only pane closes the whole tab.
        closeTab(workspaceId, tabId);
        return;
    }

    auto* parent = leaf->parent();
    auto* survivor = (parent->first() == leaf) ? parent->second() : parent->first();
    auto const survivorId = survivor->id();

    // Safe to announce zoom below: the last-pane branch above already returned, so the tab survives
    // closePane() and can still be asked for its zoom state afterwards.
    auto const zoomBefore = tab->zoomedLeafId();
    (void) tab->closePane(leaf);

    _events.paneClosed(tabId, leafId, survivorId);
    _events.activePaneChanged(tabId, tab->activePane()->id());
    // The close may retitle the tab (down to one pane -> that pane's name); so does the zoom it
    // cleared. Either way the tab is retitled exactly once.
    if (!announceZoomChange(*tab, zoomBefore))
        _events.tabTitleChanged(tabId);
}

void SessionModel::setActivePane(TabId tabId, PaneId leafId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto* leaf = tab->rootPane()->findPane(leafId);
    if (leaf == nullptr || !leaf->isLeaf() || leaf == tab->activePane())
        return;
    // NB: no unconditional tabTitleChanged here. Unzoomed, the resolved title does not depend on which
    // pane is active (precedence: runtime > "Multiple panes" > active session), and a single-pane tab's
    // active pane cannot change — the early-return above fires. Zoomed, it does: the tab is named after
    // the pane on screen, and announceZoomChange() emits the retitle when the zoom carries to this leaf.
    auto const zoomBefore = tab->zoomedLeafId();
    tab->setActivePane(leaf);
    _events.activePaneChanged(tabId, leafId);
    announceZoomChange(*tab, zoomBefore);
}

void SessionModel::focusDirection(TabId tabId, FocusDirection direction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;

    auto const zoomBefore = tab->zoomedLeafId();
    auto* target = tab->focusDirection(direction);
    if (target == nullptr)
        return; // no neighbor: nothing moved, so the zoom did not either
    _events.activePaneChanged(tabId, target->id());
    announceZoomChange(*tab, zoomBefore);
}

void SessionModel::setPaneRatio(TabId tabId, PaneId splitNodeId, double ratio)
{
    auto* node = findPane(tabId, splitNodeId);
    if (node == nullptr || node->isLeaf())
        return;
    node->setRatio(ratio);
    // Emit the CLAMPED value the node actually stored (setRatio() clamps to [MinimumRatio, 1-MinimumRatio]),
    // not the raw request — otherwise the GUI divider and the model disagree at the extremes (a drag to the
    // edge would echo back an out-of-range ratio the model never adopted).
    _events.paneRatioChanged(tabId, splitNodeId, node->ratio());
}

void SessionModel::resizeActivePane(TabId tabId, FocusDirection direction, double fraction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto const zoomBefore = tab->zoomedLeafId();
    auto* split = tab->resizeActivePane(direction, fraction);
    if (split == nullptr)
        return; // no ancestor split on that axis: nothing moved, so the zoom did not either

    // Emit the CLAMPED value the node actually stored (Pane::setRatio clamps), not the raw request.
    _events.paneRatioChanged(tabId, split->id(), split->ratio());
    announceZoomChange(*tab, zoomBefore);
}

void SessionModel::toggleActivePaneOrientation(TabId tabId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    auto const zoomBefore = tab->zoomedLeafId();
    auto* split = tab->toggleActivePaneOrientation();
    if (split == nullptr)
        return; // single-pane tab: no split to flip, so nothing (zoom included) changed
    _events.paneOrientationChanged(tabId, split->id(), split->splitState());
    announceZoomChange(*tab, zoomBefore);
}

std::expected<void, SessionModel::SetOrientationError> SessionModel::setSplitOrientation(SessionId anchor,
                                                                                         SplitState axis)
{
    if (axis != SplitState::Horizontal && axis != SplitState::Vertical)
        return std::unexpected(SetOrientationError::NotAnAxis);
    auto const [workspace, tab, leaf] = findSessionLeaf(anchor);
    if (tab == nullptr || leaf == nullptr)
        return std::unexpected(SetOrientationError::UnknownSession);
    auto* const split = leaf->parent();
    if (split == nullptr)
        return std::unexpected(SetOrientationError::NoSplit);
    if (split->splitState() == axis)
        return {};
    auto const zoomBefore = tab->zoomedLeafId();
    tab->clearZoom(); // the changed axis must be visible, as for the toggle
    split->setOrientation(axis);
    _events.paneOrientationChanged(tab->id(), split->id(), split->splitState());
    announceZoomChange(*tab, zoomBefore);
    return {};
}

void SessionModel::swapActivePane(TabId tabId, FocusDirection direction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;

    auto const zoomBefore = tab->zoomedLeafId();
    auto const [a, b] = tab->swapActivePane(direction);
    if (a == nullptr)
        return; // no neighbor
    _events.paneSwapped(tabId, a->id(), b->id());
    _events.activePaneChanged(tabId, tab->activePane()->id());
    announceZoomChange(*tab, zoomBefore);
}

void SessionModel::moveActivePane(TabId tabId, FocusDirection direction)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;

    auto const zoomBefore = tab->zoomedLeafId();
    if (!tab->moveActivePane(direction, _nextPaneId))
        return; // no neighbor
    // The re-parent path consumes a fresh split id; the swap-degenerate path does not, but always
    // advancing keeps ids monotonic and unique whichever branch ran.
    ++_nextPaneId;
    _events.paneTreeRestructured(tabId);
    _events.activePaneChanged(tabId, tab->activePane()->id());
    announceZoomChange(*tab, zoomBefore);
}

void SessionModel::toggleActivePaneZoom(TabId tabId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;

    auto const zoomBefore = tab->zoomedLeafId();
    (void) tab->toggleZoom();
    announceZoomChange(*tab, zoomBefore);
}

SessionId SessionModel::replaceLeafSession(TabId tabId, PaneId leafId, SessionId session)
{
    auto* leaf = findPane(tabId, leafId);
    if (leaf == nullptr || !leaf->isLeaf())
        return SessionId {};

    // The leaf object itself stays, so the tab's active-leaf pointer and its zoom (which names that
    // leaf) are untouched by construction: nothing but the session changes.
    auto const replaced = leaf->replaceSession(session);
    _events.paneSessionReplaced(tabId, leafId);
    return replaced;
}

bool SessionModel::announceZoomChange(Tab& tab, std::optional<PaneId> zoomedLeafBefore)
{
    auto const after = tab.zoomedLeafId();
    if (after == zoomedLeafBefore)
        return false;

    _events.paneZoomChanged(tab.id(), after);
    // Tab::title() reads the zoom, so a zoom change is always a retitle: the tab is named after the
    // pane on screen while zoomed, and falls back to "Multiple panes" once it is not.
    _events.tabTitleChanged(tab.id());
    return true;
}

// }}}
// {{{ Title & color

void SessionModel::setTabTitle(TabId tabId, std::string title)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    // An empty (or all-whitespace) rename means "reset to the default template", never "blank the
    // label": an empty runtime override takes precedence over the default template and would
    // leave the tab permanently unlabeled — the GUI's inline rename editor opens pre-filled with the
    // raw override (empty for a never-renamed tab) and commits verbatim on click-away. Normalizing
    // here keeps the invariant "no blank runtime titles" for every adapter.
    if (std::ranges::all_of(title, [](unsigned char c) { return std::isspace(c) != 0; }))
    {
        resetTabTitle(tabId);
        return;
    }
    tab->setRuntimeTitle(std::move(title));
    _events.tabTitleChanged(tabId);
}

void SessionModel::resetTabTitle(TabId tabId)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    tab->setRuntimeTitle(std::nullopt);
    _events.tabTitleChanged(tabId);
}

void SessionModel::setTabColor(TabId tabId, TabColorSource source, vtbackend::RGBColor color)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    tab->setColor(source, color);
    _events.tabColorChanged(tabId);
}

void SessionModel::resetTabColor(TabId tabId, TabColorSource source)
{
    auto* tab = findTab(tabId);
    if (tab == nullptr)
        return;
    tab->resetColor(source);
    _events.tabColorChanged(tabId);
}

// }}}

} // namespace vtworkspace
