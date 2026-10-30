// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An org.freedesktop.portal.Settings for the private session bus a harness stands up: it serves ReadAll
/// from a table the test supplies and emits SettingChanged on request.

#ifdef __linux__

    #include <contour/platform/PortalCall.hpp>

    #include <QtCore/QMap>
    #include <QtCore/QStringList>
    #include <QtCore/QTimer>
    #include <QtCore/QVariantMap>
    #include <QtDBus/QDBusArgument>
    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusConnectionInterface>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusMetaType>
    #include <QtDBus/QDBusVariant>
    #include <QtDBus/QDBusVirtualObject>

    #include <chrono>
    #include <utility>

namespace contour::test
{

/// The settings a FakeSettingsPortal serves: namespace to key to value, as ReadAll's `a{sa{sv}}`.
using FakePortalTable = QMap<QString, QVariantMap>;

/// @return A structure of @p fields as the wire carries it, for a FakePortalTable entry of any shape -- a
///         malformed colour, say.
template <typename... Fields>
[[nodiscard]] QVariant portalStructureValue(Fields const&... fields)
{
    auto argument = QDBusArgument {};
    argument.beginStructure();
    (argument << ... << fields);
    argument.endStructure();
    return QVariant::fromValue(argument);
}

/// @return A portal colour as the wire carries it, a `(ddd)` structure, ready for a FakePortalTable.
[[nodiscard]] inline QVariant portalColorValue(double red, double green, double blue)
{
    return portalStructureValue(red, green, blue);
}

/// The object a FakeSettingsPortal exports at the portal path: answers ReadAll, and nothing else.
///
/// A virtual object rather than an adaptor, so the D-Bus method needs no C++ slot spelled `ReadAll`.
class FakeSettingsPortalObject final: public QDBusVirtualObject
{
  public:
    /// @param table What ReadAll serves; it must outlive this object.
    /// @param replyDelay How long ReadAll holds its reply back.
    FakeSettingsPortalObject(FakePortalTable const& table, std::chrono::milliseconds replyDelay):
        _table { table }, _replyDelay { replyDelay }
    {
    }

    /// @return No introspection data: nothing here introspects the fake.
    [[nodiscard]] QString introspect(QString const& /*path*/) const override { return {}; }

    /// Answers a ReadAll with the table's entries for the namespaces asked for (all of them for none),
    /// once the reply delay has passed. The timer is this object's, so a fake that dies first sends nothing.
    /// @param message The incoming message.
    /// @param connection The connection it came in on.
    /// @return Whether the message was a ReadAll; the signature is QDBusVirtualObject's.
    bool handleMessage(QDBusMessage const& message, QDBusConnection const& connection) override
    {
        if (message.member() != QStringLiteral("ReadAll"))
            return false;
        auto const namespaces = message.arguments().value(0).toStringList();
        auto served = FakePortalTable {};
        for (auto const& [group, values]: _table.asKeyValueRange())
            if (namespaces.isEmpty() || namespaces.contains(group))
                served.insert(group, values);
        QTimer::singleShot(_replyDelay, this, [message, connection, served] {
            connection.send(message.createReply(QVariant::fromValue(served)));
        });
        return true;
    }

  private:
    FakePortalTable const& _table;
    std::chrono::milliseconds _replyDelay;
};

/// A Settings portal registered as org.freedesktop.portal.Desktop on its own connection to the session
/// bus, for as long as it lives.
///
/// Its own connection, so the channel's calls cross the bus rather than being served inside this
/// process's session-bus connection, where a delayed reply would not exercise the wire.
class FakeSettingsPortal
{
  public:
    /// The name of the connection the fake portal is served on.
    static constexpr auto ConnectionName = QLatin1StringView("contour-fake-portal");

    /// Registers the portal.
    /// @param table What ReadAll serves.
    /// @param replyDelay How long ReadAll holds its reply back.
    explicit FakeSettingsPortal(FakePortalTable table,
                                std::chrono::milliseconds replyDelay = std::chrono::milliseconds { 0 }):
        _table { std::move(table) },
        _bus { QDBusConnection::connectToBus(QDBusConnection::SessionBus, ConnectionName) },
        _object { _table, replyDelay }
    {
        qDBusRegisterMetaType<FakePortalTable>();
        _bus.registerVirtualObject(platform::PortalPath, &_object);
        _bus.registerService(platform::PortalService);
    }

    FakeSettingsPortal(FakeSettingsPortal const&) = delete;
    FakeSettingsPortal& operator=(FakeSettingsPortal const&) = delete;
    FakeSettingsPortal(FakeSettingsPortal&&) = delete;
    FakeSettingsPortal& operator=(FakeSettingsPortal&&) = delete;

    ~FakeSettingsPortal()
    {
        _bus.unregisterService(platform::PortalService);
        _bus.unregisterObject(platform::PortalPath);
        QDBusConnection::disconnectFromBus(ConnectionName);
    }

    /// @return Whether the bus names this fake as the portal's owner.
    [[nodiscard]] bool isRegistered() const
    {
        auto const owner = _bus.interface()->serviceOwner(platform::PortalService);
        return owner.isValid() && owner.value() == _bus.baseService();
    }

    /// Emits SettingChanged(@p group, @p key, @p value) from the portal.
    /// @param group The namespace.
    /// @param key The key.
    /// @param value The new value, wrapped in the signal's `v`.
    void emitSettingChanged(QString const& group, QString const& key, QVariant const& value)
    {
        auto signal = QDBusMessage::createSignal(platform::PortalPath,
                                                 QStringLiteral("org.freedesktop.portal.Settings"),
                                                 QStringLiteral("SettingChanged"));
        signal << group << key << QVariant::fromValue(QDBusVariant { value });
        _bus.send(signal);
    }

  private:
    FakePortalTable _table;
    QDBusConnection _bus;
    FakeSettingsPortalObject _object;
};

} // namespace contour::test

#endif // defined(__linux__)
