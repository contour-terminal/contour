// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/BlurBehind.hpp>
#include <contour/platform/KWinTitleBandMaterial.hpp>

#include <core/Environment.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <utility>

namespace contour::platform
{

KWinTitleBandMaterial::KWinTitleBandMaterial(config::HostPlatform desktop,
                                             std::function<DisplayServer()> server,
                                             TitleBandBlur blur):
    _desktop { desktop }, _server { std::move(server) }, _blur { std::move(blur) }
{
}

std::expected<void, TitleBandMaterialError> KWinTitleBandMaterial::apply(QWindow& window,
                                                                         TitleBandRequest const& request)
{
    auto const blurred = request.backdrop == config::BandBackdrop::Blurred;
    switch (_server())
    {
        case DisplayServer::Other:
            if (blurred)
                return std::unexpected(TitleBandMaterialError::Unsupported);
            return {};
        case DisplayServer::X11:
            // An X11 window manager that knows no blur ignores the property, and the translucent band would
            // show the desktop unblurred: only KWin is trusted to blur.
            if (blurred && _desktop != config::HostPlatform::KdePlasma)
                return std::unexpected(TitleBandMaterialError::Unsupported);
            break;
        case DisplayServer::Wayland: break;
    }
    // Clearing a blur that was never asked for is harmless; a failure to clear it is not worth a solid band.
    auto const applied = _blur(window, blurred ? request.height : 0.0);
    if (!blurred)
        return {};
    return applied;
}

NativeTitleBarMetrics KWinTitleBandMaterial::metrics(QWindow& /*window*/) const
{
    return {};
}

DisplayServer currentDisplayServer()
{
    auto const name = QGuiApplication::platformName();
    if (name == QStringLiteral("wayland"))
        return DisplayServer::Wayland;
    if (name == QStringLiteral("xcb"))
        return DisplayServer::X11;
    return DisplayServer::Other;
}

std::unique_ptr<ITitleBandMaterial> makeKWinTitleBandMaterial()
{
    auto const env = core::LiveEnvironment {};
    return std::make_unique<KWinTitleBandMaterial>(
        config::detectDesktopPlatform(env), currentDisplayServer, setTitleBandBlur);
}

} // namespace contour::platform
