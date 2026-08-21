// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/ExternalLauncher.hpp>
    #include <contour/platform/PortalCall.hpp>
    #include <contour/platform/QtExternalLauncher.hpp>

    #include <QtCore/QLatin1StringView>
    #include <QtCore/QObject>

namespace contour::platform
{

/// The portal interface this launcher speaks. Named here rather than in the source file because the
/// factory that builds the launcher's PortalCaller lives beside it. @see NotificationPortalInterface.
constexpr auto OpenUriPortalInterface = QLatin1StringView("org.freedesktop.portal.OpenURI");

/// Builds the arguments for org.freedesktop.portal.OpenURI.OpenURI, in wire order.
///
/// A free function rather than a member, for the same reason buildPortalNotificationOptions() is:
/// it is the part a headless test can check head-on, without a bus or a portal.
///
/// The parent_window is always empty. A real handle would parent the portal's "open with" chooser
/// to our window, but obtaining one means either Qt's private portalWindowIdentifier() or an
/// xdg_foreign export round trip; Qt itself passes empty whenever nothing has focus, and the only
/// cost is a chooser that is not tied to our window, in the case where the desktop shows one at all.
///
/// @param url The resource to open.
/// @return (parent_window, uri, options), the OpenURI signature `(ssa{sv})`.
[[nodiscard]] QVariantList buildOpenUriArguments(QUrl const& url);

/// Opens URLs through org.freedesktop.portal.OpenURI, asynchronously.
///
/// **Why this exists at all, and why it is NOT selected by SandboxState** -- the asymmetry with
/// selectNotificationBackend() is deliberate and is the surprising part:
///
/// Qt's QDesktopServices::openUrl() reaches the portal with `QDBusConnection::sessionBus().call()`,
/// a BLOCKING call on the calling thread at D-Bus's 25-second default. Through Qt 6.9 it took that
/// path only when checkNeedPortalSupport() said so -- a /.flatpak-info and snap test -- so it could
/// only freeze a sandboxed Contour. Qt 6.10 removed the gate:
///
///     bool QDesktopUnixServices::openUrl(const QUrl &url)
///     {
///         if (!m_hasNoPortal)
///             return runWithXdgActivationToken(&QDesktopUnixServices::openUrlWithPortal, url);
///         return runWithXdgActivationToken(&QDesktopUnixServices::openUrlWithoutPortal, url);
///     }
///
/// and m_hasNoPortal only becomes true once a probe answers ServiceUnknown. A portal that EXISTS
/// but is wedged leaves it false, so on a current Qt every hyperlink click can freeze the window on
/// any desktop, sandboxed or not. That is why this replaces Qt's openUrl wherever D-Bus is
/// available, rather than only inside a sandbox.
/// @see https://github.com/contour-terminal/contour/issues/2075 and, for the same class of bug in a
/// call we do own, issue #2051.
///
/// The fallback for a portal that answers with an error is xdg-open, NOT QDesktopServices::openUrl
/// -- which on Qt 6.10+ would put the blocking call straight back. It is only ever reached after a
/// reply has arrived, so the portal is by then known to be responsive and merely refusing.
class PortalExternalLauncher final: public QObject, public ExternalLauncher
{
    Q_OBJECT

  public:
    /// @param caller How a portal method call is issued; required, so a test can drive it by hand.
    /// @param parent Qt ownership, or nullptr.
    explicit PortalExternalLauncher(PortalCaller caller, QObject* parent = nullptr);

    [[nodiscard]] std::expected<void, LaunchError> openUrl(QUrl const& url) override;
    bool runDetached(QString const& program, QStringList const& arguments) override;
    int execute(QString const& program, QStringList const& arguments) override;

  private:
    /// How a portal method call is issued.
    PortalCaller _call;

    /// The process-spawning half, which the portal has nothing to do with: runDetached() and
    /// execute() start a child directly on both paths, so they are delegated rather than repeated.
    /// It is also what the xdg-open fallback goes through.
    QtExternalLauncher _processes;
};

} // namespace contour::platform

#endif // defined(__linux__)
