// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An org.freedesktop.portal.GlobalShortcuts for the private session bus a harness stands up: it answers
/// CreateSession, BindShortcuts and Session.Close the way the portal does (a request object path, then a
/// Response signal on it), records what it was asked, and emits Activated on request.

#ifdef __linux__

    #include <contour/platform/DBusGlobalShortcutsChannel.hpp>
    #include <contour/platform/PortalCall.hpp>

    #include <QtCore/QList>
    #include <QtCore/QStringList>
    #include <QtCore/QTimer>
    #include <QtCore/QVariantMap>
    #include <QtDBus/QDBusArgument>
    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusConnectionInterface>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusMetaType>
    #include <QtDBus/QDBusObjectPath>
    #include <QtDBus/QDBusVirtualObject>

    #include <cstdint>
    #include <utility>

namespace contour::test
{

/// What a FakeGlobalShortcutsPortal answers and what it has been asked.
struct FakeShortcutsState
{
    /// The Response code of the next CreateSession (0 success, 1 cancelled, 2 other).
    uint createResponse = 0;
    /// The Response code of the next BindShortcuts.
    uint bindResponse = 0;
    /// The session handles it handed out, in order.
    QStringList sessions;
    /// Every shortcut of every BindShortcuts, in order.
    QList<platform::PortalShortcut> bound;
    /// The session handles it was asked to close, in order.
    QStringList closed;
    /// Whether it implements org.freedesktop.host.portal.Registry; a portal that does not answers an
    /// unknown-method error.
    bool hasRegistry = true;
    /// The application ids Registry.Register was called with, in order.
    QStringList registered;
    /// The members it was called with across Registry and GlobalShortcuts, in order.
    QStringList calls;
};

/// The object a FakeGlobalShortcutsPortal exports at the portal path and below it.
class FakeGlobalShortcutsObject final: public QDBusVirtualObject
{
  public:
    /// @param state What it answers and records; it must outlive this object.
    explicit FakeGlobalShortcutsObject(FakeShortcutsState& state): _state { state } {}

    /// @return No introspection data: nothing here introspects the fake.
    [[nodiscard]] QString introspect(QString const& /*path*/) const override { return {}; }

    /// Answers the three methods; any other message is not handled.
    bool handleMessage(QDBusMessage const& message, QDBusConnection const& connection) override
    {
        auto const interface = message.interface();
        if (interface == QLatin1String("org.freedesktop.portal.Session")
            && message.member() == QLatin1String("Close"))
        {
            _state.closed.append(message.path());
            connection.send(message.createReply());
            return true;
        }
        if (interface == QLatin1String("org.freedesktop.host.portal.Registry")
            && message.member() == QLatin1String("Register") && _state.hasRegistry)
        {
            _state.calls.append(message.member());
            _state.registered.append(message.arguments().value(0).toString());
            connection.send(message.createReply());
            return true;
        }
        if (interface != QLatin1String("org.freedesktop.portal.GlobalShortcuts"))
            return false;
        _state.calls.append(message.member());
        if (message.member() == QLatin1String("CreateSession"))
        {
            auto const options = qdbus_cast<QVariantMap>(message.arguments().value(0));
            auto const session = QStringLiteral("%1/session/%2/%3")
                                     .arg(QString(platform::PortalPath),
                                          senderOf(message),
                                          options.value(QStringLiteral("session_handle_token")).toString());
            _state.sessions.append(session);
            respond(message,
                    connection,
                    options,
                    _state.createResponse,
                    QVariantMap { { QStringLiteral("session_handle"), session } });
            return true;
        }
        if (message.member() == QLatin1String("BindShortcuts"))
        {
            auto const shortcuts = qdbus_cast<QList<platform::PortalShortcut>>(message.arguments().value(1));
            auto const options = qdbus_cast<QVariantMap>(message.arguments().value(3));
            _state.bound.append(shortcuts);
            respond(message, connection, options, _state.bindResponse, {});
            return true;
        }
        return false;
    }

  private:
    /// @return The caller's bus name as a request path spells it.
    [[nodiscard]] static QString senderOf(QDBusMessage const& message)
    {
        auto sender = message.service();
        sender.remove(QLatin1Char(':'));
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        return sender;
    }

    /// Replies with the request's object path, then emits the Response on it.
    void respond(QDBusMessage const& message,
                 QDBusConnection const& connection,
                 QVariantMap const& options,
                 uint code,
                 QVariantMap const& results)
    {
        auto const requestPath = QStringLiteral("%1/request/%2/%3")
                                     .arg(QString(platform::PortalPath),
                                          senderOf(message),
                                          options.value(QStringLiteral("handle_token")).toString());
        connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(requestPath))));
        // After the reply is on the wire, as the portal does it.
        QTimer::singleShot(0, this, [connection, requestPath, code, results] {
            auto signal = QDBusMessage::createSignal(
                requestPath, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
            signal << code << results;
            connection.send(signal);
        });
    }

    FakeShortcutsState& _state;
};

/// A GlobalShortcuts portal registered as org.freedesktop.portal.Desktop on its own connection to the
/// session bus, for as long as it lives. Its own connection, so the channel's calls cross the bus.
class FakeGlobalShortcutsPortal
{
  public:
    /// The name of the connection the fake portal is served on.
    static constexpr auto ConnectionName = QLatin1StringView("contour-fake-shortcuts-portal");

    /// Registers the portal.
    FakeGlobalShortcutsPortal():
        _bus { QDBusConnection::connectToBus(QDBusConnection::SessionBus, ConnectionName) }, _object { state }
    {
        qDBusRegisterMetaType<platform::PortalShortcut>();
        qDBusRegisterMetaType<QList<platform::PortalShortcut>>();
        _bus.registerVirtualObject(platform::PortalPath, &_object, QDBusConnection::SubPath);
        _bus.registerService(platform::PortalService);
    }

    FakeGlobalShortcutsPortal(FakeGlobalShortcutsPortal const&) = delete;
    FakeGlobalShortcutsPortal& operator=(FakeGlobalShortcutsPortal const&) = delete;
    FakeGlobalShortcutsPortal(FakeGlobalShortcutsPortal&&) = delete;
    FakeGlobalShortcutsPortal& operator=(FakeGlobalShortcutsPortal&&) = delete;

    ~FakeGlobalShortcutsPortal()
    {
        _bus.unregisterService(platform::PortalService);
        _bus.unregisterObject(platform::PortalPath, QDBusConnection::UnregisterTree);
        QDBusConnection::disconnectFromBus(ConnectionName);
    }

    /// @return Whether the bus names this fake as the portal's owner.
    [[nodiscard]] bool isRegistered() const
    {
        auto const owner = _bus.interface()->serviceOwner(platform::PortalService);
        return owner.isValid() && owner.value() == _bus.baseService();
    }

    /// Emits Activated for @p shortcutId on @p session, carrying @p activationToken when it is not empty.
    void emitActivated(QString const& session, QString const& shortcutId, QString const& activationToken)
    {
        auto signal = QDBusMessage::createSignal(platform::PortalPath,
                                                 QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
                                                 QStringLiteral("Activated"));
        auto options = QVariantMap {};
        if (!activationToken.isEmpty())
            options.insert(QStringLiteral("activation_token"), activationToken);
        signal << QVariant::fromValue(QDBusObjectPath(session)) << shortcutId << qulonglong { 1 } << options;
        _bus.send(signal);
    }

    /// What it answers and what it has been asked.
    FakeShortcutsState state {};

  private:
    QDBusConnection _bus;
    FakeGlobalShortcutsObject _object;
};

} // namespace contour::test

#endif // defined(__linux__)
