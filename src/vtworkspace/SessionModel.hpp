// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <vtworkspace/Arrangement.hpp>
#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/Move.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/Tab.hpp>
#include <vtworkspace/WorkspaceState.hpp>

namespace vtworkspace
{

/// A logical workspace: an ordered list of tabs with one active tab.
///
/// In single-process GUI mode there is one Workspace per OS window; in daemon mode a Workspace is a
/// server-side layout that one or more clients may attach to.
class Workspace
{
  public:
    explicit Workspace(WorkspaceId id) noexcept: _id { id } {}

    Workspace(Workspace const&) = delete;
    Workspace& operator=(Workspace const&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;
    ~Workspace() = default;

    [[nodiscard]] WorkspaceId id() const noexcept { return _id; }
    [[nodiscard]] int tabCount() const noexcept { return static_cast<int>(_tabs.size()); }
    [[nodiscard]] bool empty() const noexcept { return _tabs.empty(); }

    /// The active tab, or nullptr if the workspace has no tabs.
    [[nodiscard]] Tab* activeTab() const noexcept;
    [[nodiscard]] int activeTabIndex() const noexcept { return _activeTabIndex; }

    /// The tab that was active before the current one (the target of a "switch to previous tab"
    /// toggle), or nullptr if there is no valid previous tab (never switched, or the previous tab was
    /// closed). Tracked here — the authoritative, Qt-free model — so switch-to-previous-tab is a pure
    /// model query and lives per workspace automatically.
    [[nodiscard]] Tab* previousActiveTab() const noexcept;
    [[nodiscard]] int previousActiveTabIndex() const noexcept { return _previousActiveTabIndex; }

    /// The tab at @p index, or nullptr if out of range.
    [[nodiscard]] Tab* tabAt(int index) const noexcept;

    /// The index of @p tab, or -1 if not found.
    [[nodiscard]] int indexOf(TabId tab) const noexcept;
    [[nodiscard]] int indexOf(Tab const* tab) const noexcept;

  private:
    friend class SessionModel;

    /// Inserts @p tab at @p index, shifting the active and previous slots so they keep naming the same
    /// tabs. Activates nothing and announces nothing; the caller does both.
    /// @param index Where the tab goes, already clamped to `0..tabCount()`.
    /// @param tab The tab to insert.
    void insertTabAt(int index, std::unique_ptr<Tab> tab);

    WorkspaceId _id;
    std::vector<std::unique_ptr<Tab>> _tabs;
    int _activeTabIndex = -1;
    /// The slot that was active before _activeTabIndex, or -1 if none. Maintained by SessionModel: set on
    /// a genuine activation (createTab/activateTab), and translated/invalidated (never left dangling) by
    /// the reindexing operations (moveTab/closeTabAt).
    int _previousActiveTabIndex = -1;
};

/// The session id a host restoring @p tree mints first, so that no new session is named as a restored one.
/// @param tree A captured workspace tree (SessionModel::captureTree()).
/// @return One above the highest session a leaf of @p tree carries; 1 for a tree without leaves, as
///         SessionId{} is the sentinel for none.
[[nodiscard]] SessionId firstUnusedSessionId(WorkspaceTreeState const& tree);

/// The single source of truth for all workspaces, tabs and panes.
///
/// SessionModel is deliberately free of Qt and of any transport. Every mutation is performed through
/// one of its methods, which updates the tree and notifies the host through ModelEvents. The Qt GUI
/// supplies a ModelEvents implementation that turns each callback into a Qt signal; a future daemon
/// supplies one that fans each callback out to subscribed network clients. The model is the same in
/// both worlds, which is what lets a daemon own it and many clients observe it.
///
/// Session lifetime is *not* owned here: the model refers to a session only by SessionId. Creating
/// the underlying terminal/PTY for a new SessionId, and tearing it down for a closed one, is the
/// host's responsibility (it learns of both through the SessionAllocator and ModelEvents).
class SessionModel
{
  public:
    /// Allocates a fresh session for a new pane and returns its id. The host creates the backing
    /// terminal/PTY for that id. Invoked when a tab or split needs a new session.
    using SessionAllocator = std::function<SessionId()>;

