// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for PortalNotificationTransport — the org.freedesktop.portal.Notification backend a
// sandboxed Contour must use, because Flatpak cannot reach org.freedesktop.Notifications and
// Flathub declines to grant the permission that would. @see issue #2074.
//
// The portal differs from the session-bus service in three ways that all land here: it assigns no
// notification id, it has no close signal of any kind, and its ActionInvoked is broadcast. So the
// cases below are about the id bookkeeping, the close this transport has to ASSUME rather than
// observe, and not mistaking another application's notification for one of ours.
//
// Both the portal call and the delay are injected, so none of this needs a portal to talk to — and
// running the suite never pops a real notification up at whoever is running it.

#ifdef __linux__

    #include <contour/platform/PortalNotificationTransport.hpp>
    #include <contour/test/NotificationFixtures.hpp>
    #include <contour/test/PortalFixtures.hpp>

    #include <QtCore/QMetaObject>

    #include <catch2/catch_test_macros.hpp>

    #include <chrono>
    #include <optional>
    #include <utility>
    #include <vector>

using contour::platform::CallOutcome;
using contour::platform::makePortalIdPrefix;
using contour::platform::NotificationTransport;
using contour::platform::PortalNotificationTransport;
using contour::platform::toPortalPriority;
using contour::test::aNotification;
using vtbackend::CloseReport;
using namespace std::chrono_literals;

namespace
{

/// One scheduled delayed action, with the timer the test has yet to fire.
struct ScheduledAction
{
    std::chrono::milliseconds delay;
    std::function<void()> action;
};

/// Everything the transport reached for, and nothing it was given back unasked.
///
/// The portal half comes from the shared recorder every portal-speaking class's tests use; what is
/// added here is the delay, which only this transport has. Deliberately not a mock framework: the
/// recorded vectors ARE the assertions. Nothing replies or fires on its own, which is how a test
/// observes that the transport records a notification only once the portal ACCEPTED it, and assumes
/// a close only once the delay actually elapsed.
struct Recorder: contour::test::RecordingPortalCaller
{
    std::vector<ScheduledAction> scheduled;

    [[nodiscard]] contour::platform::DelayScheduler scheduler()
    {
        return [this](QObject*, std::chrono::milliseconds delay, std::function<void()> action) {
            scheduled.emplace_back(ScheduledAction { delay, std::move(action) });
        };
    }

    /// Fires the timer at @p index, as the event loop would once the delay elapsed.
    void fireScheduled(size_t index) { scheduled.at(index).action(); }
};

/// One close event as the transport reported it.
struct ClosedEvent
{
    NotificationTransport::ServerId serverId;
    uint32_t reason;
    CloseReport report;
};

/// A transport over a recorder, already subscribed, with the events it reports collected.
struct Fixture
{
    Recorder recorder;
    std::vector<ClosedEvent> closed;
    std::vector<NotificationTransport::ServerId> activated;
    std::vector<std::optional<NotificationTransport::ServerId>> sent;
    PortalNotificationTransport transport;

    explicit Fixture(std::chrono::milliseconds closeDelay = 10000ms):
        transport { closeDelay, "contour-test", recorder.scheduler(), recorder.caller() }
    {
        transport.subscribe(
            [this](auto serverId, auto reason, auto report) {
                closed.emplace_back(ClosedEvent { serverId, reason, report });
            },
            [this](auto serverId) { activated.push_back(serverId); });
    }

    /// Sends @p notification, leaving the portal's reply outstanding.
    void send(vtbackend::DesktopNotification const& notification,
              NotificationTransport::ServerId replacesId = 0)
    {
        transport.notify(notification, replacesId, [this](auto serverId) { sent.push_back(serverId); });
    }

    /// Sends @p notification and lets the portal accept it, returning the id it was recorded under.
    [[nodiscard]] NotificationTransport::ServerId sendAccepted(
        vtbackend::DesktopNotification const& notification, NotificationTransport::ServerId replacesId = 0)
    {
        auto const index = recorder.calls.size();
        send(notification, replacesId);
        recorder.completeCall(index, CallOutcome::Accepted);
        return sent.back().value();
    }
};

/// Plays back the portal's ActionInvoked signal, the way the bus would deliver it.
///
/// Through the meta-object rather than as a direct call, because the slot is private -- which is how
/// the sibling D-Bus transport keeps its own, and there is no reason for a test to widen the public
/// surface of one of them.
void fireActionInvoked(PortalNotificationTransport& transport, QString const& identifier)
{
    // Checked, because a name resolved at run time can stop resolving: were the slot renamed, every
    // "and nothing was reported" case below would keep passing while reporting nothing at all.
    auto const invoked = QMetaObject::invokeMethod(&transport,
                                                   "onActionInvoked",
                                                   Q_ARG(QString, identifier),
                                                   Q_ARG(QString, QStringLiteral("default")),
                                                   Q_ARG(QVariantList, QVariantList {}));
    REQUIRE(invoked);
}

} // namespace

