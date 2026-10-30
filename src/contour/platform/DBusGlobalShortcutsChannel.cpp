// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/DBusGlobalShortcutsChannel.hpp>
    #include <contour/platform/GlobalHotkey.hpp>
    #include <contour/platform/PortalCall.hpp>
    #include <contour/platform/PortalGlobalHotkey.hpp>

    #include <QtCore/QList>
    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusMetaType>
    #include <QtDBus/QDBusPendingCallWatcher>
    #include <QtDBus/QDBusPendingReply>

    #include <algorithm>
    #include <array>
    #include <memory>
    #include <utility>

namespace contour::platform
{

namespace
{
    constexpr auto GlobalShortcutsInterface = QLatin1StringView("org.freedesktop.portal.GlobalShortcuts");
    constexpr auto RequestInterface = QLatin1StringView("org.freedesktop.portal.Request");
    constexpr auto SessionInterface = QLatin1StringView("org.freedesktop.portal.Session");
    constexpr auto RegistryInterface = QLatin1StringView("org.freedesktop.host.portal.Registry");

    /// The object path prefix every request of every client lives under.
    constexpr auto RequestPathPrefix = QLatin1StringView("/org/freedesktop/portal/desktop/request/");

    constexpr auto GlobalShortcutsSource = DBusSignalSource {
        .service = PortalService,
        .path = PortalPath,
        .interface = GlobalShortcutsInterface,
    };

    // A function rather than a constant because SLOT() is not constexpr.
    [[nodiscard]] std::array<DBusSignalSubscription, 1> responseSubscriptions()
    {
        return { DBusSignalSubscription { QLatin1StringView("Response"),
                                          SLOT(onResponse(uint, QVariantMap)) } };
    }

    [[nodiscard]] std::array<DBusSignalSubscription, 1> activatedSubscriptions()
    {
        return { DBusSignalSubscription {
            QLatin1StringView("Activated"),
            SLOT(onShortcutActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)) } };
    }

    /// @return The response code as the Response signal spells it: 0 success, 1 cancelled, 2 anything else.
    [[nodiscard]] PortalResponse portalResponseOf(uint code) noexcept
    {
        switch (code)
        {
            case 0: return PortalResponse::Success;
            case 1: return PortalResponse::Cancelled;
            default: return PortalResponse::Other;
        }
    }

    /// @return The object path the portal gives the request made with @p token by the caller @p busName.
    [[nodiscard]] QString requestPathOf(QString busName, QString const& token)
    {
        busName.remove(QLatin1Char(':'));
        busName.replace(QLatin1Char('.'), QLatin1Char('_'));
        return QString(RequestPathPrefix) + busName + QLatin1Char('/') + token;
    }
} // namespace

QDBusArgument& operator<<(QDBusArgument& argument, PortalShortcut const& shortcut)
{
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}

void operator>>(QDBusArgument const& argument, PortalShortcut& shortcut)
{
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
}

PortalRequestWatcher::PortalRequestWatcher(QString const& requestPath,
                                           DBusSignalSubscriber const& subscriber,
                                           Answer onAnswer,
                                           QObject* parent):
    QObject { parent },
    _path { requestPath.toLatin1() },
    _onAnswer { std::move(onAnswer) },
    _subscription { subscriber(DBusSignalSource { .service = PortalService,
                                                  .path = QLatin1StringView(_path),
                                                  .interface = RequestInterface },
                               this,
                               responseSubscriptions()) }
{
}

void PortalRequestWatcher::fail()
{
    finish(PortalResponse::Other, {});
}

void PortalRequestWatcher::onResponse(uint response, QVariantMap const& results)
{
    finish(portalResponseOf(response), results);
}

void PortalRequestWatcher::finish(PortalResponse response, QVariantMap const& results)
{
    if (std::exchange(_state, State::Answered) == State::Answered)
        return;
    deleteLater();
    if (_onAnswer)
        _onAnswer(response, results);
}

DBusGlobalShortcutsChannel::DBusGlobalShortcutsChannel(DBusSignalSubscriber subscriber,
                                                       std::optional<std::string> registryAppId):
    _subscriber { std::move(subscriber) }, _registryAppId { std::move(registryAppId) }
{
}

void DBusGlobalShortcutsChannel::onActivated(ShortcutActivated onActivated)
{
    _onActivated = std::move(onActivated);
}