    /// @param events    Observer notified of every change. Must outlive the model.
    /// @param allocator Supplies a new SessionId (and its backing terminal) on demand.
    SessionModel(ModelEvents& events, SessionAllocator allocator);

    SessionModel(SessionModel const&) = delete;
    SessionModel& operator=(SessionModel const&) = delete;
    SessionModel(SessionModel&&) = delete;
    SessionModel& operator=(SessionModel&&) = delete;
    ~SessionModel() = default;

    // {{{ Snapshot & restore (daemon live upgrade)

    /// @return Every workspace, tab and pane, with the ids and session ids they have now.
    [[nodiscard]] WorkspaceTreeState captureTree() const;

    /// @return The counters the next workspace, tab and pane are minted from.
    [[nodiscard]] ModelIdState idState() const noexcept;

    /// Rebuilds the tree of a model that a previous process held, with the very ids it had, so that
    /// reconnecting clients map onto the same objects. Fires no event: nothing observes a model being
    /// rebuilt, and the host that restores it announces what it must itself.
    ///
    /// A model that holds a workspace already is a contract violation (Require), not an error: restoring is
    /// done once, into a fresh model. Session ids are taken as given; the SessionAllocator is not called,
    /// so the host must seed its own counter above every restored session id (firstUnusedSessionId()). A
    /// leaf carrying SessionId{} is refused.
    /// @param tree The workspaces, tabs and panes to rebuild.
    /// @param ids  The counters to mint from afterwards; every id in @p tree must be below its counter.
    /// @return Nothing; or why @p tree cannot be a model. On error the model is still empty.
    [[nodiscard]] std::expected<void, ModelRestoreError> restore(WorkspaceTreeState const& tree,
                                                                 ModelIdState const& ids);

    // }}}
    // {{{ Workspaces

    /// Creates a new, empty workspace and returns it.
    [[nodiscard]] Workspace* createWorkspace();

    /// Moves @p workspace to position @p toIndex in the workspace order. @p toIndex is clamped to the order's
    /// bounds; an unknown workspace, or a move to where it already is, changes nothing and announces nothing.
    /// @param workspace The workspace to move.
    /// @param toIndex   Its new 0-based position.
    void moveWorkspace(WorkspaceId workspace, int toIndex);

    /// Removes a workspace and everything in it. Each contained session is reported closed.
    void removeWorkspace(WorkspaceId workspace);

    [[nodiscard]] Workspace* workspace(WorkspaceId id) const noexcept;

    /// @return How many workspaces the model holds.
    [[nodiscard]] int workspaceCount() const noexcept { return static_cast<int>(_workspaces.size()); }

    /// @return The workspace at @p index (0-based), or nullptr if out of range. For
    ///         enumerating every workspace (e.g. the daemon serializing all of them).
    [[nodiscard]] Workspace* workspaceAt(int index) const noexcept
    {
        return index >= 0 && std::cmp_less(index, _workspaces.size())
                   ? _workspaces[static_cast<std::size_t>(index)].get()
                   : nullptr;
    }

    // }}}
    // {{{ Tabs

    /// Appends a new tab (with a single fresh-session pane) to @p workspace and makes it active.
    [[nodiscard]] Tab* createTab(WorkspaceId workspace);

    /// Closes the tab @p tab in @p workspace, reporting each of its sessions closed. If it was the
    /// last tab, the workspace becomes empty (the host decides whether to close the OS window).
    void closeTab(WorkspaceId workspace, TabId tab);

    /// Closes every tab in @p workspace except @p keep.
    void closeOtherTabs(WorkspaceId workspace, TabId keep);

    /// Closes every tab positioned after @p anchor in @p workspace.
    void closeTabsToRight(WorkspaceId workspace, TabId anchor);

    /// Makes @p tab the active tab of @p workspace.
    void activateTab(WorkspaceId workspace, TabId tab);

    /// Moves @p tab to @p toIndex within @p workspace.
    void moveTab(WorkspaceId workspace, TabId tab, int toIndex);

