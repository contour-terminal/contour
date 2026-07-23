// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/mux/AttachController.h>
#include <contour/mux/RemoteLayout.h>

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

namespace contour
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
    [[nodiscard]] bool anyLeafClaimed(vthost::proto::WirePane const& pane,
                                      AttachController const& controller)
    {
        if (pane.split == 0)
            return controller.isClaimed(pane.session);
        return std::ranges::any_of(pane.children,
                                   [&](auto const& child) { return anyLeafClaimed(child, controller); });
    }

    /// The acting (first) child's space share for a wire split ratio (units of
    /// 1/10000, the same encoding wireToLayoutPane decodes). Falls back to an even
    /// split for a missing or degenerate value, so a mirrored split never collapses
    /// to a zero-width pane.
    [[nodiscard]] double firstChildShare(uint16_t wireRatio)
    {
        auto const share = static_cast<double>(wireRatio) / 10000.0;
        return (share > 0.0 && share < 1.0) ? share : 0.5;
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
                                                      AttachController const& controller)
    {
        if (node.split == 0 || node.children.size() < 2)
            return std::nullopt;
        auto const& first = node.children[0];
        auto const& second = node.children[1];
        if (first.split == 0 && controller.isBound(first.session) && !anyLeafClaimed(second, controller))
            return SplitOp { .actingSession = first.session,
                             .newSession = leftmostSession(second),
                             .vertical = node.split == 2,
                             .ratio = firstChildShare(node.ratio) };
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
    [[nodiscard]] std::unordered_map<uint64_t, TerminalSession*> remoteToLocal(
        TerminalSessionManager& manager, vtworkspace::WindowId window, AttachController const& controller)
    {
        auto out = std::unordered_map<uint64_t, TerminalSession*> {};
        auto* win = manager.model().window(window);
        if (win == nullptr)
            return out;
        for (auto const i: std::views::iota(0, win->tabCount()))
            for (auto* session: manager.sessionsOfTab(win->tabAt(i)))
                if (auto const remote = controller.sessionForPty(&session->terminal().device()))
                    out.emplace(*remote, session);
        return out;
    }

    /// Realizes ONE whole daemon tab (no leaf yet bound) via the shared realizer.
    void realizeWholeTab(TerminalSessionManager& manager,
                         vtworkspace::WindowId window,
                         AttachController& controller,
                         vthost::proto::WireTab const& wireTab,
                         std::optional<vtbackend::PageSize> pageSize)
    {
        auto single = vthost::proto::LayoutState {};
        single.tabs.push_back(wireTab);
        auto const wl = vthost::client::wireToLayout(single);
        manager.applyLayoutToWindow(window, wl.layout, pageSize, [&](config::LayoutPane const& leaf) {
            auto const it = wl.leafSession.find(&leaf);
            if (it != wl.leafSession.end())
                controller.setNextBindSession(it->second);
        });
    }
} // namespace

void applyRemoteLayout(TerminalSessionManager& manager,
                       vtworkspace::WindowId window,
                       AttachController& controller,
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
    auto toClose = std::vector<TerminalSession*> {};
    for (auto const& [remote, local]: remoteToLocal(manager, window, controller))
        if (!layoutSessions.contains(remote))
            toClose.push_back(local);
    for (auto* local: toClose)
    {
        auto* win = manager.model().window(window);
        if (win == nullptr)
            break;
        for (auto const i: std::views::iota(0, win->tabCount()))
        {
            auto* tab = win->tabAt(i);
            if (auto* leaf = tab->rootPane()->findLeaf(local->modelSessionId()))
            {
                manager.model().setActivePane(tab->id(), leaf->id());
                manager.closeActivePane(local);
                break;
            }
        }
    }

    controller.setRealizingLayout(false);
}

} // namespace contour
