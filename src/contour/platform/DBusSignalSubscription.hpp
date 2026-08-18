// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <QtCore/QLatin1StringView>
    #include <QtCore/QObject>
    #include <QtDBus/QDBusConnection>

    #include <span>
    #include <utility>
    #include <vector>

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

/// How long a D-Bus method call to a notification service may stay outstanding, in milliseconds.
///
/// Deliberately below Qt's 25-second default: losing a reply costs only the replace-in-place
/// bookkeeping for that one notification, and several of these calls do not read their reply at
/// all. A desktop that is not answering must not be able to hold anything of ours open. @see
/// issue #2051, and the wedged-bus harness at test/e2e/notification-nonblocking.sh that holds
/// both transports to it.
constexpr auto DBusCallTimeoutMilliseconds = 5000;

/// The one service, path and interface a set of subscriptions is addressed with.
struct DBusSignalSource
{
    QLatin1StringView service;
    QLatin1StringView path;
    QLatin1StringView interface;
};

/// Owns a set of installed match rules and removes them when it dies.
///
/// The pairing is the whole point. A match rule is removed by naming it again, argument for
/// argument, and one that does not match the rule registered fails SILENTLY -- leaving the rule in
/// place for the life of the process while the bus, which runs its own thread, keeps delivering
/// towards an object being destroyed. That is a teardown race, not merely a leak.
///
/// Holding the arguments rather than asking each client to remember them is what makes the two
/// sides unable to drift apart: previously every backend had to declare a non-defaulted destructor,
/// track for itself whether it had subscribed, and repeat the identical source and subscription
/// array -- three chances per backend to get a silent race wrong. A default-constructed guard owns
/// nothing and does nothing, so "not subscribed yet" needs no separate flag.
///
/// The subscriptions are COPIED rather than referenced: they name the slots to disconnect, and a
/// caller building them in a temporary (as both backends do -- SLOT() is not constexpr, so the
/// array cannot be static) would otherwise leave the guard pointing at a dead one.
class DBusSignalSubscriptionGuard
{
  public:
    DBusSignalSubscriptionGuard() = default;

    /// @param bus The bus the rules are installed on.
    /// @param source Where the signals come from.
    /// @param receiver The object they are delivered to.
    /// @param subscriptions What was subscribed to.
    DBusSignalSubscriptionGuard(QDBusConnection bus,
                                DBusSignalSource const& source,
                                QObject* receiver,
                                std::span<DBusSignalSubscription const> subscriptions):
        _bus { std::move(bus) },
        _source { source },
        _receiver { receiver },
        _subscriptions { subscriptions.begin(), subscriptions.end() }
    {
    }

    DBusSignalSubscriptionGuard(DBusSignalSubscriptionGuard const&) = delete;
    DBusSignalSubscriptionGuard& operator=(DBusSignalSubscriptionGuard const&) = delete;

    DBusSignalSubscriptionGuard(DBusSignalSubscriptionGuard&& other) noexcept { swap(other); }

    DBusSignalSubscriptionGuard& operator=(DBusSignalSubscriptionGuard&& other) noexcept
    {
        if (this != &other)
        {
            auto discarded = DBusSignalSubscriptionGuard {};
            discarded.swap(*this);
            swap(other);
        }
        return *this;
    }

    ~DBusSignalSubscriptionGuard() { unsubscribe(); }

  private:
    void swap(DBusSignalSubscriptionGuard& other) noexcept
    {
        std::swap(_bus, other._bus);
        std::swap(_source, other._source);
        std::swap(_receiver, other._receiver);
        _subscriptions.swap(other._subscriptions);
    }

    void unsubscribe() noexcept
    {
        if (_receiver == nullptr)
            return;

        for (auto const& subscription: _subscriptions)
            _bus.disconnect(_source.service,
                            _source.path,
                            _source.interface,
                            subscription.name,
                            _receiver,
                            subscription.slot);
        _receiver = nullptr;
    }

    QDBusConnection _bus = QDBusConnection { QString {} };
    DBusSignalSource _source {};

    /// Null in a guard that owns nothing -- which is what a default-constructed one is.
    QObject* _receiver = nullptr;

    /// Copied, so the guard does not depend on the caller's array outliving it.
    std::vector<DBusSignalSubscription> _subscriptions;
};

/// Registers a match rule on @p bus for each of @p subscriptions, and hands back what removes them.
///
/// Registering does not wait for the bus: Qt hands AddMatch a null error pointer, making it
/// fire-and-forget. So this is installed unconditionally, with no check that there is anyone to
/// listen to -- the service may well start after this process did.
///
/// @param bus The bus to register on.
/// @param source Where the signals come from.
/// @param receiver The object whose slots they are delivered to.
/// @param subscriptions What to subscribe to; copied into the guard.
/// @return The guard that removes them again. Discarding it unsubscribes immediately.
[[nodiscard]] inline DBusSignalSubscriptionGuard subscribeToDBusSignals(
    QDBusConnection& bus,
    DBusSignalSource const& source,
    QObject* receiver,
    std::span<DBusSignalSubscription const> subscriptions)
{
    for (auto const& subscription: subscriptions)
        bus.connect(
            source.service, source.path, source.interface, subscription.name, receiver, subscription.slot);

    return DBusSignalSubscriptionGuard { bus, source, receiver, subscriptions };
}

} // namespace contour::platform

#endif // defined(__linux__)
