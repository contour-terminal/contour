// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSessionManager.h>
#include <contour/mux/AttachController.h>
#include <contour/mux/RemoteLayout.h>

#include <algorithm>

#include <muxserver/client/LayoutReconstruction.h>
#include <muxserver/proto/Pdu.h>

namespace contour
{

namespace
{
    /// @return True if any leaf of @p pane carries a remote session already bound to
    ///         a local pane — i.e. this subtree is already realized.
    [[nodiscard]] bool anyLeafBound(muxserver::proto::WirePane const& pane,
                                    AttachController const& controller)
    {
        if (pane.split == 0)
            return controller.isBound(pane.session);
        return std::ranges::any_of(pane.children,
                                   [&](auto const& child) { return anyLeafBound(child, controller); });
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

    // Incremental reconciliation: realize each daemon tab that is not already
    // represented locally (none of its leaves bound). This runs on every layout
    // push, so a tab the user authors on the daemon (B3-Qt) — or that another
    // client added — appears here without rebuilding the tabs already shown.
    controller.setRealizingLayout(true);
    for (auto const& wireTab: layout->tabs)
    {
        if (anyLeafBound(wireTab.root, controller))
            continue;

        // Realize this ONE tab. A fresh single-tab conversion keeps the leaf→session
        // map's pane addresses stable for exactly this realization.
        auto single = muxserver::proto::LayoutState {};
        single.tabs.push_back(wireTab);
        auto const wl = muxserver::client::wireToLayout(single);
        manager.applyLayoutToWindow(window, wl.layout, pageSize, [&](config::LayoutPane const& leaf) {
            // Bind the pane about to be born to its remote session. The seeder hands
            // us the leaf by reference INTO wl.layout, so its address keys the map.
            auto const it = wl.leafSession.find(&leaf);
            if (it != wl.leafSession.end())
                controller.setNextBindSession(it->second);
        });
    }
    controller.setRealizingLayout(false);
}

} // namespace contour
