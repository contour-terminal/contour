// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for FreeDesktopNotifier — the decision half of the Linux desktop-notification backend:
// what the freedesktop Notify call is built out of, and how a reply, a close and a server-side event
// move the OSC-identifier bookkeeping.
//
// The regression these exist for is issue #2051: the notifier used to introspect its D-Bus peer from
// its own constructor, synchronously, and to send every notification with a blocking call. On a
// desktop whose notification daemon is installed but not running, each of those waited out D-Bus's
// 25-second reply timeout — on the startup path, once per terminal session. So the first two cases
// here assert the absence of traffic and the absence of waiting, not just the content of a message.

#ifdef __linux__

    #include <contour/platform/DBusNotificationTransport.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>

    #include <QtCore/QCoreApplication>
    #include <QtCore/QStringList>
    #include <QtCore/QVariantMap>

    #include <catch2/catch_test_macros.hpp>

    #include <chrono>
    #include <string>
    #include <utility>
    #include <vector>

using contour::platform::FreeDesktopNotifier;
using contour::platform::NotificationTransport;
using vtbackend::CloseReport;

namespace
{

/// A NotificationTransport that sends nothing and remembers everything.
///
/// Deliberately not a mock framework: the recorded vectors ARE the assertions. It also never replies
/// on its own — @c completeNotify() is what delivers a reply — which is how a test can observe that
/// notify() returned before any reply existed.
class RecordingNotificationTransport final: public NotificationTransport
{
  public:
    struct NotifyCall
    {
        vtbackend::DesktopNotification notification;
        ServerId replacesId;
        SentHandler onSent;
    };

    void notify(vtbackend::DesktopNotification const& notification,
                ServerId replacesId,
                SentHandler onSent) override
    {
        notifyCalls.emplace_back(NotifyCall { notification, replacesId, std::move(onSent) });
    }

    void close(ServerId serverId) override { closedServerIds.push_back(serverId); }

    void subscribe(ClosedHandler onClosed, ActivatedHandler onActivated) override
    {
        ++subscribeCount;
        _onClosed = std::move(onClosed);
        _onActivated = std::move(onActivated);
    }

    /// Delivers the reply for the notify() call at @p index, as the bus would later on.
    void completeNotify(size_t index, std::optional<ServerId> serverId)
    {
        notifyCalls.at(index).onSent(serverId);
    }

    /// Drives the server-side signals the transport would otherwise receive over the bus.
    void emitClosed(ServerId serverId, uint32_t reason, CloseReport report = CloseReport::Observed)
    {
        _onClosed(serverId, reason, report);
    }
    void emitActivated(ServerId serverId) { _onActivated(serverId); }

    std::vector<NotifyCall> notifyCalls;
    std::vector<ServerId> closedServerIds;
    int subscribeCount = 0;

  private:
    ClosedHandler _onClosed;
    ActivatedHandler _onActivated;
};

/// Builds a notifier over a recorder, handing back a non-owning pointer to the recorder.
/// @return the notifier and the transport it is talking through.
[[nodiscard]] auto makeNotifier()
{
    auto transport = std::make_unique<RecordingNotificationTransport>();
    auto* const recorder = transport.get();
    auto notifier = std::make_unique<FreeDesktopNotifier>(std::move(transport));
    return std::pair { std::move(notifier), recorder };
}

/// Runs the posted work notify()/close() hand to the event loop.
///
/// That this is needed at all is the point: the notifier never speaks to its transport inline.
void pump()
{
    QCoreApplication::processEvents();
}

[[nodiscard]] vtbackend::DesktopNotification aNotification(std::string identifier)
{
    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = std::move(identifier);
    notification.title = "Title";
    notification.body = "Body";
    return notification;
}

} // namespace

TEST_CASE("FreeDesktopNotifier sends nothing while being constructed", "[contour][notification]")
{
    // The startup regression: constructing a session's notifier must cost no round-trip at all. It
    // subscribes to the server's signals — which does not wait for the bus — and nothing else.
    auto const [notifier, transport] = makeNotifier();

    CHECK(transport->notifyCalls.empty());
    CHECK(transport->closedServerIds.empty());
    CHECK(transport->subscribeCount == 1);
}

