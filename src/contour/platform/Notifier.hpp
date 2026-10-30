// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/UserNotificationCenter.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

#include <vtpty/SandboxInfo.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <chrono>
#include <cstdint>
#include <functional>
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
    /// Must not block: it is called on the GUI thread, which a wait on the desktop would freeze.
    /// @see https://github.com/contour-terminal/contour/issues/2051
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
/// What there is wherever no desktop notification service exists and nothing else was offered to
/// raise a notification through, so call sites never have to ask whether notifying is possible. The
/// same role NullAnnouncer plays for announcements.
class NullNotifier final: public Notifier
{
    Q_OBJECT

  public:
    void notify(vtbackend::DesktopNotification const& /*notification*/) override {}
    void close(std::string const& /*identifier*/) override {}
};

/// A Notifier that hands each notification to a window's tray, as a message.
///
/// Where there is no desktop notification service -- every platform but Linux -- a session's notices and
/// its applications' notifications go to the tray of the window hosting it, so a pane in a background
/// tab, which has no chrome on screen to show anything, is heard from all the same. A tray message cannot
/// be withdrawn, so close() does nothing and no close is ever reported. A click on it is reported as the
/// notification's activation, by whoever shows the message telling it when one happens.
class TrayNotifier final: public Notifier
{
    Q_OBJECT

  public:
    /// How a message reaches a tray: its title, its body, and what a click on it does.
    using ShowMessage =
        std::function<void(QString const& title, QString const& body, std::function<void()> onClicked)>;

    /// @param showMessage How a message reaches the tray.
    /// @param parent      Qt ownership, or nullptr.
    explicit TrayNotifier(ShowMessage showMessage, QObject* parent = nullptr):
        Notifier(parent), _showMessage { std::move(showMessage) }
    {
    }

    /// Shows @p notification as a tray message. A tray message shows no sender, so an application's is
    /// titled as one, and cannot pass for a notice of Contour's own.
    /// @param notification The notification.
    void notify(vtbackend::DesktopNotification const& notification) override;

    void close(std::string const& /*identifier*/) override {}

  private:
    ShowMessage _showMessage;
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
/// vtpty owns the sandbox vocabulary, because vtpty is what reads /.flatpak-info -- and reads more
/// than this out of it. @see vtpty::SandboxInfo. Host means the session bus is reachable by name;
/// Flatpak means only the portals are, without a static permission.
///
/// @param sandbox Where this process is running.
/// @return The backend to construct.
[[nodiscard]] constexpr NotificationBackend selectNotificationBackend(vtpty::SandboxState sandbox) noexcept
{
    switch (sandbox)
    {
        case vtpty::SandboxState::Host: return NotificationBackend::FreeDesktop;
        case vtpty::SandboxState::Flatpak: return NotificationBackend::Portal;
    }
    return NotificationBackend::FreeDesktop;
}

/// The transport for a given backend.
///
/// Separate from makeDesktopNotifier() so the dispatch is reachable without the sandbox that would
/// otherwise be the only way to select the portal: on Linux every branch is constructible here, and
/// on other platforms this is always the null transport.
///
/// @param backend Which service to talk to.
/// @param closeDelay Passed to a backend that cannot observe a close, for a notification that asked to
///                   hear of it; ignored by one that can.
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
///                   one, for a notification that asked to hear of it (OSC 99 `c=1`); any other is
///                   never assumed closed. Zero never assumes. Ignored by backends that can observe it.
/// @return The notifier; never null.
[[nodiscard]] std::unique_ptr<Notifier> makeDesktopNotifier(std::chrono::milliseconds closeDelay);

/// The notifier this platform can offer, or where it offers none, one that shows a tray message.
///
/// makeDesktopNotifier() on Linux, and a TrayNotifier everywhere else: the platform split of a
/// session's notices lives here too, so the session raises every notice through its notifier alike.
///
/// @param closeDelay      Passed to makeDesktopNotifier().
/// @param showTrayMessage How a message reaches the tray, where there is no desktop to raise it on.
/// @return The notifier; never null.
[[nodiscard]] std::unique_ptr<Notifier> makeDesktopNotifier(std::chrono::milliseconds closeDelay,
                                                            TrayNotifier::ShowMessage const& showTrayMessage);

/// Which notifier a session or the application raises through.
enum class SessionNotifierKind : uint8_t
{
    PlatformDefault = 0, ///< The platform's own: FreeDesktop or the portal on Linux, the tray elsewhere.
    UserNotifications,   ///< The operating system's user-notification centre (macOS Notification Center).
};

/// The notifier kind for a host and the centre it can reach.
///
/// The host is the one the app was given (config::HostPlatform), not a compile-time choice, so a test
/// reaches the macOS path on any OS. Only macOS has a centre that replaces the platform default; Linux
/// keeps FreeDesktop or the portal and Windows keeps the tray.
/// @param host The host the app runs on.
/// @param centre Whether the app's notification centre raises anything.
/// @return UserNotifications on macOS with an Available centre, else PlatformDefault.
[[nodiscard]] constexpr SessionNotifierKind selectSessionNotifier(
    config::HostPlatform host, UserNotificationAvailability centre) noexcept
{
    switch (host)
    {
        case config::HostPlatform::MacOS:
            return centre == UserNotificationAvailability::Available ? SessionNotifierKind::UserNotifications
                                                                     : SessionNotifierKind::PlatformDefault;
        case config::HostPlatform::Windows:
        case config::HostPlatform::KdePlasma:
        case config::HostPlatform::Other: break;
    }
    return SessionNotifierKind::PlatformDefault;
}

/// The notifier of one owner (a session, or the application) on a host that may have a notification centre.
///
/// A UserNotificationNotifier over @p centre when selectSessionNotifier() says so, whose fallback is the
/// tray, for when the centre turns out unusable; otherwise the platform's own, as the
/// overload without a centre builds it.
///
/// @param closeDelay      Passed to the notifier chosen.
/// @param showTrayMessage How a message reaches the tray, where the platform has no desktop to raise it on.
/// @param centre          The application's notification centre; it must outlive the notifier.
/// @param requestPrefix   What the owner's request ids start with in the centre.
/// @param host            The host the app runs on.
/// @return The notifier; never null.
[[nodiscard]] std::unique_ptr<Notifier> makeDesktopNotifier(std::chrono::milliseconds closeDelay,
                                                            TrayNotifier::ShowMessage const& showTrayMessage,
                                                            IUserNotificationCenter& centre,
                                                            std::string requestPrefix,
                                                            config::HostPlatform host);

} // namespace contour::platform

// So the notificationClosed signal survives a queued connection, where Qt must copy its arguments
// through the metatype system rather than pass them on the stack.
Q_DECLARE_METATYPE(vtbackend::CloseReport)
