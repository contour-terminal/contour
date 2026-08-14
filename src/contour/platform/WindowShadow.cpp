// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowShadow.hpp>

#ifdef CONTOUR_FRONTEND_XCB
    #include <contour/platform/X11WindowShadow.hpp>
#endif

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    #include <contour/platform/WaylandWindowShadow.hpp>
#endif

namespace contour::platform
{

std::unique_ptr<WindowShadow> makeWindowShadow([[maybe_unused]] QWindow& window)
{
    // Each backend decides for itself whether it applies -- it knows the platform name and, on
    // Wayland, whether the compositor even offers the protocol -- and answers nullptr when it does
    // not. So this is an ordered list of candidates, not a switch on the platform, and a build with
    // neither backend compiled in still returns something usable.
#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto shadow = makeWaylandWindowShadow(window))
        return shadow;
#endif

#ifdef CONTOUR_FRONTEND_XCB
    if (auto shadow = makeX11WindowShadow(window))
        return shadow;
#endif

    // Windows and macOS land here by design, not by omission: both give a client-decorated window a
    // system shadow through their own frame APIs (@see NativeWindowFrame), so there is nothing for
    // a shadow protocol to do. GNOME/mutter and the wlroots compositors land here because no such
    // protocol exists for them at all.
    return std::make_unique<NullWindowShadow>();
}

} // namespace contour::platform