    /// Moves @p tab out of workspace @p from and into workspace @p to at @p toIndex, transplanting its whole
    /// pane subtree (and the sessions it carries — none are closed). Fixes the active/previous-tab
    /// bookkeeping on both workspaces. No-op if either workspace is unknown or @p tab is not in @p from.
    ///
    /// This is the model half of dragging a tab between workspaces (or tearing it into a fresh workspace):
    /// the Tab object moves intact, so its sessions survive and the host only re-binds them to the
    /// destination workspace's displays.
    /// @param from    The workspace currently holding @p tab.
    /// @param tab     The tab to move.
    /// @param to      The destination workspace (may equal @p from, in which case this reorders).
    /// @param toIndex The destination position, clamped into the destination's tab range.
    void moveTabToWorkspace(WorkspaceId from, TabId tab, WorkspaceId to, int toIndex);

    /// Grafts the whole of tab @p source (in workspace @p from) beside the leaf @p leaf of tab @p target (in
    /// workspace @p to), on @p side, at ratio 0.5. The source's panes and sessions travel with it (none is
    /// closed); the source tab is removed; the grafted tree's active pane becomes @p target's active
    /// pane, and @p target becomes the active tab of @p to. Zoom state is dropped: a zoomed source shows
    /// all its panes, and so does the target.
    ///
    /// Hosts see the source tab leave (tabAboutToBeRemoved, tabClosed), then the target tab activate, and
    /// the target's tree restructure (paneTreeRestructured, activePaneChanged).
    /// @param from   The workspace holding @p source (may equal @p to).
    /// @param source The tab to merge.
    /// @param to     The workspace holding @p target.
    /// @param target The tab receiving it.
    /// @param leaf   A leaf of @p target to split.
    /// @param side   The side of @p leaf the merged tab goes on.
    /// @return The root of the grafted tree; nullptr, and nothing changed, when @p source equals
    ///         @p target, an id is unknown, or @p leaf is not a leaf of @p target.
    [[nodiscard]] Pane* mergeTabIntoSplit(
        WorkspaceId from, TabId source, WorkspaceId to, TabId target, PaneId leaf, SplitSide side);

    // }}}
    // {{{ Moves

    /// Moves a pane or a whole tab to a new place: beside another pane, onto another pane (trading
    /// places), into another tab as a split (beside its active pane), into a workspace's tab strip, or into a
    /// workspace of its own. No session is closed; the moved
    /// node keeps its sessions, and the pane keeps its PaneId except when it swaps (only the sessions
    /// trade places then) or becomes a tab of its own.
    ///
    /// Everything is validated before the first mutation, so a refusal changes nothing and fires no
    /// event. On success the target tab becomes its workspace's active tab, the moved session becomes its
    /// active pane, and zoom is cleared on every tab involved. A source tab left without panes is
    /// removed; a source workspace left without tabs stays in the model, empty, for the host to close (as
    /// with moveTabToWorkspace).
    /// @param source What is dragged. A one-pane tab counts as a pane.
    /// @param target Where it is dropped.
    /// @return Where focus lands, or why nothing happened.
    [[nodiscard]] std::expected<MoveResult, MoveError> moveNode(MoveSource const& source,
                                                                MoveTarget const& target);

    /// @param source What a move would drag.
    /// @return Whether @p source is everything its workspace holds: its only tab, or that tab's only pane. A
    ///         NewWorkspaceTarget refuses such a source as MoveError::NoOp; a host that makes the new
    ///         workspace itself asks this first.
    [[nodiscard]] bool fillsItsWorkspace(MoveSource const& source) const;

    /// Lays @p tab's panes out as @p shape: the leaves it has are rearranged into the given splits, ratios
    /// and order, each going where its session is named. No session is spawned or closed, and every leaf
    /// keeps its PaneId, so a host showing a pane keeps showing it. What a host mirroring another model
    /// does once a tab holds the panes the other model's tab does, but in another tree.
    ///
    /// Hosts see the tree restructure (paneTreeRestructured) and, if a zoom was cleared, the zoom change
    /// and the retitle. The active pane stays the active pane, so focus does not move. A tab already
    /// arranged as @p shape is left as it is, and nothing fires.
    /// @param tab   The tab to rearrange.
    /// @param shape The tree to lay its panes out as, naming each session of the tab exactly once.
    /// @return Nothing, or why nothing changed.
    [[nodiscard]] std::expected<void, ArrangeError> arrangeTab(TabId tab, PaneArrangement const& shape);

    // }}}
    // {{{ Panes