TEST_CASE("FreeDesktopNotifier does not wait for a reply", "[contour][notification]")
{
    auto const [notifier, transport] = makeNotifier();

    notifier->notify(aNotification("osc-1"));

    // Nothing has even been handed to the transport yet: the send is posted, not inline.
    CHECK(transport->notifyCalls.empty());

    pump();
    REQUIRE(transport->notifyCalls.size() == 1);

    // And the call is outstanding — notify() returned with no reply in existence, which is exactly
    // what a blocking QDBusInterface::call could not do.
    auto second = aNotification("osc-1");
    notifier->notify(second);
    pump();
    REQUIRE(transport->notifyCalls.size() == 2);
    // Still 0 == "a fresh notification": without a reply there is no server id to replace.
    CHECK(transport->notifyCalls[1].replacesId == 0);
}

TEST_CASE("buildFreedesktopNotifyArguments builds the Notify tuple", "[contour][notification]")
{
    // Checked head-on rather than through the notifier: the tuple is what the D-Bus transport puts
    // on the wire, and nothing about building it needs a bus, a daemon or an event loop.
    auto notification = aNotification("osc-1");
    notification.urgency = vtbackend::NotificationUrgency::Critical;
    notification.timeout = 4200;

    auto const arguments = contour::platform::buildFreedesktopNotifyArguments(notification, /*replacesId*/ 0);

    // Signature susssasa{sv}i, in wire order. The types matter as much as the values: a QVariant
    // built from the wrong integer type marshals as the wrong D-Bus type and the server rejects it.
    REQUIRE(arguments.size() == 8);
    CHECK(arguments[0].toString() == QStringLiteral("contour"));
    CHECK(arguments[1].typeId() == QMetaType::UInt);
    CHECK(arguments[1].value<quint32>() == 0);
    CHECK(arguments[2].toString().isEmpty());
    CHECK(arguments[3].toString() == QStringLiteral("Title"));
    CHECK(arguments[4].toString() == QStringLiteral("Body"));
    CHECK(arguments[5].toStringList()
          == QStringList { QStringLiteral("default"), QStringLiteral("Activate") });
    CHECK(arguments[6].toMap()["urgency"].value<uint8_t>() == 2);
    CHECK(arguments[7].typeId() == QMetaType::Int);
    CHECK(arguments[7].toInt() == 4200);

    SECTION("an explicit application name wins over the default")
    {
        notification.applicationName = "my-app";
        auto const named = contour::platform::buildFreedesktopNotifyArguments(notification, 0);
        CHECK(named[0].toString() == QStringLiteral("my-app"));
    }

    SECTION("the replaced id is carried as replaces_id")
    {
        auto const replacing = contour::platform::buildFreedesktopNotifyArguments(notification, 77);
        CHECK(replacing[1].value<quint32>() == 77);
    }
}

TEST_CASE("FreeDesktopNotifier hands the transport the notification, not a wire tuple",
          "[contour][notification]")
{
    // The seam is stated in the domain type, so the portal transport never has to read a shape it
    // does not speak. @see issue #2074.
    auto const [notifier, transport] = makeNotifier();

    auto notification = aNotification("osc-1");
    notification.urgency = vtbackend::NotificationUrgency::Critical;
    notifier->notify(notification);
    pump();

    REQUIRE(transport->notifyCalls.size() == 1);
    CHECK(transport->notifyCalls[0].notification.identifier == "osc-1");
    CHECK(transport->notifyCalls[0].notification.title == "Title");
    CHECK(transport->notifyCalls[0].notification.urgency == vtbackend::NotificationUrgency::Critical);
    CHECK(transport->notifyCalls[0].replacesId == 0);
}

TEST_CASE("FreeDesktopNotifier replaces in place once the reply arrives", "[contour][notification]")
{
    auto const [notifier, transport] = makeNotifier();

    notifier->notify(aNotification("osc-1"));
    pump();
    REQUIRE(transport->notifyCalls.size() == 1);

    transport->completeNotify(0, 77);

    // The same OSC identifier now updates the notification already on screen rather than stacking a
    // second one beside it.
    notifier->notify(aNotification("osc-1"));
    pump();
    REQUIRE(transport->notifyCalls.size() == 2);
    CHECK(transport->notifyCalls[1].replacesId == 77);

    SECTION("a failed send records nothing, so the next one is fresh again")
    {
        transport->completeNotify(1, std::nullopt);

        notifier->notify(aNotification("osc-1"));
        pump();
        REQUIRE(transport->notifyCalls.size() == 3);
        // Still 77: the failed call did not overwrite the live mapping.
        CHECK(transport->notifyCalls[2].replacesId == 77);
    }
}

