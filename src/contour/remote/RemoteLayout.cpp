// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vthost/TabEdits.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Arrangement.hpp>
#include <vtworkspace/Move.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace contour::remote
{

namespace
{
    auto const layoutLog =
        core::log::Category("gui.attach.layout", "Realizing a daemon's layout in the GUI.");
} // namespace

namespace
{
    /// The leftmost leaf's session of a subtree (the pane that seeded it).
    [[nodiscard]] uint64_t leftmostSession(vthost::proto::WirePane const& pane)
    {
        auto const* node = &pane;
        while (node->split != 0 && !node->children.empty())
            node = &node->children.front();
        return node->session;
    }

    /// Appends every leaf session of @p pane to @p out, in tree order.
    void collectSessions(vthost::proto::WirePane const& pane, std::vector<uint64_t>& out)
    {
        if (!pane.isSplit())
        {
            out.push_back(pane.session);
            return;
        }
        for (auto const& child: pane.children)
            collectSessions(child, out);
    }

    /// @return Every leaf session of @p pane, in tree order.
    [[nodiscard]] std::vector<uint64_t> sessionsOf(vthost::proto::WirePane const& pane)
    {
        auto out = std::vector<uint64_t> {};
        collectSessions(pane, out);
        return out;
    }

    /// @return True if any leaf of @p pane carries a remote session already claimed
    ///         locally — bound to a pane, or tombstoned by a user close the daemon
    ///         has not acknowledged yet (a stale layout push must not resurrect it).
    [[nodiscard]] bool anyLeafClaimed(vthost::proto::WirePane const& pane, NativeController const& controller)
    {
        if (pane.split == 0)
            return controller.isClaimed(pane.session);
        return std::ranges::any_of(pane.children,
                                   [&](auto const& child) { return anyLeafClaimed(child, controller); });
    }

    /// One local split to perform to catch up with a daemon-side split: split the
    /// pane hosting @c actingSession (the split's surviving first child) to add a
    /// pane for @c newSession (the freshly created second child), reproducing the
    /// daemon split's @c ratio.
    struct SplitOp
    {
        uint64_t actingSession = 0;
        uint64_t newSession = 0;
        bool vertical = false;
        double ratio = 0.5; ///< The acting (first) child's space share on the daemon.
    };

    /// Finds ONE not-yet-realized split in @p node: a split whose first child is a
    /// BOUND leaf and whose second child's subtree is entirely UNCLAIMED (the shape
    /// a daemon `splitActivePane` leaves — old session in the first child, new in
    /// the second). Recurses through already-claimed subtrees to find nested new
    /// splits.
    [[nodiscard]] std::optional<SplitOp> findNewSplit(vthost::proto::WirePane const& node,
                                                      NativeController const& controller)
    {
        if (node.split == 0 || node.children.size() < 2)
            return std::nullopt;
        auto const& first = node.children[0];
        auto const& second = node.children[1];
        if (first.split == 0 && controller.isBound(first.session) && !anyLeafClaimed(second, controller))
            return SplitOp { .actingSession = first.session,
                             .newSession = leftmostSession(second),
                             .vertical = node.split == 2,
                             .ratio = vthost::proto::fromWireRatio(node.ratio) };
        if (anyLeafClaimed(first, controller))
            if (auto op = findNewSplit(first, controller))
                return op;
        if (anyLeafClaimed(second, controller))
            if (auto op = findNewSplit(second, controller))
                return op;
        return std::nullopt;
    }

    /// @return The first leaf of @p pane, in tree order, bound to a local pane; none for a subtree with none.
    [[nodiscard]] std::optional<uint64_t> firstBoundLeaf(vthost::proto::WirePane const& pane,
                                                         NativeController const& controller)
    {
        if (!pane.isSplit())
            return controller.isBound(pane.session) ? std::optional { pane.session } : std::nullopt;
        for (auto const& child: pane.children)
            if (auto const bound = firstBoundLeaf(child, controller))
                return bound;
        return std::nullopt;
    }

    /// Finds ONE leaf a move grafted in that findNewSplit() does not see: a split one child of which is
    /// entirely UNCLAIMED and the other shows a bound pane, in either order. A pane a daemon move put beside
    /// or onto one another client shows -- dragged from a process that alone mirrored it -- lands first as
    /// often as second, and beside a subtree as often as beside a leaf. It is realized by splitting a bound
    /// pane of its sibling; the tree is then laid out as the daemon's (arrangeMovedTabs), so only the pane
    /// count has to be right here. Recurses, as findNewSplit() does.
    [[nodiscard]] std::optional<SplitOp> findGraftedLeaf(vthost::proto::WirePane const& node,
                                                         NativeController const& controller)
    {
        if (node.split == 0 || node.children.size() < 2)
            return std::nullopt;
        auto const ratio = vthost::proto::fromWireRatio(node.ratio);
        auto const graftedBeside = [&](vthost::proto::WirePane const& shown,
                                       vthost::proto::WirePane const& grafted,
                                       double shownShare) -> std::optional<SplitOp> {
            if (anyLeafClaimed(grafted, controller))
                return std::nullopt;
            return firstBoundLeaf(shown, controller).transform([&](uint64_t acting) {
                return SplitOp { .actingSession = acting,
                                 .newSession = leftmostSession(grafted),
                                 .vertical = node.split == 2,
                                 .ratio = shownShare };
            });
        };
        auto const& first = node.children[0];
        auto const& second = node.children[1];
        if (auto op = graftedBeside(first, second, ratio))
            return op;
        if (auto op = graftedBeside(second, first, 1.0 - ratio))
            return op;
        for (auto const& child: node.children)
            if (anyLeafClaimed(child, controller))
                if (auto op = findGraftedLeaf(child, controller))
                    return op;
        return std::nullopt;
    }

    /// One local merge to perform to catch up with a daemon-side MergeTabIntoSplit: graft the whole tab
    /// @c source beside the pane @c leaf of @c target, on @c side.
    struct MergeOp
    {
        vtworkspace::Tab const* source = nullptr;
        vtworkspace::Tab const* target = nullptr;
        vtworkspace::PaneId leaf {};
        vtworkspace::SplitSide side = vtworkspace::SplitSide::Left;
    };

    /// Finds ONE not-yet-reproduced merge in @p node: a split holding a leaf beside a subtree whose sessions
    /// are, all of them and no others, one OTHER local tab's -- the shape a daemon merge leaves, since
    /// it moves a tab's sessions without spawning or closing any. Recurses, for nested merges.
    ///
    /// Without this the sessions the merge moved are already claimed here, so neither the realizer (no
    /// unclaimed leaf) nor findNewSplit (no unclaimed second child) would see anything to do, and the local
    /// tabs would stay as they were while the daemon's layout is another shape.
    [[nodiscard]] std::optional<MergeOp> findNewMerge(
        std::unordered_map<uint64_t, contour::session::TerminalSession*> const& remoteToLocalSession,
        vtworkspace::SessionModel const& model,
        vthost::proto::WirePane const& node)
    {
        if (!node.isSplit())
            return std::nullopt;
        auto const tabOf = [&](uint64_t remote) -> vtworkspace::Tab const* {
            auto const it = remoteToLocalSession.find(remote);
            return it != remoteToLocalSession.end()
                       ? std::get<1>(model.findSessionLeaf(it->second->modelSessionId()))
                       : nullptr;
        };
        auto const pairOf = [&](vthost::proto::WirePane const& leaf,
                                vthost::proto::WirePane const& grafted,
                                vtworkspace::GraftOrder order) -> std::optional<MergeOp> {
            if (leaf.isSplit())
                return std::nullopt;
            auto const* const target = tabOf(leaf.session);
            auto const sessions = sessionsOf(grafted);
            auto const* const source = tabOf(sessions.front());
            if (target == nullptr || source == nullptr || source == target
                || std::cmp_not_equal(source->paneCount(), sessions.size())
                || !std::ranges::all_of(sessions, [&](uint64_t remote) { return tabOf(remote) == source; }))
                return std::nullopt;
            // The side whose split axis is the node's and whose graft order is the subtree's.
            auto const side = vtworkspace::sideOf(static_cast<vtworkspace::SplitState>(node.split), order);
            if (!side)
                return std::nullopt;
            auto const [window, leafTab, leafPane] =
                model.findSessionLeaf(remoteToLocalSession.at(leaf.session)->modelSessionId());
            return MergeOp { .source = source, .target = target, .leaf = leafPane->id(), .side = *side };
        };
        auto const& first = node.children[0];
        auto const& second = node.children[1];
        if (auto op = pairOf(first, second, vtworkspace::GraftOrder::GraftSecond))
            return op;
        if (auto op = pairOf(second, first, vtworkspace::GraftOrder::GraftFirst))
            return op;
        if (auto op = findNewMerge(remoteToLocalSession, model, first))
            return op;
        return findNewMerge(remoteToLocalSession, model, second);
    }

    /// Maps each remote session to its local TerminalSession, by matching the pane's
    /// pty against the controller's bindings. Rebuilt after each split so a
    /// just-created pane is available to seed the next.
    [[nodiscard]] std::unordered_map<uint64_t, contour::session::TerminalSession*> remoteToLocal(
        contour::session::TerminalSessionManager& manager,
        vtworkspace::WindowId window,
        NativeController const& controller)
    {
        auto out = std::unordered_map<uint64_t, contour::session::TerminalSession*> {};
        auto* win = manager.model().window(window);
        if (win == nullptr)
            return out;
        for (auto const i: std::views::iota(0, win->tabCount()))
            for (auto* session: manager.sessionsOfTab(win->tabAt(i)))
                if (auto const remote = controller.sessionForPty(&session->terminal().device()))
                    out.emplace(*remote, session);
        return out;
    }

    /// Where every remote session bound to a pane is shown, in any window: built with one walk over the
    /// model, and told of each move the reconcile makes, so that no lookup walks it again.
    class BoundSessions
    {
      public:
        BoundSessions(contour::session::TerminalSessionManager& manager, NativeController const& controller)
        {
            manager.model().forEachTab([&](vtworkspace::Window const&, vtworkspace::Tab& tab) {
                for (auto const* session: manager.sessionsOfTab(&tab))
                    if (auto const remote = controller.sessionForPty(&session->terminal().device()))
                    {
                        auto const local = session->modelSessionId();
                        _localOf.emplace(*remote, local);
                        _remoteOf.emplace(local.value, *remote);
                        _tabOf.emplace(local.value, tab.id());
                    }
            });
        }

        /// @return The local session showing @p remote; none for a session no pane is bound to.
        [[nodiscard]] std::optional<vtworkspace::SessionId> localOf(uint64_t remote) const
        {
            auto const it = _localOf.find(remote);
            return it != _localOf.end() ? std::optional { it->second } : std::nullopt;
        }

        /// @return The remote session @p local shows; none for an unbound one.
        [[nodiscard]] std::optional<uint64_t> remoteOf(vtworkspace::SessionId local) const
        {
            auto const it = _remoteOf.find(local.value);
            return it != _remoteOf.end() ? std::optional { it->second } : std::nullopt;
        }

        /// @return The local tab showing @p remote; none for a session no pane is bound to.
        [[nodiscard]] std::optional<vtworkspace::TabId> tabOf(uint64_t remote) const
        {
            auto const local = localOf(remote);
            return local ? std::optional { _tabOf.at(local->value) } : std::nullopt;
        }

        /// Notes that a move put the pane showing @p local into @p tab.
        void moved(vtworkspace::SessionId local, vtworkspace::TabId tab)
        {
            _tabOf.insert_or_assign(local.value, tab);
        }

      private:
        std::unordered_map<uint64_t, vtworkspace::SessionId> _localOf; ///< Remote session -> local one.
        std::unordered_map<uint64_t, uint64_t> _remoteOf;              ///< Local SessionId value -> remote.
        std::unordered_map<uint64_t, vtworkspace::TabId> _tabOf;       ///< Local SessionId value -> its tab.
    };

    /// @return Whether @p wire and @p local are the SAME tree: identical shape, and every leaf pair
    ///         naming one session under @p bound. Ratios are deliberately not compared —
    ///         a differing ratio is the thing being reconciled, not evidence that the trees disagree.
    ///
    /// Shape alone is not identity: two tabs can be shaped alike, and a tab mid-reconciliation is
    /// shaped like neither. Asking before writing turns "the passes have not converged yet" into a
    /// no-op instead of a ratio written onto the wrong divider. Pure over the lookup, so it pays no locked
    /// binding scan per leaf.
    [[nodiscard]] bool sameTree(BoundSessions const& bound,
                                vthost::proto::WirePane const& wire,
                                vtworkspace::Pane const& local)
    {
        if (wire.isSplit() == local.isLeaf())
            return false;
        if (!wire.isSplit())
            return bound.localOf(wire.session) == local.session();
        return local.first() != nullptr && local.second() != nullptr
               && sameTree(bound, wire.children[0], *local.first())
               && sameTree(bound, wire.children[1], *local.second());
    }

    /// @return Whether every split of @p wire runs along the same axis as its counterpart in @p local.
    ///         @pre sameTree(), which is what makes the lockstep descent sound.
    ///
    /// sameTree() names the panes, not how they are laid out: a pane dropped on an edge of its only sibling
    /// that keeps the leaf order -- the right pane onto the left one's bottom -- changes the axis alone.
    [[nodiscard]] bool sameAxes(vthost::proto::WirePane const& wire, vtworkspace::Pane const& local)
    {
        if (local.isLeaf())
            return true;
        return wire.split == std::to_underlying(local.splitState())
               && sameAxes(wire.children[0], *local.first()) && sameAxes(wire.children[1], *local.second());
    }

    /// @return The daemon tree @p wire as an arrangement of the local sessions showing it; none while a leaf
    ///         of it is shown by no pane.
    [[nodiscard]] std::optional<vtworkspace::PaneArrangement> arrangementOf(
        vthost::proto::WirePane const& wire, BoundSessions const& bound)
    {
        if (!wire.isSplit())
        {
            auto const local = bound.localOf(wire.session);
            if (!local)
                return std::nullopt;
            return vtworkspace::PaneArrangement { .session = *local, .children = {} };
        }
        if (wire.children.size() != 2 || wire.split > std::to_underlying(vtworkspace::SplitState::Vertical))
            return std::nullopt;
        auto first = arrangementOf(wire.children[0], bound);
        auto second = arrangementOf(wire.children[1], bound);
        if (!first || !second)
            return std::nullopt;
        return vtworkspace::PaneArrangement { .split = static_cast<vtworkspace::SplitState>(wire.split),
                                              .ratio = vthost::proto::fromWireRatio(wire.ratio),
                                              .session = {},
                                              .children = { std::move(*first), std::move(*second) } };
    }

    /// Makes local @p tab the one showing @p wireTab, whose bound sessions are @p shown (tree order, not
    /// empty), and moves into it every one of them another tab shows: the same TerminalSessions, never
    /// closed in one place and mirrored anew in another. A tab still showing none of them first trades
    /// one of its panes for the first of them, so that no window is emptied of a tab it keeps. The panes
    /// arrive beside one another; the tree is laid out as the daemon's once the push is in
    /// (arrangeMovedTabs).
    void gatherInto(contour::session::TerminalSessionManager& manager,
                    BoundSessions& bound,
                    vtworkspace::Tab const& tab,
                    std::span<uint64_t const> shown)
    {
        auto const tabId = tab.id();
        auto const inTab = [&](uint64_t remote) {
            return bound.tabOf(remote) == tabId;
        };
        if (!std::ranges::any_of(shown, inTab))
        {
            auto const traded = tab.leafSessions().front();
            auto const from = *bound.tabOf(shown.front());
            if (!manager.applyNodeMove(vtworkspace::PaneSource { .session = *bound.localOf(shown.front()) },
                                       vtworkspace::SwapTarget { .pane = traded }))
                return;
            bound.moved(*bound.localOf(shown.front()), tabId);
            bound.moved(traded, from);
        }
        auto const anchor = *bound.localOf(*std::ranges::find_if(shown, inTab));
        for (auto const remote: shown)
            if (!inTab(remote))
                if (auto const moved = manager.applyNodeMove(
                        vtworkspace::PaneSource { .session = *bound.localOf(remote) },
                        vtworkspace::EdgeTarget { .pane = anchor, .side = vtworkspace::SplitSide::Right }))
                    bound.moved(*bound.localOf(remote), moved->tab);
    }

    /// Brings @p window's tabs to the daemon's @p layout, one whole push of which @p layouts is, by moving
    /// the panes it names that other tabs or windows show into the tabs paired with its tabs: what the daemon
    /// did on a MoveNode, done here to the same TerminalSessions. A daemon tab is shown by the local tab
    /// paired with it (NativeController::pairTab), moved into this window if another shows it; one paired
    /// with none, which the daemon made for a move, by a tab the first of its panes is taken out into. The
    /// paired ones go first, so a tab a new one takes a pane out of is the one paired with it. A daemon tab
    /// no pane of which is shown yet is left for the realizer.
    void gatherMovedPanes(contour::session::TerminalSessionManager& manager,
                          NativeController& controller,
                          BoundSessions& bound,
                          vtworkspace::WindowId window,
                          vthost::proto::LayoutState const& layout)
    {
        auto& model = manager.model();
        auto wireTabs = std::ranges::to<std::vector>(
            layout.tabs | std::views::transform([](auto const& tab) { return &tab; }));
        std::ranges::stable_partition(wireTabs, [&](vthost::proto::WireTab const* wireTab) {
            return controller.tabPairedWith(wireTab->tabId).has_value();
        });
        for (auto const* const wireTab: wireTabs)
        {
            auto const shown = std::ranges::to<std::vector>(
                sessionsOf(wireTab->root)
                | std::views::filter([&](uint64_t remote) { return bound.localOf(remote).has_value(); }));
            auto const* const here = model.window(window);
            if (shown.empty() || here == nullptr)
                continue;
            auto const* tab = static_cast<vtworkspace::Tab const*>(nullptr);
            if (auto const paired = controller.tabPairedWith(wireTab->tabId))
                tab = model.findTab(*paired);
            if (tab == nullptr)
            {
                // A tab the daemon made for a move: its first pane, taken out into a tab of its own.
                auto const moved = manager.applyNodeMove(
                    vtworkspace::PaneSource { .session = *bound.localOf(shown.front()) },
                    vtworkspace::TabWellTarget { .window = window, .index = here->tabCount() });
                if (!moved)
                    continue;
                bound.moved(moved->focused, moved->tab);
                tab = model.findTab(moved->tab);
            }
            else if (auto const from = model.windowOfTab(tab->id()); from != window)
                std::ignore = manager.applyNodeMove(
                    vtworkspace::TabSource { .window = from, .tab = tab->id() },
                    vtworkspace::TabWellTarget { .window = window, .index = here->tabCount() });
            controller.pairTab(tab->id(), wireTab->tabId);
            gatherInto(manager, bound, *tab, shown);
        }
    }

    /// Lays every local tab paired with a daemon tab of @p layouts, and showing exactly that tab's panes,
    /// out as that tab's tree: the moves gatherMovedPanes() made put the panes in the right tabs, beside one
    /// another, and the daemon's tree says where. Over every window, as a window's pass may have taken the
    /// last foreign pane out of a tab of another window that has had its own pass already.
    void arrangeMovedTabs(vtworkspace::SessionModel& model,
                          NativeController const& controller,
                          BoundSessions const& bound,
                          LayoutSnapshot const& layouts)
    {
        for (auto const& layout: layouts | std::views::values)
            for (auto const& wireTab: layout->tabs)
            {
                auto const paired = controller.tabPairedWith(wireTab.tabId);
                auto const* const tab = paired ? model.findTab(*paired) : nullptr;
                // A tree the same but for a ratio is applyRatios()'s: it compares as the daemon quantizes.
                // One the same but for an axis is not: only arrangeTab() turns a split.
                if (tab == nullptr
                    || (sameTree(bound, wireTab.root, *tab->rootPane())
                        && sameAxes(wireTab.root, *tab->rootPane())))
                    continue;
                if (auto const shape = arrangementOf(wireTab.root, bound))
                    std::ignore = model.arrangeTab(*paired, *shape); // refused while it shows other panes
            }
    }

    /// Puts @p window's tabs paired with the daemon tabs of @p layout in the daemon's order, ahead of any
    /// still unpaired.
    void placeTabs(vtworkspace::SessionModel& model,
                   NativeController const& controller,
                   vtworkspace::WindowId window,
                   vthost::proto::LayoutState const& layout)
    {
        auto row = 0;
        for (auto const& wireTab: layout.tabs)
        {
            auto const paired = controller.tabPairedWith(wireTab.tabId);
            auto const* const win = model.window(window);
            if (!paired || win == nullptr || win->indexOf(*paired) < 0)
                continue;
            model.moveTab(window, *paired, row); // a no-op where it is already
            ++row;
        }
    }

    /// Applies every daemon ratio that differs from the local one, so a divider another client moved
    /// arrives here too. @pre sameTree(), which is what makes the lockstep descent sound.
    ///
    /// The comparison is in WIRE units: the daemon quantizes to 1/10000 before sending, so comparing
    /// the decoded doubles would see a difference on every push that no write can remove. It is not
    /// what stops these writes echoing back to the daemon — applyRemoteLayout runs inside
    /// setRealizingLayout(), and NativeController::reportSplitRatio drops a report while that holds.
    /// This only keeps a converged push from costing a model write and a QML relayout.
    void applyRatios(contour::session::TerminalSessionManager& manager,
                     vtworkspace::TabId tab,
                     vthost::proto::WirePane const& wire,
                     vtworkspace::Pane& local)
    {
        if (local.isLeaf())
            return;
        if (vthost::proto::toWireRatio(local.ratio()) != wire.ratio)
            manager.setPaneRatio(tab, local.id(), vthost::proto::fromWireRatio(wire.ratio));
        applyRatios(manager, tab, wire.children[0], *local.first());
        applyRatios(manager, tab, wire.children[1], *local.second());
    }

    /// Brings how @p local is presented -- its active and zoomed pane, title, colour, and whether it is its
    /// window's active tab -- in line with how the daemon presents @p wire, so a change another client made
    /// arrives here too. @pre sameTree(), so every leaf of @p wire has its local session in @p bound.
    ///
    /// The edits are the ones this client would report were the daemon's presentation its own
    /// (vthost::client::tabEditsToReport, read the other way round), applied to the local model by the
    /// daemon's own table (vthost::applyTabEdit) once their sessions are translated to this model's: the two
    /// ends then cannot disagree on what an edit means. Each compares first, so a converged push writes
    /// nothing; and none is echoed back, as this runs while the controller realizes a layout, during which
    /// NativeController::reportTabPresentation drops what the manager reports.
    void applyPresentation(vtworkspace::SessionModel& model,
                           BoundSessions const& bound,
                           vthost::proto::WireTab const& wire,
                           vthost::client::TabActivity activity,
                           vtworkspace::Tab const& local)
    {
        auto const remoteOf = [&](vtworkspace::Pane const* leaf) -> uint64_t {
            return leaf != nullptr ? bound.remoteOf(leaf->session()).value_or(0) : 0;
        };
        auto const* window = model.window(model.windowOfTab(local.id()));
        auto const here = vthost::client::TabPresentation {
            .activeSession = remoteOf(local.activePane()),
            .zoomedSession = local.isZoomed() ? remoteOf(local.activePane()) : 0,
            .title = local.runtimeTitle(),
            .color = local.color(vtworkspace::TabColorSource::User).transform([](vtbackend::RGBColor color) {
                return color.value();
            }),
            .activity = window != nullptr && window->activeTab() == &local
                            ? vthost::client::TabActivity::Active
                            : vthost::client::TabActivity::Inactive,
        };
        for (auto edit:
             vthost::client::tabEditsToReport(vthost::client::presentationOf(wire, activity), here))
        {
            auto const session = bound.localOf(edit.session);
            if (!session)
                continue;
            edit.session = session->value;
            std::ignore = vthost::applyTabEdit(model, edit);
        }
    }

    /// Closes every pane of @p window whose remote session no layout of @p alive names: a pane closed on the
    /// daemon (by this or another client), or that exited. Collected first (closing mutates the model), then
    /// each target pane is activated and closed (closeActivePane removes the tab's ACTIVE pane).
    void closeEndedPanes(contour::session::TerminalSessionManager& manager,
                         NativeController const& controller,
                         vtworkspace::WindowId window,
                         LayoutSnapshot const& alive)
    {
        auto named = std::unordered_set<uint64_t> {};
        for (auto const& layout: alive | std::views::values)
            for (auto const& wireTab: layout->tabs)
                for (auto const session: sessionsOf(wireTab.root))
                    named.insert(session);
        auto toClose = std::vector<contour::session::TerminalSession*> {};
        for (auto const& [remote, local]: remoteToLocal(manager, window, controller))
            if (!named.contains(remote))
                toClose.push_back(local);
        for (auto* local: toClose)
        {
            auto const [win, tab, leaf] = manager.model().findSessionLeaf(local->modelSessionId());
            if (leaf == nullptr)
                continue;
            manager.model().setActivePane(tab->id(), leaf->id());
            // Detach, not Destroy: the daemon is where this pane went away, so authoring a close back
            // at it would at best echo a dead session id — and would destroy a live one that only left this
            // window's layout (a move whose destination window is followed by another client).
            manager.closeActivePane(local, contour::session::SessionEnd::Detach);
        }
    }

    /// Realizes ONE whole daemon tab (no leaf yet bound) via the shared realizer.
    void realizeWholeTab(contour::session::TerminalSessionManager& manager,
                         vtworkspace::WindowId window,
                         NativeController& controller,
                         vthost::proto::WireTab const& wireTab,
                         std::optional<vtbackend::PageSize> pageSize)
    {
        auto single = vthost::proto::LayoutState {};
        single.tabs.push_back(wireTab);
        auto const wl = vthost::client::wireToLayout(single);
        manager.applyLayoutToWindow(
            window, wl.layout, pageSize, [&](contour::config::LayoutPane const& leaf) {
                auto const it = wl.leafSession.find(&leaf);
                if (it != wl.leafSession.end())
                    controller.setNextBindSession(it->second);
            });
    }
} // namespace

std::vector<ReconciledWindow> windowsToReconcile(LayoutSnapshot const& layouts)
{
    return std::ranges::to<std::vector>(layouts | std::views::transform([](auto const& entry) {
                                            auto sessions = std::vector<uint64_t> {};
                                            for (auto const& tab: entry.second->tabs)
                                                collectSessions(tab.root, sessions);
                                            return ReconciledWindow {
                                                .daemonWindow = entry.first,
                                                .tabs = entry.second->tabs.empty() ? DaemonWindowTabs::None
                                                                                   : DaemonWindowTabs::Some,
                                                .sessions = std::move(sessions),
                                            };
                                        }));
}

std::vector<ReconciledWindow> windowsToReconcile(NativeController const& controller)
{
    return windowsToReconcile(controller.settledLayouts());
}

void reconcileAttachWindows(contour::session::TerminalSessionManager& manager,
                            NativeController& controller,
                            LayoutSnapshot const& layouts,
                            AttachWindowMap& windows,
                            std::function<void(ReconciledWindow const&)> const& openWindow)
{
    auto const isOpen = [&](vtworkspace::WindowId window) {
        return manager.model().window(window) != nullptr;
    };
    auto const wereOpen = std::ranges::to<std::vector>(
        windows | std::views::filter([&](auto const& entry) { return isOpen(entry.second); })
        | std::views::keys);
    {
        auto const hold = manager.holdEmptiedWindows();
        auto const reconciled = windowsToReconcile(layouts);
        for (auto const& window: reconciled)
        {
            if (auto const mapped = windows.find(window.daemonWindow); mapped != windows.end())
            {
                applyRemoteLayout(manager, mapped->second, controller, layouts, window.daemonWindow);
                manager.deliverRestoreSummaryTo(mapped->second);
                continue;
            }
            openWindow(window);
        }
        // A window a pass emptied that shows a daemon window holding tabs is the next reconcile's to fill.
        for (auto const& window: reconciled)
            if (auto const mapped = windows.find(window.daemonWindow);
                window.tabs == DaemonWindowTabs::Some && mapped != windows.end())
                hold.keep(mapped->second);
    }
    // A window this pass closed goes with its mapping, and so does one closed before -- by its user -- once
    // its daemon window is pushed empty: the daemon window filled again then gets an OS window of its own,
    // where a mapping to the closed one would show it nowhere. One whose daemon window still holds tabs, or
    // is merely absent from this snapshot (a reconnect settles windows frame by frame, a partial snapshot
    // lacks some), keeps its mapping, so that the window its user closed does not open again.
    auto const isPushedEmpty = [&](uint64_t daemonWindow) {
        auto const found = layouts.find(daemonWindow);
        return found != layouts.end() && found->second->tabs.empty();
    };
    std::erase_if(windows, [&](auto const& entry) {
        return !isOpen(entry.second)
               && (std::ranges::contains(wereOpen, entry.first) || isPushedEmpty(entry.first));
    });
}

void forgetClosedTabPairings(contour::session::TerminalSessionManager const& manager,
                             NativeController& controller)
{
    auto liveTabs = std::unordered_set<uint64_t> {};
    manager.model().forEachTab(
        [&liveTabs](auto const&, auto const& tab) { liveTabs.insert(tab.id().value); });
    controller.forgetClosedTabs(liveTabs);
}

void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WindowId window,
                       NativeController& controller,
                       std::optional<uint64_t> daemonWindow,
                       std::optional<vtbackend::PageSize> pageSize)
{
    applyRemoteLayout(manager, window, controller, controller.settledLayouts(), daemonWindow, pageSize);
}

