// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/DesktopNotification.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace contour::platform
{

class NotificationTransport;

/// The desktop-notification capability a terminal session reaches for.
///
/// An interface rather than the concrete backend, for the reason Announcer gives: the DECISIONS --
/// which notification is raised, and what the session reports back over the PTY when the desktop
/// closes or activates one -- are the part worth testing, and a headless test has no notification
/// daemon to observe the delivery through. A recording implementation makes those decisions
/// assertable, which is how the OSC 99 `p=close` / `p=activated` replies are covered at all.
class Notifier: public QObject
{
    Q_OBJECT

  public:
    explicit Notifier(QObject* parent = nullptr): QObject(parent) {}

    /// Virtual through QObject already; stated so the contract does not rest on that being noticed.
    ~Notifier() override = default;

    /// Raises @p notification on the desktop, replacing an earlier one with the same identifier.
    ///
    /// Must not block: this is called from the terminal thread with the terminal's state mutex
    /// held. @see https://github.com/contour-terminal/contour/issues/2051
    virtual void notify(vtbackend::DesktopNotification const& notification) = 0;

    /// Asks the desktop to withdraw the notification carrying @p identifier. Must not block either.
    virtual void close(std::string const& identifier) = 0;

  signals:
    /// Emitted when a notification is closed by the desktop environment.
    ///
    /// @param identifier the OSC 99 identifier of the closed notification.
    /// @param reason the D-Bus close reason code (1=expired, 2=dismissed, 3=closed, 4=undefined).
    /// @param report whether the desktop reported the close, or a timer merely assumed it.
    void notificationClosed(QString identifier, uint reason, vtbackend::CloseReport report);

    /// Emitted when the user interacts with a notification.
    ///
    /// @param identifier the OSC 99 identifier of the activated notification.
    void actionInvoked(QString identifier);
};

/// A Notifier that raises nothing.
///
/// The default wherever no desktop notification service exists -- every platform but Linux, where
/// notifications go through the QML tray icon instead -- so call sites never have to ask whether
/// notifying is possible. The same role NullAnnouncer plays for announcements.
class NullNotifier final: public Notifier
{
    Q_OBJECT

  public:
    void notify(vtbackend::DesktopNotification const& /*notification*/) override {}
    void close(std::string const& /*identifier*/) override {}
};

/// Whether this process runs inside an application sandbox, and which.
enum class SandboxState : uint8_t
{
    Host = 0,    ///< No sandbox: the session bus is reachable by name.
    Flatpak = 1, ///< A Flatpak sandbox: only the portals are reachable without a static permission.
};

/// Which service a notifier should talk to.
enum class NotificationBackend : uint8_t
{
    FreeDesktop = 0, ///< org.freedesktop.Notifications on the session bus.
    Portal = 1,      ///< org.freedesktop.portal.Notification.
};

/// The notification service a given sandbox state can reach.
///
/// The portal is chosen ONLY when sandboxed, not wherever it happens to exist. On the host,
/// org.freedesktop.Notifications is strictly the more capable of the two: it reports that a
/// notification was dismissed, which the portal cannot, so preferring the portal everywhere would
/// trade away a working OSC 99 `c=1` close report for uniformity.
///
/// @param sandbox Where this process is running.
/// @return The backend to construct.
[[nodiscard]] constexpr NotificationBackend selectNotificationBackend(SandboxState sandbox) noexcept
{
    switch (sandbox)
    {
        case SandboxState::Host: return NotificationBackend::FreeDesktop;
        case SandboxState::Flatpak: return NotificationBackend::Portal;
    }
    return NotificationBackend::FreeDesktop;
}

/// A fresh portal-notification id namespace, unique within this process and across calls.
///
/// Portal notification ids are chosen by the application, and two things make a namespace
/// necessary. Between processes: the portal's ActionInvoked signal is broadcast and carries no app
/// id, so another sandboxed application's notification "1" would be indistinguishable from ours.
/// WITHIN one process: a transport is built per terminal session, and two panes are free to use the
/// same OSC 99 identifier -- so a per-process prefix alone would have them collide, replacing each
/// other's popups and each answering the other's activation. Hence unique per call, not per process.
///
/// @return The prefix, of the shape `contour-<pid>-<n>`.
[[nodiscard]] std::string makePortalIdPrefix();

/// The transport for a given backend.
///
/// Separate from makeDesktopNotifier() so the dispatch is reachable without the sandbox that would
/// otherwise be the only way to select the portal: on Linux every branch is constructible here, and
/// on other platforms this is always the null transport.
///
/// @param backend Which service to talk to.
/// @param closeDelay Passed to a backend that cannot observe a close; ignored by one that can.
/// @return The transport; never null.
[[nodiscard]] std::unique_ptr<NotificationTransport> makeNotificationTransport(
    NotificationBackend backend, std::chrono::milliseconds closeDelay);

/// The notifier this platform can offer.
///
/// A FreeDesktopNotifier on Linux -- over the session bus or over the portal, depending on whether
/// this process is sandboxed -- and a NullNotifier everywhere else. Mirrors makeSpeechSynthesizer():
/// the platform split lives here, so nothing above this layer needs an #ifdef to hold a notifier.
///
/// @param closeDelay How long a backend that cannot observe a close should wait before assuming
///                   one. Zero never assumes. Ignored by backends that can observe it.
/// @return The notifier; never null.
[[nodiscard]] std::unique_ptr<Notifier> makeDesktopNotifier(std::chrono::milliseconds closeDelay);

} // namespace contour::platform

// So the notificationClosed signal survives a queued connection, where Qt must copy its arguments
// through the metatype system rather than pass them on the stack.
Q_DECLARE_METATYPE(vtbackend::CloseReport)