void DBusGlobalShortcutsChannel::request(QLatin1StringView method,
                                         QVariantList arguments,
                                         QVariantMap options,
                                         PortalRequestWatcher::Answer onAnswer)
{
    auto bus = QDBusConnection::sessionBus();
    auto const token = QStringLiteral("contour_%1").arg(++_requestCount);
    options.insert(QStringLiteral("handle_token"), token);
    arguments.append(options);

    // Subscribed before the call, so a Response that overtakes the call's own reply is not missed.
    auto* const watcher = new PortalRequestWatcher(
        requestPathOf(bus.baseService(), token), _subscriber, std::move(onAnswer), this);

    auto call = QDBusMessage::createMethodCall(PortalService, PortalPath, GlobalShortcutsInterface, method);
    call.setArguments(arguments);
    auto* const pending =
        new QDBusPendingCallWatcher(bus.asyncCall(call, DBusCallTimeoutMilliseconds), watcher);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, watcher, [watcher, method](auto* self) {
        auto const reply = QDBusPendingReply<QDBusObjectPath>(*self);
        self->deleteLater();
        if (!reply.isError())
            return; // The Response signal follows, whenever the portal or the user is done.
        portalLog()(
            "GlobalShortcuts {} failed: {}", asStringView(method), reply.error().message().toStdString());
        watcher->fail();
    });
}

void DBusGlobalShortcutsChannel::registerApplicationId()
{
    if (!_registryAppId)
        return;
    // Sent ahead of CreateSession on the same connection, so the portal sees it first. No Response follows
    // it and a portal without a Registry just errors, which only costs the log line.
    auto call = QDBusMessage::createMethodCall(
        PortalService, PortalPath, RegistryInterface, QStringLiteral("Register"));
    call.setArguments({ QString::fromStdString(*_registryAppId), QVariantMap {} });
    auto* const pending = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, DBusCallTimeoutMilliseconds), this);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, this, [](auto* self) {
        auto const reply = QDBusPendingReply<>(*self);
        self->deleteLater();
        if (reply.isError())
            portalLog()("Registry.Register failed, carrying on: {}", reply.error().message().toStdString());
    });
}

void DBusGlobalShortcutsChannel::createSession(SessionCreated onCreated)
{
    if (!_activations)
    {
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<QList<PortalShortcut>>();
        _activations.emplace(_subscriber(GlobalShortcutsSource, this, activatedSubscriptions()));
        registerApplicationId();
    }

    auto const sessionToken = QStringLiteral("contour_session_%1").arg(_requestCount + 1);
    request(QLatin1StringView("CreateSession"),
            {},
            QVariantMap { { QStringLiteral("session_handle_token"), sessionToken } },
            [this, onCreated = std::move(onCreated)](PortalResponse response, QVariantMap const& results) {
                auto handle = results.value(QStringLiteral("session_handle")).toString();
                if (response == PortalResponse::Success && handle.isEmpty())
                    response = PortalResponse::Other;
                if (response == PortalResponse::Success)
                    _sessions.push_back(handle);
                if (onCreated)
                    onCreated(response, handle.toStdString());
            });
}

void DBusGlobalShortcutsChannel::bindShortcuts(std::string const& sessionHandle,
                                               std::vector<ShortcutBinding> const& shortcuts,
                                               ShortcutsBound onBound)
{
    auto list = QList<PortalShortcut> {};
    for (auto const& binding: shortcuts)
        list.append(PortalShortcut {
            .id = QString::fromStdString(binding.id),
            .properties =
                QVariantMap { { QStringLiteral("description"), QString::fromStdString(binding.description) },
                              { QStringLiteral("preferred_trigger"),
                                QString::fromStdString(binding.preferredTrigger) } } });

    request(QLatin1StringView("BindShortcuts"),
            { QVariant::fromValue(QDBusObjectPath(QString::fromStdString(sessionHandle))),
              QVariant::fromValue(list),
              QString {} },
            {},
            [onBound = std::move(onBound)](PortalResponse response, QVariantMap const& /*results*/) {
                if (onBound)
                    onBound(response);
            });
}

void DBusGlobalShortcutsChannel::closeSession(std::string const& sessionHandle)
{
    auto const handle = QString::fromStdString(sessionHandle);
    std::erase(_sessions, handle);
    auto call =
        QDBusMessage::createMethodCall(PortalService, handle, SessionInterface, QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(call, DBusCallTimeoutMilliseconds);
}

void DBusGlobalShortcutsChannel::onShortcutActivated(QDBusObjectPath const& session,
                                                     QString const& shortcutId,
                                                     qulonglong /*timestamp*/,
                                                     QVariantMap const& options)
{
    if (!_onActivated || !std::ranges::contains(_sessions, session.path()))
        return;
    _onActivated(shortcutId.toStdString(),
                 options.value(QStringLiteral("activation_token")).toString().toStdString());
}

std::unique_ptr<IGlobalHotkey> makeGlobalHotkey()
{
    auto const appId = registryApplicationIdFor(vtpty::currentSandbox().state);
    return std::make_unique<PortalGlobalHotkey>(std::make_unique<DBusGlobalShortcutsChannel>(
        sessionBusSubscriber(), appId ? std::optional<std::string> { std::string(*appId) } : std::nullopt));
}

} // namespace contour::platform

#endif // defined(__linux__)