TEST_CASE("FreeDesktopNotifier closes only what is live", "[contour][notification]")
{
    auto const [notifier, transport] = makeNotifier();

    // An identifier that was never sent resolves to no server id, so nothing goes on the wire.
    notifier->close("never-sent");
    pump();
    CHECK(transport->closedServerIds.empty());

    notifier->notify(aNotification("osc-1"));
    pump();
    transport->completeNotify(0, 55);

    notifier->close("osc-1");
    pump();
    REQUIRE(transport->closedServerIds.size() == 1);
    CHECK(transport->closedServerIds[0] == 55);

    // Closing twice is not closing someone else's notification the second time.
    notifier->close("osc-1");
    pump();
    CHECK(transport->closedServerIds.size() == 1);
}

TEST_CASE("FreeDesktopNotifier drives the real D-Bus transport without waiting",
          "[contour][notification][dbus]")
{
    // Everything above talks to a recorder, which cannot catch the bug this file exists for: that
    // was in the D-Bus adapter, and specifically in it waiting. So this one case builds the notifier
    // exactly as production does, against whatever session bus the machine running it has.
    //
    // test/e2e/notification-nonblocking.sh is what makes that bus interesting: there
    // org.freedesktop.Notifications is activatable and never answers, which is what a desktop with
    // its notification daemon disabled looks like, and what used to cost 25 seconds per session.
    auto const startedAt = std::chrono::steady_clock::now();

    auto notifier = contour::platform::makeDesktopNotifier(std::chrono::milliseconds { 10000 });
    REQUIRE(notifier != nullptr);

    // Actually sending is gated, so that a run on a developer's own desktop does not pop a
    // notification up at them and leave it there. The wedged-bus harness, where by construction
    // nothing can be displayed, sets this and so covers the send path too.
    if (qEnvironmentVariableIsSet("CONTOUR_TEST_NOTIFICATION_SEND"))
    {
        notifier->notify(aNotification("osc-real"));
        notifier->close("osc-real");
    }
    QCoreApplication::processEvents();

    // Deliberately generous: this separates "returned promptly" from "waited out a 25-second D-Bus
    // reply timeout", and is not trying to measure anything finer than that.
    CHECK(std::chrono::steady_clock::now() - startedAt < std::chrono::seconds { 5 });
}

TEST_CASE("FreeDesktopNotifier reports server-side close and activation", "[contour][notification]")
{
    auto const [notifier, transport] = makeNotifier();

    struct ClosedEvent
    {
        QString identifier;
        uint reason;
        CloseReport report;
    };
    auto closed = std::vector<ClosedEvent> {};
    auto activated = std::vector<QString> {};
    QObject::connect(notifier.get(),
                     &contour::platform::Notifier::notificationClosed,
                     [&](QString const& identifier, uint reason, CloseReport report) {
                         closed.emplace_back(ClosedEvent { identifier, reason, report });
                     });
    QObject::connect(notifier.get(),
                     &contour::platform::Notifier::actionInvoked,
                     [&](QString const& identifier) { activated.push_back(identifier); });

    notifier->notify(aNotification("osc-1"));
    pump();
    transport->completeNotify(0, 12);

    SECTION("a close event carries the OSC identifier and the reason")
    {
        transport->emitClosed(12, /*reason*/ 2);

        REQUIRE(closed.size() == 1);
        CHECK(closed[0].identifier == QStringLiteral("osc-1"));
        CHECK(closed[0].reason == 2);
        CHECK(closed[0].report == CloseReport::Observed);

        // The notification is retired, so a repeat of the same server id says nothing.
        transport->emitClosed(12, 2);
        CHECK(closed.size() == 1);
    }

    SECTION("a close the backend only assumed is forwarded as such")
    {
        // What the portal backend produces: nothing observed the close, a timer expired. The
        // provenance travels with the event, so the session can report it honestly.
        transport->emitClosed(12, /*reason*/ 1, CloseReport::Untracked);

        REQUIRE(closed.size() == 1);
        CHECK(closed[0].identifier == QStringLiteral("osc-1"));
        CHECK(closed[0].report == CloseReport::Untracked);
    }

    SECTION("an activation carries the OSC identifier")
    {
        transport->emitActivated(12);

        REQUIRE(activated.size() == 1);
        CHECK(activated[0] == QStringLiteral("osc-1"));
    }

    SECTION("another application's notification is not ours to report")
    {
        transport->emitClosed(999, 2);
        transport->emitActivated(999);

        CHECK(closed.empty());
        CHECK(activated.empty());
    }
}

#endif // defined(__linux__)