TEST_CASE("makePortalIdPrefix hands out a fresh namespace every time", "[contour][notification]")
{
    // A transport is built per terminal session, and two panes are free to pick the same OSC 99
    // identifier. Were the namespace merely per-process, their portal ids would be equal: the portal
    // would replace one pane's popup with the other's, and because ActionInvoked is broadcast, a
    // click on either would be reported back by BOTH sessions.
    auto const first = makePortalIdPrefix();
    auto const second = makePortalIdPrefix();

    CHECK(first != second);
    CHECK(first.starts_with("contour-"));
}
TEST_CASE("buildPortalNotificationOptions maps urgency onto the portal priority string",
          "[contour][notification]")
{
    // The portal names four levels to freedesktop's three, so Critical lands on `urgent` -- the one
    // that survives Do-Not-Disturb -- and the portal's `high` has no source to come from.
    STATIC_CHECK(toPortalPriority(vtbackend::NotificationUrgency::Low) == "low");
    STATIC_CHECK(toPortalPriority(vtbackend::NotificationUrgency::Normal) == "normal");
    STATIC_CHECK(toPortalPriority(vtbackend::NotificationUrgency::Critical) == "urgent");
}

TEST_CASE("buildPortalNotificationOptions builds the AddNotification dictionary", "[contour][notification]")
{
    auto notification = aNotification("osc-1");
    notification.urgency = vtbackend::NotificationUrgency::Critical;

    auto const options = contour::platform::buildPortalNotificationOptions(notification);

    CHECK(options["title"].toString() == QStringLiteral("Title"));
    CHECK(options["body"].toString() == QStringLiteral("Body"));
    CHECK(options["priority"].toString() == QStringLiteral("urgent"));

    // The portal has no counterpart for freedesktop's app_name, app_icon or expire_timeout: it
    // takes the application from the sandbox's own app id and never expires on our instruction.
    // Sending them anyway would be sending keys the interface does not define.
    CHECK_FALSE(options.contains("app_name"));
    CHECK_FALSE(options.contains("expire_timeout"));
}

TEST_CASE("buildPortalNotificationOptions does not export the default action", "[contour][notification]")
{
    // The absence of an "app." prefix is the whole mechanism, and reads like an oversight: the
    // portal D-Bus-activates an "app."-prefixed action through org.freedesktop.Application and
    // delivers nothing back to us, while any other name takes the branch that emits ActionInvoked.
    // Prefix this and OSC 99's a=report and a=focus both go silent.
    auto const options = contour::platform::buildPortalNotificationOptions(aNotification("osc-1"));

    auto const defaultAction = options["default-action"].toString();
    REQUIRE_FALSE(defaultAction.isEmpty());
    CHECK_FALSE(defaultAction.startsWith(QStringLiteral("app.")));
}

TEST_CASE("PortalNotificationTransport namespaces its portal ids", "[contour][notification]")
{
    // The portal's ActionInvoked is broadcast and drops the app id it matched on, so a bare "1"
    // would be indistinguishable from another sandboxed application's notification "1".
    auto fixture = Fixture {};

    auto const portalId = fixture.transport.portalIdFor("osc-1");
    CHECK(portalId == "contour-test/osc-1");
}

TEST_CASE("PortalNotificationTransport sends AddNotification with the id and options",
          "[contour][notification]")
{
    auto fixture = Fixture {};

    fixture.send(aNotification("osc-1"));

    REQUIRE(fixture.recorder.calls.size() == 1);
    CHECK(fixture.recorder.calls[0].method == QStringLiteral("AddNotification"));
    REQUIRE(fixture.recorder.calls[0].arguments.size() == 2);
    CHECK(fixture.recorder.calls[0].arguments[0].toString() == QStringLiteral("contour-test/osc-1"));
    CHECK(fixture.recorder.calls[0].arguments[1].toMap()["title"].toString() == QStringLiteral("Title"));

    // The call is outstanding: notify() returned with no reply in existence, and nothing was
    // assumed about a notification the portal has not yet accepted.
    CHECK(fixture.sent.empty());
    CHECK(fixture.recorder.scheduled.empty());
}

