// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/DBusPortalSettingsChannel.hpp>
    #include <contour/platform/PortalCall.hpp>

    #include <QtCore/QMap>
    #include <QtCore/QStringList>
    #include <QtCore/QVariantMap>
    #include <QtDBus/QDBusArgument>
    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusError>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusMetaType>
    #include <QtDBus/QDBusPendingCallWatcher>
    #include <QtDBus/QDBusPendingReply>

    #include <algorithm>
    #include <array>
    #include <memory>
    #include <ranges>
    #include <utility>

namespace contour::platform
{

namespace
{
    /// ReadAll's reply, `a{sa{sv}}`: namespace to key to value.
    using PortalSettingsMap = QMap<QString, QVariantMap>;

    constexpr auto SettingsSource = DBusSignalSource {
        .service = PortalService,
        .path = PortalPath,
        .interface = QLatin1StringView("org.freedesktop.portal.Settings"),
    };

    /// The signature of a portal colour: three doubles.
    constexpr auto PortalColorSignature = QLatin1StringView("(ddd)");

    /// The errors that mean "no Settings portal here", as opposed to one that failed to answer.
    constexpr auto NoPortalErrors = std::array {
        QDBusError::ServiceUnknown,
        QDBusError::UnknownInterface,
        QDBusError::UnknownMethod,
        QDBusError::UnknownObject,
    };

    // A function rather than a constant because SLOT() expands to qFlagLocation(), which is neither
    // constexpr nor safe to run before main().
    [[nodiscard]] std::array<DBusSignalSubscription, 1> signalSubscriptions()
    {
        return { DBusSignalSubscription { QLatin1StringView("SettingChanged"),
                                          SLOT(onSettingChanged(QString, QString, QDBusVariant)) } };
    }

    /// @return Why @p error left us without settings.
    [[nodiscard]] PortalSettingsError portalSettingsErrorOf(QDBusError const& error)
    {
        return std::ranges::contains(NoPortalErrors, error.type()) ? PortalSettingsError::NoPortal
                                                                   : PortalSettingsError::Failed;
    }

    /// @return A portal colour from a `(ddd)` structure; nothing for any other signature.
    [[nodiscard]] PortalSettingValue portalColorOf(QDBusArgument const& argument)
    {
        if (argument.currentSignature() != PortalColorSignature)
            return std::monostate {};
        auto color = PortalColor {};
        argument.beginStructure();
        argument >> color.red >> color.green >> color.blue;
        argument.endStructure();
        return color;
    }

    /// @return Every setting of @p map, in its order.
    [[nodiscard]] PortalSettings portalSettingsOf(PortalSettingsMap const& map)
    {
        auto settings = PortalSettings {};
        for (auto const& [group, values]: map.asKeyValueRange())
            for (auto const& [key, value]: values.asKeyValueRange())
                settings.push_back(PortalSetting { .group = group.toStdString(),
                                                   .key = key.toStdString(),
                                                   .value = portalSettingValueOf(value) });
        return settings;
    }
} // namespace

PortalSettingValue portalSettingValueOf(QVariant const& value)
{
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        return portalSettingValueOf(value.value<QDBusVariant>().variant());
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return portalColorOf(value.value<QDBusArgument>());
    switch (value.metaType().id())
    {
        case QMetaType::UInt: return std::uint32_t { value.toUInt() };
        case QMetaType::Int:
            if (auto const number = value.toInt(); number >= 0)
                return static_cast<std::uint32_t>(number);
            return std::monostate {};
        case QMetaType::QString: return value.toString().toStdString();
        default: return std::monostate {};
    }
}

DBusPortalSettingsChannel::DBusPortalSettingsChannel(std::vector<std::string> namespaces,
                                                     PortalSettingChanged onChange,
                                                     DBusSignalSubscriber const& subscriber):
    _namespaces { std::move(namespaces) },
    _onChange { std::move(onChange) },
    _subscription { subscriber(SettingsSource, this, signalSubscriptions()) }
{
    qDBusRegisterMetaType<PortalSettingsMap>();
}

void DBusPortalSettingsChannel::readAll(PortalSettingsReply onReply)
{
    auto call = QDBusMessage::createMethodCall(
        SettingsSource.service, SettingsSource.path, SettingsSource.interface, QStringLiteral("ReadAll"));
    auto namespaces = QStringList {};
    for (auto const& name: _namespaces)
        namespaces.append(QString::fromStdString(name));
    call << namespaces;

    // Parented to this, so a reply after this is gone reaches nothing.
    auto* const watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, DBusCallTimeoutMilliseconds), this);
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, this, [onReply = std::move(onReply)](auto* self) {
            auto const reply = QDBusPendingReply<PortalSettingsMap>(*self);
            self->deleteLater();
            if (reply.isError())
            {
                portalLog()("Settings portal ReadAll failed: {}", reply.error().message().toStdString());
                onReply(std::unexpected(portalSettingsErrorOf(reply.error())));
                return;
            }
            onReply(portalSettingsOf(reply.value()));
        });
}

void DBusPortalSettingsChannel::onSettingChanged(QString const& group,
                                                 QString const& key,
                                                 QDBusVariant const& value)
{
    auto name = group.toStdString();
    if (!_onChange || !std::ranges::contains(_namespaces, name))
        return;
    _onChange(PortalSetting {
        .group = std::move(name), .key = key.toStdString(), .value = portalSettingValueOf(value.variant()) });
}

PortalSettingsChannelFactory dbusPortalSettingsChannels()
{
    return [](std::vector<std::string> namespaces,
              PortalSettingChanged onChange) -> std::unique_ptr<IPortalSettingsChannel> {
        return std::make_unique<DBusPortalSettingsChannel>(std::move(namespaces), std::move(onChange));
    };
}

} // namespace contour::platform

#endif // defined(__linux__)
