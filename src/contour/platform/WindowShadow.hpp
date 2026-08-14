// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/WindowShadowTiles.hpp>

#include <memory>

class QWindow;

namespace contour::platform
{

/// Publishes a window's drop shadow to the compositor.
///
/// A frameless window has no server-side decoration, and on KWin the drop shadow is drawn as part
/// of that decoration -- so a client-decorated window has to hand the compositor its own shadow.
/// Two protocols do that, `_KDE_NET_WM_SHADOW` on X11 and `org_kde_kwin_shadow` on Wayland, and
/// this is what they have in common.
///
/// An interface rather than a free function, unlike its neighbour @ref setBlurBehind. That one
/// keeps its per-window state in a function-local `static std::map` keyed on QWindow* and cleans it
/// up from a lambda on QWindow::destroyed, which is exactly the ambient hidden state the
/// architecture rules forbid; it gets away with it because it owns one protocol object per window.
/// A shadow owns EIGHT server-side pixmaps or `wl_buffer`s per window, and their lifetime is the
/// hard part of this: freeing them early corrupts the shadow, freeing them late leaks server
/// memory on every scale change. Making it an object gives that lifetime a destructor at a known
/// point, and makes the whole apply/withdraw sequence testable against a recording double with no
/// compositor present. @ref setBlurBehind should eventually move behind the same seam.
class WindowShadow
{
  public:
    WindowShadow() = default;
    WindowShadow(WindowShadow const&) = delete;
    WindowShadow& operator=(WindowShadow const&) = delete;
    WindowShadow(WindowShadow&&) = delete;
    WindowShadow& operator=(WindowShadow&&) = delete;
    virtual ~WindowShadow() = default;

    /// Publishes @p tiles, replacing whatever was published before.
    ///
    /// Implementations skip the upload when the geometry is unchanged, so a caller may re-assert
    /// the current shadow as often as it likes.
    virtual void apply(WindowShadowTiles const& tiles) = 0;

    /// Withdraws the shadow: the window is maximized, server-decorated, or shadows are off.
    virtual void withdraw() = 0;
};

/// The shadow that does nothing, for every platform and compositor with no protocol for one.
///
/// Not an error case: GNOME/mutter, the wlroots compositors, Windows and macOS all end up here, the
/// last two because they give a frameless window a system shadow by other means.
class NullWindowShadow final: public WindowShadow
{
  public:
    void apply(WindowShadowTiles const& /*tiles*/) override {}
    void withdraw() override {}
};

/// The shadow attachment @p window's compositor can offer.
///
/// @param window The window to publish for. Note this resolves the platform window, so it must not
///               be called before the window is meant to be realized.
/// @return A NullWindowShadow wherever no protocol applies; never null.
[[nodiscard]] std::unique_ptr<WindowShadow> makeWindowShadow(QWindow& window);

} // namespace contour::platform
