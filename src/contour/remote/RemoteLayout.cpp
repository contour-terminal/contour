// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/NativeController.h>
#include <contour/remote/RemoteLayout.h>
#include <contour/session/TerminalSession.h>
#include <contour/session/TerminalSessionManager.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vthost/client/LayoutReconstruction.h>
#include <vthost/proto/Pdu.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/SessionModel.h>
#include <vtworkspace/Tab.h>

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
    //
    // Each wire tab is paired with the local tab that sameTree() accepts — the identity check IS the
    // pairing, so there is no separate correspondence to look up and keep honest. applyRatios only
    // writes a double, so neither the window nor the tabs move under the loop.
    auto* win = manager.model().window(window);
    for (auto const& wireTab: layout->tabs)
        for (auto const i: std::views::iota(0, win != nullptr ? win->tabCount() : 0))
            if (auto* tab = win->tabAt(i); sameTree(sessionMap, wireTab.root, *tab->rootPane()))
            {
                applyRatios(manager, tab->id(), wireTab.root, *tab->rootPane());
                break;
            }

    controller.setRealizingLayout(false);
}

} // namespace contour::remote
