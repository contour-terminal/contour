// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/mux/AttachController.h>
#include <contour/mux/RemoteLayout.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_map>

#include <muxserver/client/LayoutReconstruction.h>
#include <muxserver/proto/Pdu.h>
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
                       std::optional<vtbackend::PageSize> pageSize)
{
    auto const layout = controller.layout();
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
    controller.setRealizingLayout(false);
}

} // namespace contour