    /// Splits @p tab's active pane along @p direction, allocating a new session for the new pane.
    /// Returns the new (now active) leaf.
    Pane* splitActivePane(TabId tab, SplitState direction, double ratio = 0.5);

    /// Closes pane @p leaf in @p tab. If it was the tab's last pane, the whole tab is closed
    /// instead; a workspace left with no tabs stays in the model (empty), for the host to close or
    /// keep — see closeTab. Reports the closed session(s).
    void closePane(WorkspaceId workspace, TabId tab, PaneId leaf);

    /// Sets the active pane of @p tab.
    void setActivePane(TabId tab, PaneId leaf);

    /// Moves focus within @p tab in @p direction.
    void focusDirection(TabId tab, FocusDirection direction);

    /// Updates the split ratio of the internal node @p splitNode in @p tab.
    void setPaneRatio(TabId tab, PaneId splitNode, double ratio);

    /// Grows or shrinks @p tab's active pane along @p direction by @p fraction, by nudging the ratio
    /// of the nearest ancestor split on the matching axis. No-op if the active pane has no such
    /// ancestor (a single pane, or only cross-axis splits above it).
    /// @param tab       The tab whose active pane resizes.
    /// @param direction The side the active pane grows toward.
    /// @param fraction  The ratio delta magnitude in (0, 1); the sign is derived from @p direction.
    void resizeActivePane(TabId tab, FocusDirection direction, double fraction);

    /// Flips the orientation of @p tab's active pane's parent split (Horizontal<->Vertical). No-op if
    /// the active pane is the tab's only pane.
    void toggleActivePaneOrientation(TabId tab);

    /// Why setSplitOrientation() changed nothing.
    enum class SetOrientationError : uint8_t
    {
        UnknownSession, ///< No pane hosts the anchor session.
        NoSplit,        ///< The anchor's tab is a single pane: there is no parent split.
        NotAnAxis,      ///< The requested state is neither Horizontal nor Vertical.
    };

    /// Sets the axis of the split that is the parent of the leaf hosting @p anchor, whatever pane is
    /// active, and clears the tab's zoom as the toggle does. A split already on @p axis is left alone and
    /// fires no event. Fires paneOrientationChanged otherwise.
    /// @param anchor The session of a leaf inside the split to change.
    /// @param axis   The axis to set: Horizontal or Vertical.
    /// @return Nothing, or why nothing changed.
    [[nodiscard]] std::expected<void, SetOrientationError> setSplitOrientation(SessionId anchor,
                                                                               SplitState axis);

    /// Swaps @p tab's active pane with its neighbor in @p direction (the two terminals trade slots),
    /// keeping the moved session focused. No-op if there is no neighbor.
    void swapActivePane(TabId tab, FocusDirection direction);

    /// Moves @p tab's active pane across its neighbor in @p direction, re-parenting it in the tree
    /// (see Tab::moveActivePane). No-op if there is no neighbor.
    void moveActivePane(TabId tab, FocusDirection direction);

    /// Toggles zoom on @p tab's active pane: zoomed, that pane alone fills the tab area (see Tab's
    /// zoom block). No-op on a single-pane tab.
    void toggleActivePaneZoom(TabId tab);

    /// Puts @p session into @p tab's leaf @p leaf in place of the one it carries (a restart in the same
    /// pane). The leaf keeps its PaneId, so the active pane and a zoom on it are unchanged; raises
    /// ModelEvents::paneSessionReplaced once.
    /// @param tab     The tab holding the leaf.
    /// @param leaf    The leaf whose session is replaced.
    /// @param session The session the leaf carries from now on.
    /// @return The session the leaf carried before, or an empty id (and no event) when @p tab has no
    ///         leaf @p leaf.
    SessionId replaceLeafSession(TabId tab, PaneId leaf, SessionId session);

    // }}}
    // {{{ Title & color (authoritative state lives here, below the GUI)

    void setTabTitle(TabId tab, std::string title);
    void resetTabTitle(TabId tab);

    /// Assigns @p tab's color on behalf of @p source. Sources are independent and ranked, so a user's
    /// choice and an application's DECAC color coexist; see TabColorSource.
    /// @param tab The tab to color.
    /// @param source The assigning source.
    /// @param color The color to assign.
    void setTabColor(TabId tab, TabColorSource source, vtbackend::RGBColor color);

