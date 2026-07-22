// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSessionManager.h>
#include <contour/mux/AttachController.h>
#include <contour/mux/RemoteLayout.h>

#include <muxserver/client/LayoutReconstruction.h>

namespace contour
{

void applyRemoteLayout(TerminalSessionManager& manager,
                       vtmux::WindowId window,
                       AttachController& controller,
                       std::optional<vtbackend::PageSize> pageSize)
{
    auto const wl = controller.wireLayout();
    if (wl.layout.tabs.empty())
        return;

    // Allow session creation with nothing pending — during realization the panes
    // are bound by setNextBindSession, not the FIFO queue.
    controller.setRealizingLayout(true);
    manager.applyLayoutToWindow(window, wl.layout, pageSize, [&](config::LayoutPane const& leaf) {
        // Bind the pane about to be born to its remote session. The seeder hands us
        // the leaf by reference INTO wl.layout, so its address keys the map.
        auto const it = wl.leafSession.find(&leaf);
        if (it != wl.leafSession.end())
            controller.setNextBindSession(it->second);
    });
    controller.setRealizingLayout(false);
}

} // namespace contour
