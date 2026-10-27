// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vthost/TabEdits.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace contour::remote
{

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

    /// Collects every leaf session of @p pane into @p out.
    void collectSessions(vthost::proto::WirePane const& pane, std::unordered_set<uint64_t>& out)
    {
        if (pane.split == 0)
        {
            out.insert(pane.session);
            return;
        }
        for (auto const& child: pane.children)
            collectSessions(child, out);
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

    /// Every side a grafted tab can go on.
    constexpr auto AllSplitSides = std::array { vtworkspace::SplitSide::Left,
                                                vtworkspace::SplitSide::Right,
                                                vtworkspace::SplitSide::Top,
                                                vtworkspace::SplitSide::Bottom };

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
            auto sessions = std::unordered_set<uint64_t> {};
            collectSessions(grafted, sessions);
            auto const* const source = tabOf(*sessions.begin());
            if (target == nullptr || source == nullptr || source == target
                || std::cmp_not_equal(source->paneCount(), sessions.size())
                || !std::ranges::all_of(sessions, [&](uint64_t remote) { return tabOf(remote) == source; }))
                return std::nullopt;
            // The side whose split axis is the node's and whose graft order is the subtree's.
            auto const side = std::ranges::find_if(AllSplitSides, [&](vtworkspace::SplitSide candidate) {
                return std::to_underlying(vtworkspace::splitStateOf(candidate)) == node.split
                       && vtworkspace::graftOrderOf(candidate) == order;
            });
            if (side == AllSplitSides.end())
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

    /// @return Whether @p wire and @p local are the SAME tree: identical shape, and every leaf pair
    ///         naming one session under @p remoteToLocalSession. Ratios are deliberately not compared —
    ///         a differing ratio is the thing being reconciled, not evidence that the trees disagree.
    ///
    /// Shape alone is not identity: two tabs can be shaped alike, and a tab mid-reconciliation is
    /// shaped like neither. Asking before writing turns "the passes have not converged yet" into a
    /// no-op instead of a ratio written onto the wrong divider. Pure over the map, so it needs neither
    /// the manager nor the controller — and pays no locked binding scan per leaf.
    [[nodiscard]] bool sameTree(
        std::unordered_map<uint64_t, contour::session::TerminalSession*> const& remoteToLocalSession,
        vthost::proto::WirePane const& wire,
        vtworkspace::Pane const& local)
    {
        if (wire.isSplit() == local.isLeaf())
            return false;
        if (!wire.isSplit())
        {
            auto const it = remoteToLocalSession.find(wire.session);
            return it != remoteToLocalSession.end() && it->second->modelSessionId() == local.session();
        }
        return local.first() != nullptr && local.second() != nullptr
               && sameTree(remoteToLocalSession, wire.children[0], *local.first())
               && sameTree(remoteToLocalSession, wire.children[1], *local.second());
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
    /// arrives here too. @pre sameTree(), so every leaf of @p wire has its local session in the map.
    ///
    /// The edits are the ones this client would report were the daemon's presentation its own
    /// (vthost::client::tabEditsToReport, read the other way round), applied to the local model by the
    /// daemon's own table (vthost::applyTabEdit) once their sessions are translated to this model's: the two
    /// ends then cannot disagree on what an edit means. Each compares first, so a converged push writes
    /// nothing; and none is echoed back, as this runs while the controller realizes a layout, during which
    /// NativeController::reportTabPresentation drops what the manager reports.
    void applyPresentation(
        vtworkspace::SessionModel& model,
        std::unordered_map<uint64_t, contour::session::TerminalSession*> const& remoteToLocalSession,
        vthost::proto::WireTab const& wire,
        vthost::client::TabActivity activity,
        vtworkspace::Tab const& local)
    {
        auto const remoteOf = [&](vtworkspace::Pane const* leaf) -> uint64_t {
            if (leaf != nullptr)
                for (auto const& [remote, session]: remoteToLocalSession)
                    if (session->modelSessionId() == leaf->session())
                        return remote;
            return 0;
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
            auto const session = remoteToLocalSession.find(edit.session);
            if (session == remoteToLocalSession.end())
                continue;
            edit.session = session->second->modelSessionId().value;
            std::ignore = vthost::applyTabEdit(model, edit);
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

void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WindowId window,
                       NativeController& controller,
                       std::optional<uint64_t> daemonWindow,
                       std::optional<vtbackend::PageSize> pageSize)
{
    // Select the daemon window to reconcile: the caller's, or the primary (lowest-id)
    // window for the single-window path.
    auto const layout = daemonWindow ? controller.layout(*daemonWindow) : controller.layout();
    if (!layout)
        return;

    // Incremental reconciliation: bring the local tree in line with the daemon's
    // on every layout push, so a tab/split authored on the daemon — here or by
    // another client — appears without rebuilding what is already shown.
    controller.setRealizingLayout(true);

    // Merges first: a tab the daemon grafted into a split of another keeps its sessions, all of them
    // already claimed here, so the passes below would find nothing to realize and leave the local tabs as
    // they were. The manager makes the merge locally because the controller, realizing, does not route it.
    for (auto const& wireTab: layout->tabs)
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

    for (auto const& wireTab: layout->tabs)
    {
        if (!anyLeafClaimed(wireTab.root, controller))
        {
            realizeWholeTab(manager, window, controller, wireTab, pageSize);
            continue;
        }
        // The tab is already shown: catch up any new splits inside it, one at a
        // time (each split binds a new pane, then the map is rebuilt so a following
        // split can target it).
        while (auto const op = findNewSplit(wireTab.root, controller))
        {
            auto const map = remoteToLocal(manager, window, controller);
            auto const acting = map.find(op->actingSession);
            if (acting == map.end())
                break; // the acting pane is not (yet) local — try again next push
            controller.setNextBindSession(op->newSession);
            manager.splitActivePane(op->vertical, acting->second, op->ratio);
        }
    }

    // Subtractive: close any local pane whose remote session vanished from the
    // layout — a pane closed on the daemon (by this or another client) or that
    // exited. Collect first (closing mutates the model), then activate each target
    // pane and close it (closeActivePane removes the tab's ACTIVE pane).
    auto layoutSessions = std::unordered_set<uint64_t> {};
    for (auto const& wireTab: layout->tabs)
        collectSessions(wireTab.root, layoutSessions);
    auto sessionMap = remoteToLocal(manager, window, controller);
    auto toClose = std::vector<contour::session::TerminalSession*> {};
    for (auto const& [remote, local]: sessionMap)
        if (!layoutSessions.contains(remote))
            toClose.push_back(local);
    for (auto* local: toClose)
    {
        auto const [win, tab, leaf] = manager.model().findSessionLeaf(local->modelSessionId());
        if (leaf == nullptr)
            continue;
        manager.model().setActivePane(tab->id(), leaf->id());
        // Detach, not Destroy: the daemon is where this pane went away, so authoring a close back
        // at it would at best echo a dead session id — and would destroy a live one the moment a
        // session can leave a window's layout without dying (a cross-window pane move;
        // vtworkspace::moveTabToWindow already models it).
        manager.closeActivePane(local, contour::session::SessionEnd::Detach);
    }
    if (!toClose.empty())
        sessionMap = remoteToLocal(manager, window, controller); // only a close can stale it

    // Ratios last, on the settled tree: the passes above add and remove panes, and closing one makes
    // its parent absorb the survivor's ratio. A divider is only meaningful once the shape is right.
    // Then, last of all, how each tab is presented: an active or zoomed pane is named by a leaf, so it
    // too waits for the tree.
    //
    // Each wire tab is paired with the local tab that sameTree() accepts — the identity check IS the
    // pairing, so there is no separate correspondence to look up and keep honest. applyRatios only
    // writes a double, and applyPresentation changes no tree, so neither the window nor the tabs move
    // under the loop.
    auto* win = manager.model().window(window);
    for (auto const [index, wireTab]: core::views::enumerate(layout->tabs))
        for (auto const i: std::views::iota(0, win != nullptr ? win->tabCount() : 0))
            if (auto* tab = win->tabAt(i); sameTree(sessionMap, wireTab.root, *tab->rootPane()))
            {
                applyRatios(manager, tab->id(), wireTab.root, *tab->rootPane());
                applyPresentation(manager.model(),
                                  sessionMap,
                                  wireTab,
                                  std::cmp_equal(index, layout->activeTab)
                                      ? vthost::client::TabActivity::Active
                                      : vthost::client::TabActivity::Inactive,
                                  *tab);
                break;
            }

    controller.setRealizingLayout(false);
}

} // namespace contour::remote