TEST_CASE("PortalNotificationTransport records nothing when the portal refuses", "[contour][notification]")
{
    auto fixture = Fixture {};

    fixture.send(aNotification("osc-1"));
    fixture.recorder.completeCall(0, CallOutcome::Failed);

    REQUIRE(fixture.sent.size() == 1);
    CHECK_FALSE(fixture.sent[0].has_value());

    // Nothing to close and nothing to assume: a notification that was never shown must not be
    // reported as having closed later.
    CHECK(fixture.recorder.scheduled.empty());
    fixture.transport.close(1);
    CHECK(fixture.recorder.calls.size() == 1);
}

TEST_CASE("PortalNotificationTransport assumes a close once the delay elapses", "[contour][notification]")
{
    auto fixture = Fixture { 10000ms };

    auto const serverId = fixture.sendAccepted(aNotification("osc-1"));

    REQUIRE(fixture.recorder.scheduled.size() == 1);
    CHECK(fixture.recorder.scheduled[0].delay == 10000ms);
    CHECK(fixture.closed.empty()); // Nothing until the timer actually fires.

    fixture.recorder.fireScheduled(0);

    REQUIRE(fixture.closed.size() == 1);
    CHECK(fixture.closed[0].serverId == serverId);
    // Marked for what it is. The portal has no close signal, so this was never observed -- and
    // reporting it as though it had been is what would make the OSC 99 close report a lie.
    CHECK(fixture.closed[0].report == CloseReport::Untracked);
}

TEST_CASE("PortalNotificationTransport honours the notification's own lifetime", "[contour][notification]")
{
    SECTION("an explicit w= wins over the configured delay")
    {
        auto fixture = Fixture { 10000ms };
        auto notification = aNotification("osc-1");
        notification.timeout = 4200;

        (void) fixture.sendAccepted(notification);

        REQUIRE(fixture.recorder.scheduled.size() == 1);
        CHECK(fixture.recorder.scheduled[0].delay == 4200ms);
    }

    SECTION("w=0 means never auto-close, so nothing is ever assumed")
    {
        auto fixture = Fixture { 10000ms };
        auto notification = aNotification("osc-1");
        notification.timeout = 0;

        (void) fixture.sendAccepted(notification);

        CHECK(fixture.recorder.scheduled.empty());
    }

    SECTION("a zero configured delay disables the assumption entirely")
    {
        auto fixture = Fixture { 0ms };

        (void) fixture.sendAccepted(aNotification("osc-1"));

        CHECK(fixture.recorder.scheduled.empty());
    }
}

TEST_CASE("PortalNotificationTransport reuses the portal id to replace in place", "[contour][notification]")
{
    auto fixture = Fixture {};

    auto const first = fixture.sendAccepted(aNotification("osc-1"));
    auto const second = fixture.sendAccepted(aNotification("osc-1"), /*replacesId*/ first);

    // Same portal id both times: the portal's own rule is that reusing an id updates the
    // notification already showing, so replacement needs no replaces_id equivalent.
    REQUIRE(fixture.recorder.calls.size() == 2);
    CHECK(fixture.recorder.calls[1].arguments[0].toString()
          == fixture.recorder.calls[0].arguments[0].toString());

    // The superseded id is forgotten, so closing it is not closing the live notification.
    fixture.transport.close(first);
    CHECK(fixture.recorder.calls.size() == 2);

    fixture.transport.close(second);
    REQUIRE(fixture.recorder.calls.size() == 3);
    CHECK(fixture.recorder.calls[2].method == QStringLiteral("RemoveNotification"));
}

TEST_CASE("PortalNotificationTransport survives a second send before the first was answered",
          "[contour][notification]")
{
    // notify() returns without waiting, so a second notification carrying the same OSC identifier
    // can be issued while the first AddNotification is still outstanding -- and it arrives with
    // replacesId 0, because nothing upstream has a server id to name yet. Both land on the SAME
    // portal id all the same, so the id map must not be left believing two server ids are live
    // under it: the stale one's timer would otherwise retire the live one, dropping its activation
    // on the floor and reporting a close for a popup still on screen.
    auto fixture = Fixture {};

    fixture.send(aNotification("osc-1"));
    fixture.send(aNotification("osc-1"));

    fixture.recorder.completeCall(0, CallOutcome::Accepted);
    fixture.recorder.completeCall(1, CallOutcome::Accepted);

    REQUIRE(fixture.sent.size() == 2);
    auto const live = fixture.sent[1].value();

    // The superseded notification says nothing when its timer comes up.
    REQUIRE(fixture.recorder.scheduled.size() == 2);
    fixture.recorder.fireScheduled(0);
    CHECK(fixture.closed.empty());

    // ... and the live one is still reachable, in both directions.
    fireActionInvoked(fixture.transport, QStringLiteral("contour-test/osc-1"));
    REQUIRE(fixture.activated.size() == 1);
    CHECK(fixture.activated[0] == live);
}