    /// Clears @p source's color for @p tab, falling back to the next-highest source that has one.
    /// @param tab The tab to un-color.
    /// @param source The source whose color to clear.
    void resetTabColor(TabId tab, TabColorSource source);

    /// Installs the session-title resolver — a cross-layer bridge the host supplies so tabs can
    /// derive their title from the active pane's program title.
    ///
    /// This is a post-construction setter (a documented rebinding seam) because the resolver reaches
    /// into the host's terminal-session registry, which is built from SessionIds the model itself
    /// allocates. The resolver cannot exist before the model, and the model cannot resolve titles
    /// without it — the two are wired once both are live.
    void setSessionTitleResolver(Tab::SessionTitleResolver resolver) { _titleResolver = std::move(resolver); }

    /// Returns the currently installed session-title resolver.
    /// @see setSessionTitleResolver
    [[nodiscard]] Tab::SessionTitleResolver const& sessionTitleResolver() const noexcept
    {
        return _titleResolver;
    }

    // }}}
    // {{{ Lookups & palette

    [[nodiscard]] Tab* findTab(TabId tab) const noexcept;

    /// The id of the workspace owning @p tab, or a default WorkspaceId{} (value 0) if @p tab is unknown. Lets
    /// the Qt host route a pane event (which carries only a TabId) to the owning workspace's controller.
    [[nodiscard]] WorkspaceId workspaceOfTab(TabId tab) const noexcept;

    /// Locates the leaf pane hosting @p session, searching every workspace. For hosts that resolve a
    /// session to its pane without knowing which workspace owns it (the daemon's session-exit pruning,
    /// per-session output mapping, and client-authored split verbs).
    /// @param session The session to locate.
    /// @return The {workspace, tab, pane} triple; all nullptr when @p session is unknown.
    [[nodiscard]] std::tuple<Workspace*, Tab*, Pane*> findSessionLeaf(SessionId session) const;

    /// Locates the LEAF pane with id @p pane, searching every workspace (pane ids are minted
    /// model-globally). For hosts resolving a wire pane reference without knowing which workspace owns
    /// it (the tmux control session's %N targets — tmux names leaf panes only, so internal split
    /// nodes deliberately do not match).
    /// @param pane The leaf pane id to locate.
    /// @return The leaf pane, or nullptr when @p pane is unknown.
    [[nodiscard]] Pane* findLeafPane(PaneId pane) const;

    /// Locates any pane — leaf or split node — of @p tab.
    ///
    /// The pane mutators all begin by resolving a (tab, pane) pair, and so does a host answering a
    /// pane event, which carries exactly that pair and nothing else. Whether the node must be a leaf
    /// differs per caller, so that stays the caller's guard; only the lookup is shared.
    /// @param tab  The tab owning the pane.
    /// @param pane The pane id to locate.
    /// @return The pane, or nullptr when either id is unknown.
    [[nodiscard]] Pane* findPane(TabId tab, PaneId pane) const;

    /// Invokes @p f(Workspace&, Tab&) for every tab of every workspace, in workspace order. Single-sources
    /// the workspaces×tabs traversal for hosts that project the whole model (the daemon's resize
    /// reprojection and attach snapshots).
    /// @param f The per-tab visitor.
    template <typename F>
    void forEachTab(F&& f) const
    {
        // @p f is invoked once per tab, so it is forwarded once into a reference binding and
        // called through that (the same idiom Pane::walkTree documents).
        auto&& visit = std::forward<F>(f);
        for (auto const& win: _workspaces)
            for (auto const& tab: win->_tabs)
                visit(*win, *tab);
    }

    /// The predefined tab-color palette offered to the user (a grid of swatches, WT-style). Both the
    /// GUI and a future daemon expose the same set so all clients see identical choices. Backed by a
    /// single constexpr table with static storage duration, so this hands back a view over it — no
    /// per-instance copy or allocation.
    [[nodiscard]] std::span<vtbackend::RGBColor const> colorPalette() const noexcept;

    // }}}

  private:
    /// Locates the workspace owning @p tab. Returns {workspace, index-of-tab} or {nullptr, -1}.
    [[nodiscard]] std::pair<Workspace*, int> locateTab(TabId tab) const noexcept;

