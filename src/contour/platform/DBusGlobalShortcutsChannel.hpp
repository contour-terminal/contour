// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/DBusSignalSubscription.hpp>
    #include <contour/platform/GlobalShortcutsChannel.hpp>

    #include <vtpty/SandboxInfo.hpp>

    #include <QtCore/QByteArray>
    #include <QtCore/QObject>
    #include <QtCore/QString>
    #include <QtCore/QVariantMap>
    #include <QtCore/QtGlobal>
    #include <QtDBus/QDBusArgument>
    #include <QtDBus/QDBusObjectPath>

    #include <cstdint>
    #include <functional>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <vector>

namespace contour::platform
{

/// The application id the portal knows this application by outside a sandbox: the desktop file's name.
inline constexpr auto PortalApplicationId = std::string_view("org.contourterminal.Contour");

/// Who registers this process's application id with `org.freedesktop.host.portal.Registry`.
enum class HostPortalRegistrar : uint8_t
{
    Application, ///< We do, ahead of the first GlobalShortcuts request.
    Qt,          ///< Qt does, from QGuiApplication::desktopFileName(); a second Register on the same bus
                 ///< connection fails ("Connection already associated with an application ID").
};

/// The registrar of this build's Qt: Qt registers by itself from 6.11 on (QtGui carries "Successfully
/// registered with host portal as"); older Qt leaves it to the application.
inline constexpr auto QtHostPortalRegistrar =
    QT_VERSION >= QT_VERSION_CHECK(6, 11, 0) ? HostPortalRegistrar::Qt : HostPortalRegistrar::Application;

/// The id to register with `org.freedesktop.host.portal.Registry` before the first portal request.
///
/// A sandbox hands the portal its application id itself; an application outside one is only an anonymous bus
/// name to the portal, which then cannot tell the GlobalShortcuts of one program from another's, unless the
/// program says who it is. Qt does that from 6.11 on, and a second registration fails.
/// @param sandbox   Where this process runs.
/// @param registrar Who registers the id; defaults to this build's Qt (QtHostPortalRegistrar).
/// @return The id to register, or nullopt where the sandbox or Qt already supplies one.
[[nodiscard]] constexpr std::optional<std::string_view> registryApplicationIdFor(
    vtpty::SandboxState sandbox, HostPortalRegistrar registrar = QtHostPortalRegistrar) noexcept
{
    return sandbox == vtpty::SandboxState::Host && registrar == HostPortalRegistrar::Application
               ? std::optional { PortalApplicationId }
               : std::nullopt;
}

/// One entry of BindShortcuts' `a(sa{sv})`: the shortcut's id and its properties.
struct PortalShortcut
{
    QString id;             ///< The shortcut's id.
    QVariantMap properties; ///< Its `description` and `preferred_trigger`.
};

/// Marshals @p shortcut as the structure `(sa{sv})`.
QDBusArgument& operator<<(QDBusArgument& argument, PortalShortcut const& shortcut);

/// Demarshals @p shortcut from the structure `(sa{sv})`.
void operator>>(QDBusArgument const& argument, PortalShortcut& shortcut);

/// The pending answer to one portal request: waits for the Response signal on the request's object path,
/// reports it once and deletes itself.
///
/// Subscribed before the method call is sent, because the portal may answer before the call's own reply
/// arrives; the path is derived from the caller's bus name and the handle token, as the portal spells it.
class PortalRequestWatcher final: public QObject
{
    Q_OBJECT

  public:
    /// Receives the response code and the results `a{sv}`.
    using Answer = std::function<void(PortalResponse, QVariantMap const&)>;

    /// @param requestPath The request's object path.
    /// @param subscriber  Installs the Response subscription.
    /// @param onAnswer    Called at most once, on the GUI thread.
    /// @param parent      Owns this watcher; a watcher destroyed with its parent never answers.
    PortalRequestWatcher(QString const& requestPath,
                         DBusSignalSubscriber const& subscriber,
                         Answer onAnswer,
                         QObject* parent);

    /// Answers Other, for a method call that failed and so will never be followed by a Response.
    void fail();

  private slots:
    /// Handles org.freedesktop.portal.Request.Response. Declares every parameter the signal carries.
    void onResponse(uint response, QVariantMap const& results);

  private:
    void finish(PortalResponse response, QVariantMap const& results);

    /// Whether the answer was given already.
    enum class State : uint8_t
    {
        Waiting = 0,
        Answered,
    };

    QByteArray _path; ///< The request path; the subscription's source views it, so it is declared first.
    Answer _onAnswer;
    DBusSignalSubscriptionGuard _subscription;
    State _state = State::Waiting;
};

/// Speaks org.freedesktop.portal.GlobalShortcuts over the session bus, asynchronously.
///
/// As the other portal channels, deliberately not a QDBusInterface: every call is an asyncCall. Nothing
/// touches the bus before the first createSession(), so building one before the QGuiApplication exists is
/// safe.
class DBusGlobalShortcutsChannel final: public QObject, public IGlobalShortcutsChannel
{
    Q_OBJECT

  public:
    /// @param subscriber    Installs the Response and Activated subscriptions.
    /// @param registryAppId The id to register with the portal's Registry before the first request, or
    ///                      nullopt to register none (a sandbox supplies its own).
    explicit DBusGlobalShortcutsChannel(DBusSignalSubscriber subscriber = sessionBusSubscriber(),
                                        std::optional<std::string> registryAppId = std::nullopt);

    /// Sends CreateSession and returns without waiting; subscribes to Activated and, when it was given an
    /// id, registers it with the portal's Registry on the first call. A portal without a Registry is
    /// carried on with.
    /// @pre A QCoreApplication exists.
    void createSession(SessionCreated onCreated) override;

    /// Sends BindShortcuts for @p sessionHandle and returns without waiting.
    void bindShortcuts(std::string const& sessionHandle,
                       std::vector<ShortcutBinding> const& shortcuts,
                       ShortcutsBound onBound) override;

    /// Sends Session.Close for @p sessionHandle and returns without waiting.
    void closeSession(std::string const& sessionHandle) override;

    /// @copydoc IGlobalShortcutsChannel::onActivated
    void onActivated(ShortcutActivated onActivated) override;

  private slots:
    /// Handles the Activated D-Bus signal. Declares every parameter the signal carries.
    void onShortcutActivated(QDBusObjectPath const& session,
                             QString const& shortcutId,
                             qulonglong timestamp,
                             QVariantMap const& options);

  private:
    /// Registers the application id with the portal's Registry, when there is one to register.
    void registerApplicationId();

    /// Sends @p method with @p arguments and the handle token, and answers through @p onAnswer.
    void request(QLatin1StringView method,
                 QVariantList arguments,
                 QVariantMap options,
                 PortalRequestWatcher::Answer onAnswer);

    DBusSignalSubscriber _subscriber;
    std::optional<std::string> _registryAppId;
    /// The Activated subscription, installed by the first createSession() so that building the channel
    /// does not open the bus.
    std::optional<DBusSignalSubscriptionGuard> _activations;
    ShortcutActivated _onActivated;
    std::vector<QString> _sessions;
    std::uint64_t _requestCount = 0;
};

} // namespace contour::platform

#endif // defined(__linux__)
