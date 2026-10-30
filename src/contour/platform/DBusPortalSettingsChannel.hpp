// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/DBusSignalSubscription.hpp>
    #include <contour/platform/PortalSettings.hpp>
    #include <contour/platform/PortalSettingsChannel.hpp>

    #include <QtCore/QObject>
    #include <QtCore/QString>
    #include <QtCore/QVariant>
    #include <QtDBus/QDBusVariant>

    #include <string>
    #include <vector>

namespace contour::platform
{

/// One setting's value as QtDBus hands it over, decoded.
///
/// Nested QDBusVariants are unwrapped first (old portals wrap the value twice); then a `uint` or a
/// non-negative `int` is a std::uint32_t, a string a std::string, and a `(ddd)` structure a PortalColor.
/// @param value The value of one `a{sv}` entry, or of SettingChanged's `v`.
/// @return The decoded value; std::monostate for any other type.
[[nodiscard]] PortalSettingValue portalSettingValueOf(QVariant const& value);

/// Speaks org.freedesktop.portal.Settings over the session bus, asynchronously.
///
/// As DBusNotificationTransport, deliberately not a QDBusInterface, whose constructor introspects the
/// remote object synchronously: every call here is an asyncCall, so a portal that never answers costs
/// at most DBusCallTimeoutMilliseconds, and never on the calling thread.
class DBusPortalSettingsChannel final: public QObject, public IPortalSettingsChannel
{
    Q_OBJECT

  public:
    /// Subscribes to SettingChanged, without waiting for the bus.
    /// @param namespaces The namespaces ReadAll asks for; a change in any other is dropped.
    /// @param onChange Called on the GUI thread with each change of one of @p namespaces; may be empty.
    /// @param subscriber Installs the SettingChanged subscription.
    /// @pre A QCoreApplication exists (ruling R-i: the sources are built on first read, after it).
    DBusPortalSettingsChannel(std::vector<std::string> namespaces,
                              PortalSettingChanged onChange,
                              DBusSignalSubscriber const& subscriber = sessionBusSubscriber());

    /// Sends ReadAll for the channel's namespaces and returns without waiting. The reply watcher is
    /// owned by this channel, so a reply after its destruction reaches nothing.
    /// @param onReply Called on the GUI thread with the settings; NoPortal when the bus says there is no
    ///                Settings portal, Failed on any other error, a timeout included.
    void readAll(PortalSettingsReply onReply) override;

  private slots:
    /// Handles the SettingChanged D-Bus signal. Declares every parameter the signal carries; see
    /// DBusSignalSubscription.
    void onSettingChanged(QString const& group, QString const& key, QDBusVariant const& value);

  private:
    std::vector<std::string> _namespaces;
    PortalSettingChanged _onChange;
    /// The installed match rule, removed when this dies. @see DBusSignalSubscriptionGuard.
    DBusSignalSubscriptionGuard _subscription;
};

/// @return A factory building DBusPortalSettingsChannels on the session bus.
[[nodiscard]] PortalSettingsChannelFactory dbusPortalSettingsChannels();

} // namespace contour::platform

#endif // defined(__linux__)
