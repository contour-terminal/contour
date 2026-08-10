// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/DesktopNotification.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <string>

namespace contour::platform
{

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
    void notificationClosed(QString identifier, uint reason);

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

/// The notifier this platform can offer.
///
/// A FreeDesktopNotifier over D-Bus on Linux, a NullNotifier everywhere else. Mirrors
/// makeSpeechSynthesizer(): the platform split lives here, so nothing above this layer needs an
/// #ifdef to hold a notifier.
[[nodiscard]] std::unique_ptr<Notifier> makeDesktopNotifier();

} // namespace contour::platform
