// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The title band's material on KDE Plasma: KWin blurs what is behind the band, which paints a translucent
/// tint over it (`tab_style.material: frosted | glass`).

#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <expected>
#include <functional>
#include <memory>

class QWindow;

namespace contour::platform
{

/// Sets or clears the blur behind a window's title band; setTitleBandBlur() in production, a recorder in
/// tests.
using TitleBandBlur = std::function<std::expected<void, TitleBandMaterialError>(QWindow&, qreal)>;

/// Which windowing system the application runs on, as far as a compositor's blur goes.
enum class DisplayServer : uint8_t
{
    Other = 0, ///< Neither: no compositor blur is reachable.
    Wayland,   ///< A Wayland compositor, which says itself whether it blurs (org_kde_kwin_blur).
    X11,       ///< An X11 window manager, which ignores a blur property it does not know.
};

/// The title band's material on a compositor that blurs behind a region of a window.
///
/// A Wayland compositor answers for itself whether it blurs; an X11 window manager silently ignores the
/// property, which would leave a translucent band over an unblurred desktop, so on X11 only KDE Plasma's
/// KWin is trusted to blur and every other desktop reports the material unsupported. The window then paints
/// the solid band (R-g).
class KWinTitleBandMaterial final: public ITitleBandMaterial
{
  public:
    /// @param desktop The desktop the session runs (config::detectDesktopPlatform).
    /// @param server  Which windowing system the platform plugin speaks; asked when the material is applied,
    ///                not here, because the material is built before a QGuiApplication exists.
    /// @param blur    Sets or clears the blur behind a band.
    KWinTitleBandMaterial(config::HostPlatform desktop,
                          std::function<DisplayServer()> server,
                          TitleBandBlur blur);

    /// Blurs behind the band for BandBackdrop::Blurred, and clears that blur for every other backdrop.
    /// @param window  The window whose band to dress.
    /// @param request What to draw behind it; its height is the region blurred.
    /// @return Unsupported where no compositor blurs behind the band; nothing otherwise.
    [[nodiscard]] std::expected<void, TitleBandMaterialError> apply(QWindow& window,
                                                                    TitleBandRequest const& request) override;

    /// @return Zeros: the band has no native title bar under it.
    [[nodiscard]] NativeTitleBarMetrics metrics(QWindow& window) const override;

    /// @return A compositor is all this material needs: a client-decorated window has no OS title bar.
    [[nodiscard]] BandMaterialHost host() const noexcept override { return BandMaterialHost::Compositor; }

  private:
    config::HostPlatform _desktop;
    std::function<DisplayServer()> _server;
    TitleBandBlur _blur;
};

/// @return The windowing system the running platform plugin speaks.
[[nodiscard]] DisplayServer currentDisplayServer();

/// Creates the KWin material for the session's desktop.
/// @return The material.
[[nodiscard]] std::unique_ptr<ITitleBandMaterial> makeKWinTitleBandMaterial();

} // namespace contour::platform
