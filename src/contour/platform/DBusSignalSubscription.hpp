// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <QtCore/QLatin1StringView>
    #include <QtCore/QObject>
    #include <QtDBus/QDBusConnection>

    #include <span>

namespace contour::platform
{

/// One signal a D-Bus client subscribes to, and the slot it is delivered into.
struct DBusSignalSubscription
{
    /// The signal's name on the interface.
    QLatin1StringView name;

    /// The receiving slot, as SLOT() expands it.
    ///
    /// Note that the slot must declare EVERY parameter the signal carries. QtDBus matches an
    /// incoming signal to a subscription by comparing the message's signature against the one it
    /// reconstructs from the slot's parameters, so a slot taking fewer arguments does not fail
    /// loudly -- it is simply never called.
    char const* slot = nullptr;
};

/// The one service, path and interface a set of subscriptions is addressed with.
struct DBusSignalSource
{
    QLatin1StringView service;
    QLatin1StringView path;
    QLatin1StringView interface;
};

/// Registers a match rule on @p bus for each of @p subscriptions.
///
/// Registering does not wait for the bus: Qt hands AddMatch a null error pointer, making it
/// fire-and-forget. So this is installed unconditionally, with no check that there is anyone to
/// listen to -- the service may well start after this process did.
///
/// @param bus The bus to register on.
/// @param source Where the signals come from.
/// @param receiver The object whose slots they are delivered to.
/// @param subscriptions What to subscribe to.
inline void subscribeToDBusSignals(QDBusConnection& bus,
                                   DBusSignalSource const& source,
                                   QObject* receiver,
                                   std::span<DBusSignalSubscription const> subscriptions)
{
    for (auto const& subscription: subscriptions)
        bus.connect(
            source.service, source.path, source.interface, subscription.name, receiver, subscription.slot);
}

/// Removes the match rules subscribeToDBusSignals() registered.
///
/// Pairing the two here is the point: a match rule is removed by naming it again, argument for
/// argument, and one that does not match the rule registered fails SILENTLY -- leaving the rule in
/// place for the life of the process while the bus keeps delivering towards an object that is being
/// destroyed. The bus runs its own thread, so that is a teardown race and not merely a leak. Going
/// through one function with one @p source is what makes the two sides unable to drift apart.
///
/// @param bus The bus to remove them from.
/// @param source Where the signals come from; must equal what was subscribed with.
/// @param receiver The object they were delivered to.
/// @param subscriptions What was subscribed to.
inline void unsubscribeFromDBusSignals(QDBusConnection& bus,
                                       DBusSignalSource const& source,
                                       QObject* receiver,
                                       std::span<DBusSignalSubscription const> subscriptions)
{
    for (auto const& subscription: subscriptions)
        bus.disconnect(
            source.service, source.path, source.interface, subscription.name, receiver, subscription.slot);
}

} // namespace contour::platform

#endif // defined(__linux__)