    /// Removes the tab at @p index of @p workspace, announcing it. @p beforeErase, when given, runs after the
    /// "about to be removed" announcement and before the tab is destroyed, for a caller that must take
    /// something out of the tab first.
    void closeTabAt(Workspace& workspace, int index, std::function<void()> const& beforeErase = {});

    /// Announces everything that follows from @p tab's zoom having changed during a mutation, given
    /// the zoomed leaf captured @p zoomedLeafBefore it ran. Every pane mutator ends with this call.
    ///
    /// This is the ONE place the zoom contract turns into events, so no mutator has to re-derive it.
    /// Comparing Tab::zoomedLeafId() rather than the raw flag is what collapses three cases into one
    /// comparison: it changes when zoom is entered, when it is cleared, and when it follows focus to
    /// another leaf — and it is stable, so this emits nothing, whenever an unzoomed tab is mutated.
    ///
    /// The retitle rides along here because Tab::title() reads the zoom: a tab is named after the
    /// pane on screen while zoomed and "Multiple panes" once it is not. Deriving that per mutator is
    /// how the three restructuring paths (orientation flip, swap, move) came to silently omit it.
    ///
    /// @pre @p tab must still exist — never call this where the mutation could have closed the tab
    ///      (closePane's last-pane branch), for which tabClosed already tells the host everything.
    /// @param tab               The tab that was mutated.
    /// @param zoomedLeafBefore  Tab::zoomedLeafId() sampled before the mutation.
    /// @return true if a zoom change — and with it a retitle — was announced. A caller that would emit
    ///         its own tabTitleChanged for an unrelated reason checks this so the tab is not retitled
    ///         twice, which would run the host's whole status-line fan-out for one keystroke.
    bool announceZoomChange(Tab& tab, std::optional<PaneId> zoomedLeafBefore);

    /// Inserts @p tab into @p workspace at @p index (clamped), announcing it and making it the active tab.
    Tab* insertTab(Workspace& workspace, int index, std::unique_ptr<Tab> tab);

    /// Announces the zoom change, or else the retitle, of @p tab after a move touched its panes.
    void announcePaneMove(Tab& tab, std::optional<PaneId> zoomedLeafBefore);

    /// Announces that a move took a pane out of @p tab, which keeps others: its tree restructured, its
    /// active pane, and its zoom change or retitle.
    void announceSourceShrunk(Tab& tab, std::optional<PaneId> zoomedLeafBefore);

    // {{{ moveNode() halves, defined with it.
    enum class SourceShape : uint8_t;
    struct MoveSourceNode;
    struct MoveTargetNode;

    /// @return What @p source names, or why it names nothing.
    [[nodiscard]] std::expected<MoveSourceNode, MoveError> resolveMoveSource(MoveSource const& source) const;

    /// @return What @p target names, or why it names nothing.
    [[nodiscard]] std::expected<MoveTargetNode, MoveError> resolveMoveTarget(MoveTarget const& target) const;

    /// @return Whether @p src is everything its workspace holds (@see fillsItsWorkspace(MoveSource const&)).
    [[nodiscard]] static bool fillsItsWorkspace(MoveSourceNode const& src) noexcept;

    /// Moves @p src beside @p dst's pane, on @p side. @pre moveNode() validated the move.
    /// @return Where focus lands; an error only were the validation wrong, never by undefined behaviour.
    std::expected<MoveResult, MoveError> moveBeside(MoveSourceNode const& src,
                                                    MoveTargetNode const& dst,
                                                    SplitSide side);

    /// Trades @p src's pane with @p dst's. @pre moveNode() validated the move.
    MoveResult swapWith(MoveSourceNode const& src, MoveTargetNode const& dst);

    /// Moves @p src into @p workspace's tab strip at @p index. @pre moveNode() validated the move.
    MoveResult moveIntoWell(MoveSourceNode const& src, Workspace& workspace, int index);
    // }}}

    ModelEvents& _events;
    SessionAllocator _allocateSession;
    Tab::SessionTitleResolver _titleResolver;

    std::vector<std::unique_ptr<Workspace>> _workspaces;

    WorkspaceId _nextWorkspaceId { 1 };
    TabId _nextTabId { 1 };
    PaneId _nextPaneId { 1 };
};

} // namespace vtworkspace
