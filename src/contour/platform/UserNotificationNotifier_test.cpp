// SPDX-License-Identifier: Apache-2.0
//
// A session's notifier over the application-wide notification centre: the request ids it raises, the
// click it routes back, and what it withdraws when the session goes. Against the recording centre, so
// no operating-system service is reached.

#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/UserNotificationNotifier.hpp>
#include <contour/test/NotificationFixtures.hpp>
#include <contour/test/RecordingUserNotificationCenter.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <QtTest/QSignalSpy>

using contour::platform::UserNotificationNotifier;
using contour::test::aNotification;
using contour::test::RecordingUserNotificationCenter;
using namespace std::chrono_literals;

namespace
{

/// A notifier for @p sessionId over @p center that never assumes a close.
[[nodiscard]] std::unique_ptr<UserNotificationNotifier> notifierFor(
    RecordingUserNotificationCenter& center, int sessionId, std::chrono::milliseconds closeDelay = 0ms)
{
    return std::make_unique<UserNotificationNotifier>(
        center, contour::platform::sessionRequestPrefix(sessionId), closeDelay, nullptr);
}

/// A notifier that records what it was asked and raises nothing.
class RecordingFallback final: public contour::platform::Notifier
{
  public:
    void notify(vtbackend::DesktopNotification const& notification) override
    {
        raised.push_back(notification);
    }

    void close(std::string const& identifier) override { closed.push_back(identifier); }

    /// Plays back a click on the notification raised as @p identifier.
    void click(QString const& identifier) { emit actionInvoked(identifier); }

    std::vector<vtbackend::DesktopNotification> raised {};
    std::vector<std::string> closed {};
};

} // namespace

TEST_CASE("UserNotificationNotifier: a request id carries the session", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const notifier = notifierFor(center, 7);

    auto notification = aNotification("1");
    notification.sound = vtbackend::NotificationSound::Silent;
    notifier->notify(notification);

    REQUIRE(center.posted.size() == 1);
    CHECK(center.posted[0].requestId == "contour-session-7/1");
    CHECK(center.posted[0].threadId == "contour-session-7/");
    CHECK(center.posted[0].body == "Body");
    CHECK(center.posted[0].sound == vtbackend::NotificationSound::Silent);
}

TEST_CASE("UserNotificationNotifier: an application's notification is titled as one", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const notifier = notifierFor(center, 7);

    notifier->notify(aNotification("1"));
    auto finish = aNotification(contour::platform::finishNotificationIdentifier(7));
    finish.title = "✓ make finished";
    notifier->notify(finish);

    REQUIRE(center.posted.size() == 2);
    CHECK(center.posted[0].title == "Application Message: Title");
    CHECK(center.posted[1].title == "✓ make finished");
}

TEST_CASE("UserNotificationNotifier: two sessions' identical OSC ids never meet", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const first = notifierFor(center, 7);
    auto const second = notifierFor(center, 8);
    auto const firstClicks = QSignalSpy { first.get(), &contour::platform::Notifier::actionInvoked };
    auto const secondClicks = QSignalSpy { second.get(), &contour::platform::Notifier::actionInvoked };

    first->notify(aNotification("1"));
    second->notify(aNotification("1"));
    REQUIRE(center.posted.size() == 2);
    CHECK(center.posted[0].requestId != center.posted[1].requestId);

    REQUIRE(center.activate("contour-session-8/1"));

    CHECK(firstClicks.count() == 0);
    REQUIRE(secondClicks.count() == 1);
    CHECK(secondClicks.at(0).at(0).toString() == QStringLiteral("1"));
}

TEST_CASE("UserNotificationNotifier: session 7's prefix is not the start of session 70's",
          "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const seven = notifierFor(center, 7);
    auto const seventy = notifierFor(center, 70);
    auto const sevenClicks = QSignalSpy { seven.get(), &contour::platform::Notifier::actionInvoked };

    REQUIRE(center.activate("contour-session-70/1"));

    CHECK(sevenClicks.count() == 0);
}

TEST_CASE("UserNotificationNotifier: close withdraws the request", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const notifier = notifierFor(center, 7);

    SECTION("a posted notification is withdrawn under its request id")
    {
        notifier->notify(aNotification("1"));
        notifier->close("1");
        REQUIRE(center.withdrawn.size() == 1);
        CHECK(center.withdrawn[0] == "contour-session-7/1");
    }

    SECTION("one never posted is left alone")
    {
        notifier->close("1");
        CHECK(center.withdrawn.empty());
    }
}

TEST_CASE("UserNotificationNotifier: a destroyed notifier withdraws and unsubscribes", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    {
        auto const notifier = notifierFor(center, 7);
        notifier->notify(aNotification("1"));
        notifier->notify(aNotification("2"));
        notifier->close("2");
        REQUIRE(center.subscriberCount() == 1);
    }

    // "2" was withdrawn when it was closed; only the one still up goes with the notifier.
    REQUIRE(center.withdrawn.size() == 2);
    CHECK(center.withdrawn.back() == "contour-session-7/1");
    CHECK(center.subscriberCount() == 0);
    // A click on what the previous run left behind reaches nobody, and does not crash.
    CHECK_FALSE(center.activate("contour-session-7/1"));
}

