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

#include <muxserver/client/LayoutReconstruction.h>
#include <muxserver/proto/Pdu.h>
#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

namespace contour
{

namespace
{
    /// The leftmost leaf's session of a subtree (the pane that seeded it).
    [[nodiscard]] uint64_t leftmostSession(muxserver::proto::WirePane const& pane)
    {
        auto const* node = &pane;
        while (node->split != 0 && !node->children.empty())
            node = &node->children.front();
        return node->session;
    }

    /// Collects every leaf session of @p pane into @p out.
    void collectSessions(muxserver::proto::WirePane const& pane, std::unordered_set<uint64_t>& out)
    {
        if (pane.split == 0)
        {
            out.insert(pane.session);
            return;
        }
        for (auto const& child: pane.children)
            collectSessions(child, out);
    }

    /// @return True if any leaf of @p pane carries a remote session already bound to
    ///         a local pane — i.e. this subtree is already realized locally.
    [[nodiscard]] bool anyLeafBound(muxserver::proto::WirePane const& pane,
                                    AttachController const& controller)
    {
        if (pane.split == 0)
            return controller.isBound(pane.session);
        return std::ranges::any_of(pane.children,
                                   [&](auto const& child) { return anyLeafBound(child, controller); });
    }

    /// One local split to perform to catch up with a daemon-side split: split the
    /// pane hosting @c actingSession (the split's surviving first child) to add a
    /// pane for @c newSession (the freshly created second child).
    struct SplitOp
    {
        uint64_t actingSession = 0;
        uint64_t newSession = 0;
        bool vertical = false;
    };

    /// Finds ONE not-yet-realized split in @p node: a split whose first child is a
    /// BOUND leaf and whose second child's subtree is entirely UNBOUND (the shape a
    /// daemon `splitActivePane` leaves — old session in the first child, new in the
    /// second). Recurses through already-bound subtrees to find nested new splits.
    [[nodiscard]] std::optional<SplitOp> findNewSplit(muxserver::proto::WirePane const& node,
                                                      AttachController const& controller)
    {
        if (node.split == 0 || node.children.size() < 2)
            return std::nullopt;
        auto const& first = node.children[0];
        auto const& second = node.children[1];
        if (first.split == 0 && controller.isBound(first.session) && !anyLeafBound(second, controller))
            return SplitOp { .actingSession = first.session,
                             .newSession = leftmostSession(second),
                             .vertical = node.split == 2 };
        if (anyLeafBound(first, controller))
            if (auto op = findNewSplit(first, controller))
                return op;
        if (anyLeafBound(second, controller))
            if (auto op = findNewSplit(second, controller))
                return op;
        return std::nullopt;
    }

    /// Maps each remote session to its local TerminalSession, by matching the pane's
    /// pty against the controller's bindings. Rebuilt after each split so a
    /// just-created pane is available to seed the next.
    [[nodiscard]] std::unordered_map<uint64_t, TerminalSession*> remoteToLocal(
        TerminalSessionManager& manager, vtmux::WindowId window, AttachController const& controller)
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
                         vtmux::WindowId window,
                         AttachController& controller,
                         muxserver::proto::WireTab const& wireTab,
                         std::optional<vtbackend::PageSize> pageSize)
    {
        auto single = muxserver::proto::LayoutState {};
        single.tabs.push_back(wireTab);
        auto const wl = muxserver::client::wireToLayout(single);
        manager.applyLayoutToWindow(window, wl.layout, pageSize, [&](config::LayoutPane const& leaf) {
            auto const it = wl.leafSession.find(&leaf);
            if (it != wl.leafSession.end())
                controller.setNextBindSession(it->second);
        });
    }
} // namespace

void applyRemoteLayout(TerminalSessionManager& manager,
                       vtmux::WindowId window,
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
        if (!anyLeafBound(wireTab.root, controller))
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
            manager.splitActivePane(op->vertical, acting->second);
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