TEST_CASE("PortalNotificationTransport withdraws only what is live", "[contour][notification]")
{
    auto fixture = Fixture {};

    // An id that was never sent produces no traffic at all.
    fixture.transport.close(42);
    CHECK(fixture.recorder.calls.empty());

    auto const serverId = fixture.sendAccepted(aNotification("osc-1"));

    fixture.transport.close(serverId);
    REQUIRE(fixture.recorder.calls.size() == 2);
    CHECK(fixture.recorder.calls[1].method == QStringLiteral("RemoveNotification"));
    CHECK(fixture.recorder.calls[1].arguments[0].toString() == QStringLiteral("contour-test/osc-1"));

    // Closing twice is not closing someone else's notification the second time.
    fixture.transport.close(serverId);
    CHECK(fixture.recorder.calls.size() == 2);

    SECTION("and a withdrawn notification is never assumed to have closed")
    {
        // The timer is not cancelled; it finds the notification gone and says nothing. Reporting a
        // close here would answer a withdrawal the application itself asked for.
        REQUIRE(fixture.recorder.scheduled.size() == 1);
        fixture.recorder.fireScheduled(0);

        CHECK(fixture.closed.empty());
    }
}

TEST_CASE("PortalNotificationTransport reports an activation and retires it", "[contour][notification]")
{
    auto fixture = Fixture {};

    auto const serverId = fixture.sendAccepted(aNotification("osc-1"));

    fireActionInvoked(fixture.transport, QStringLiteral("contour-test/osc-1"));

    REQUIRE(fixture.activated.size() == 1);
    CHECK(fixture.activated[0] == serverId);

    SECTION("an activated notification is not then also assumed to have closed")
    {
        REQUIRE(fixture.recorder.scheduled.size() == 1);
        fixture.recorder.fireScheduled(0);

        CHECK(fixture.closed.empty());
    }

    SECTION("and the same activation twice is still one activation")
    {
        fireActionInvoked(fixture.transport, QStringLiteral("contour-test/osc-1"));

        CHECK(fixture.activated.size() == 1);
    }
}

TEST_CASE("PortalNotificationTransport ignores another application's notification", "[contour][notification]")
{
    // ActionInvoked is broadcast to every subscriber and carries no app id, so foreign ids arrive
    // constantly. The prefix is what tells them apart.
    auto fixture = Fixture {};

    (void) fixture.sendAccepted(aNotification("osc-1"));

    fireActionInvoked(fixture.transport, QStringLiteral("some-other-app/1"));
    fireActionInvoked(fixture.transport, QStringLiteral("osc-1"));

    CHECK(fixture.activated.empty());
}

TEST_CASE("PortalNotificationTransport drives the real portal without waiting",
          "[contour][notification][dbus]")
{
    // Built exactly as production does, against whatever session bus is there. @see
    // contour::test::checkReturnsWithoutWaiting for what this is guarding and why the bound is what
    // it is.
    auto transport = PortalNotificationTransport { 10000ms,
                                                   "contour-test",
                                                   contour::platform::qtDelayScheduler(),
                                                   contour::platform::qtPortalCaller(
                                                       contour::platform::NotificationPortalInterface) };
    transport.subscribe([](auto, auto, auto) {}, [](auto) {});

    contour::test::checkReturnsWithoutWaiting([&] {
        transport.notify(aNotification("osc-real"), 0, [](auto) {});

        // close() short-circuits on an id it holds no mapping for, and on a bus that never answers
        // no mapping is ever recorded -- so the withdraw path is put on the wire through the caller
        // itself. Sending it, not skipping it, is what this case is here to time.
        contour::platform::qtPortalCaller(contour::platform::NotificationPortalInterface)(
            &transport,
            QLatin1StringView("RemoveNotification"),
            { QStringLiteral("contour-test/osc-real") },
            {});
    });
}

#endif // defined(__linux__)