void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WindowId window,
                       NativeController& controller,
                       LayoutSnapshot const& layouts,
                       std::optional<uint64_t> daemonWindow,
                       std::optional<vtbackend::PageSize> pageSize)
{
    // Select the daemon window to reconcile: the caller's, or the primary (lowest-id)
    // window for the single-window path.
    auto const found = daemonWindow ? layouts.find(*daemonWindow) : layouts.begin();
    if (found == layouts.end())
        return;
    auto const& layout = *found->second;
    // A window a move emptied is closed here while its daemon window may still be pushed (empty): there is
    // nothing to reconcile, and realizing into it would pull back what the move took out.
    if (manager.model().window(window) == nullptr)
        return;
    // A window a move below empties stays open until the pass is done -- until every window's is, under
    // reconcileAttachWindows(): a later step, or another window's pass, may fill it again.
    auto const emptied = manager.holdEmptiedWindows();
    // A daemon that moves panes ends each push, so @p layouts is all of one: a pane missing from this window
    // went to another, or ended -- and which of the two is in @p layouts too.
    auto const converges = controller.moveSupport() == contour::session::MoveSupport::Supported;

    // Incremental reconciliation: bring the local tree in line with the daemon's
    // on every layout push, so a tab/split authored on the daemon — here or by
    // another client — appears without rebuilding what is already shown.
    controller.setRealizingLayout(true);

    // Merges first: a tab the daemon grafted into a split of another keeps its sessions, all of them
    // already claimed here, so the passes below would find nothing to realize and leave the local tabs as
    // they were. The manager makes the merge locally because the controller, realizing, does not route it.
    for (auto const& wireTab: layout.tabs)
        while (auto const op =
                   findNewMerge(remoteToLocal(manager, window, controller), manager.model(), wireTab.root))
        {
            auto const* const win = manager.model().window(window);
            if (win == nullptr)
                break;
            auto const tabs = std::views::iota(0, win->tabCount());
            auto const row = std::ranges::find_if(tabs, [&](int i) { return win->tabAt(i) == op->source; });
            if (row == tabs.end())
                break;
            auto const tabCount = win->tabCount();
            manager.mergeTabIntoSplit(window, *row, window, op->target->id(), op->leaf, op->side);
            if (win->tabCount() >= tabCount)
                break; // refused: the next push tries again
        }

    // Moves next: a pane or tab the daemon moved (MoveNode) keeps its sessions, so it is moved here too.
    if (converges)
    {
        auto bound = BoundSessions { manager, controller };
        gatherMovedPanes(manager, controller, bound, window, layout);
    }

    for (auto const& wireTab: layout.tabs)
    {
        if (!anyLeafClaimed(wireTab.root, controller))
        {
            realizeWholeTab(manager, window, controller, wireTab, pageSize);
            continue;
        }
        // The tab is already shown: catch up any new splits inside it, one at a
        // time (each split binds a new pane, then the map is rebuilt so a following
        // split can target it). From a daemon that moves panes, then any pane a move grafted into it that no
        // pane here showed: arrangeMovedTabs() below lays the tree out as the daemon's.
        auto const nextSplit = [&] {
            auto op = findNewSplit(wireTab.root, controller);
            if (!op && converges)
                op = findGraftedLeaf(wireTab.root, controller);
            return op;
        };
        while (auto const op = nextSplit())
        {
            auto const map = remoteToLocal(manager, window, controller);
            auto const acting = map.find(op->actingSession);
            if (acting == map.end())
                break; // the acting pane is not (yet) local — try again next push
            controller.setNextBindSession(op->newSession);
            manager.splitActivePane(op->vertical, acting->second, op->ratio);
            // A split the manager refused (no session can be backed now) binds nothing, and the same split
            // would be found again for ever: the next push tries again instead.
            if (!controller.isBound(op->newSession))
            {
                layoutLog()(
                    "Could not realize the daemon's split for session {}; leaving it to the next push.",
                    op->newSession);
                controller.cancelReconnectBinding();
                break;
            }
        }
    }

    // Subtractive: a pane whose remote session vanished is closed. From a daemon that moves panes, only one
    // no window's layout names: one another window's names moved there, and that window's pass takes it.
    closeEndedPanes(manager, controller, window, converges ? layouts : LayoutSnapshot { *found });

    // The tree last: the passes above add, move and remove panes. A daemon tab whose panes the local tab
    // paired with it holds is laid out as the daemon's, and this window's tabs are put in its order.
    auto const bound = BoundSessions { manager, controller };
    if (converges)
        arrangeMovedTabs(manager.model(), controller, bound, layouts);

    // Each wire tab is paired with the local tab that sameTree() accepts — the identity check IS the
    // pairing for a tab the realizer made.
    auto* win = manager.model().window(window);
    auto shownAs = std::vector<std::pair<std::size_t, vtworkspace::Tab*>> {};
    for (auto const [index, wireTab]: core::views::enumerate(layout.tabs))
        for (auto const i: std::views::iota(0, win != nullptr ? win->tabCount() : 0))
            if (auto* tab = win->tabAt(i); sameTree(bound, wireTab.root, *tab->rootPane()))
            {
                controller.pairTab(tab->id(), wireTab.tabId);
                shownAs.emplace_back(index, tab);
                break;
            }
    if (converges)
        placeTabs(manager.model(), controller, window, layout);

    // Ratios on the settled tree: closing a pane makes its parent absorb the survivor's ratio, so a divider
    // is only meaningful once the shape is right. Then, last of all, how each tab is presented: an active or
    // zoomed pane is named by a leaf, so it too waits for the tree. applyRatios only writes a double, and
    // applyPresentation changes no tree, so neither the window nor the tabs move under the loop.
    for (auto const [index, tab]: shownAs)
    {
        auto const& wireTab = layout.tabs[index];
        applyRatios(manager, tab->id(), wireTab.root, *tab->rootPane());
        applyPresentation(manager.model(),
                          bound,
                          wireTab,
                          std::cmp_equal(index, layout.activeTab) ? vthost::client::TabActivity::Active
                                                                  : vthost::client::TabActivity::Inactive,
                          *tab);
    }

    controller.setRealizingLayout(false);
}

} // namespace contour::remote