TEST_CASE("UserNotificationNotifier: a close the application asked for is assumed, as untracked",
          "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const notifier = notifierFor(center, 7, 1ms);
    auto closes = QSignalSpy { notifier.get(), &contour::platform::Notifier::notificationClosed };

    auto asked = aNotification("1");
    asked.closeEventRequested = true;
    notifier->notify(asked);
    notifier->notify(aNotification("2")); // asked for nothing

    REQUIRE(closes.wait(2000));
    REQUIRE(closes.count() == 1);
    CHECK(closes.at(0).at(0).toString() == QStringLiteral("1"));
    CHECK(closes.at(0).at(2).value<vtbackend::CloseReport>() == vtbackend::CloseReport::Untracked);
}

TEST_CASE("UserNotificationNotifier: a withdrawn notification is not reported closed", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const notifier = notifierFor(center, 7, 1ms);
    auto closes = QSignalSpy { notifier.get(), &contour::platform::Notifier::notificationClosed };

    auto asked = aNotification("1");
    asked.closeEventRequested = true;
    notifier->notify(asked);
    notifier->close("1");

    CHECK_FALSE(closes.wait(50));
}

TEST_CASE("UserNotificationNotifier: a centre that turns unavailable hands the notice to the fallback",
          "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto fallback = std::make_unique<RecordingFallback>();
    auto* const tray = fallback.get();
    auto const notifier = std::make_unique<UserNotificationNotifier>(
        center, contour::platform::sessionRequestPrefix(7), 0ms, std::move(fallback));
    auto const clicks = QSignalSpy { notifier.get(), &contour::platform::Notifier::actionInvoked };

    SECTION("while the centre works, the fallback hears nothing")
    {
        notifier->notify(aNotification("1"));
        CHECK(center.posted.size() == 1);
        CHECK(tray->raised.empty());
    }

    SECTION("once it is unavailable, the fallback raises it and its click is the notifier's")
    {
        center.setAvailability(contour::platform::UserNotificationAvailability::Unavailable);
        notifier->notify(aNotification("1"));
        CHECK(center.posted.empty());
        REQUIRE(tray->raised.size() == 1);
        CHECK(tray->raised[0].identifier == "1");

        tray->click(QStringLiteral("1"));
        REQUIRE(clicks.count() == 1);
        CHECK(clicks.at(0).at(0).toString() == QStringLiteral("1"));
    }
}

TEST_CASE("UserNotificationNotifier: a request the centre gives up on goes to the fallback as it was sent",
          "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto fallback = std::make_unique<RecordingFallback>();
    auto* const tray = fallback.get();
    auto notifier = std::make_unique<UserNotificationNotifier>(
        center, contour::platform::sessionRequestPrefix(7), 1h, std::move(fallback));

    auto asked = aNotification("1");
    asked.closeEventRequested = true;
    notifier->notify(asked);
    notifier->notify(aNotification("2"));
    REQUIRE(center.posted.size() == 2);

    for (auto const& request: center.posted)
        REQUIRE(center.undeliver(request));

    REQUIRE(tray->raised.size() == 2);
    CHECK(tray->raised[0].identifier == "1");
    CHECK(tray->raised[0].title == "Title"); // not the centre's "Application Message: Title"
    CHECK(tray->raised[0].body == "Body");
    CHECK(tray->raised[0].closeEventRequested);
    CHECK(tray->raised[1].identifier == "2");
    CHECK_FALSE(tray->raised[1].closeEventRequested);

    // The fallback owns them now: nothing is left for the destructor to withdraw from the centre.
    notifier.reset();
    CHECK(center.withdrawn.empty());
}

TEST_CASE("UserNotificationNotifier: an assumed close keeps the notification withdrawable",
          "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto notifier = notifierFor(center, 7, 1ms);
    auto closes = QSignalSpy { notifier.get(), &contour::platform::Notifier::notificationClosed };

    auto asked = aNotification("1");
    asked.closeEventRequested = true;
    notifier->notify(asked);
    REQUIRE(closes.wait(2000));

    SECTION("a later close of it still withdraws it")
    {
        notifier->close("1");
        CHECK(center.withdrawn == std::vector<std::string> { "contour-session-7/1" });
    }

    SECTION("destroying the notifier withdraws it")
    {
        notifier.reset();
        CHECK(center.withdrawn == std::vector<std::string> { "contour-session-7/1" });
    }
}

TEST_CASE("UserNotificationNotifier: a repost under the same id arms a fresh timer", "[usernotifications]")
{
    RecordingUserNotificationCenter center;
    auto const notifier = notifierFor(center, 7, 1ms);
    auto closes = QSignalSpy { notifier.get(), &contour::platform::Notifier::notificationClosed };

    auto asked = aNotification("1");
    asked.closeEventRequested = true;
    notifier->notify(asked);
    REQUIRE(closes.wait(2000));
    notifier->notify(asked);

    REQUIRE(closes.wait(2000));
    CHECK(closes.count() == 2);
}
