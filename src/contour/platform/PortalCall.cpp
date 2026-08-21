// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/PortalCall.hpp>

    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusPendingCallWatcher>
    #include <QtDBus/QDBusPendingReply>

    #include <utility>

namespace contour::platform
{

PortalCaller qtPortalCaller(QLatin1StringView interface)
{
    // The bus is resolved per call rather than captured here, unlike DBusNotificationTransport which
    // holds one. A launcher is built by the composition root for EVERY `contour` verb, including the
    // headless ones -- and QDBusConnection::sessionBus() opens a real connection, which a
    // `contour daemon` that never opens a URL has no business holding. It cost the daemon its
    // orderly shutdown: with the connection open, SIGTERM stopped unlinking the sockets.
    return [interface](QObject* context,
                       QLatin1StringView method,
                       QVariantList const& arguments,
                       std::function<void(CallOutcome)> onReply) {
        auto bus = QDBusConnection::sessionBus();
        auto message = QDBusMessage::createMethodCall(PortalService, PortalPath, interface, method);
        message.setArguments(arguments);

        auto pending = bus.asyncCall(message, DBusCallTimeoutMilliseconds);
        if (!onReply)
            return; // Sent and forgotten: nothing in this reply is acted on.

        // Parented to the context, so a caller destroyed with a call in flight takes its watcher
        // with it and the handler -- which reaches back into that caller -- is never run.
        auto* const watcher = new QDBusPendingCallWatcher(pending, context);
        QObject::connect(watcher,
                         &QDBusPendingCallWatcher::finished,
                         context,
                         [onReply = std::move(onReply), interface, method](auto* self) {
                             // Only whether a reply arrived is read; no portal method whose reply is
                             // watched here returns a value anything acts on.
                             auto const reply = QDBusPendingReply<>(*self);
                             self->deleteLater();

                             if (reply.isError())
                                 portalLog()("Portal call {}.{} failed: {}",
                                             asStringView(interface),
                                             asStringView(method),
                                             reply.error().message().toStdString());

                             onReply(reply.isError() ? CallOutcome::Failed : CallOutcome::Accepted);
                         });
    };
}

} // namespace contour::platform

#endif // defined(__linux__)
