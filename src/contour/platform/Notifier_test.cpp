// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the notification backend selection — which desktop-notification service a given
// sandbox state can actually reach. Pure and constexpr, so the choice is pinned without needing a
// Flatpak sandbox to run inside of. @see issue #2074.

#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/NotificationTransport.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/test/NotificationFixtures.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>

using contour::platform::makeNotificationTransport;
using contour::platform::NotificationBackend;
using contour::platform::selectNotificationBackend;
using vtpty::SandboxState;

TEST_CASE("selectNotificationBackend prefers the session bus off the sandbox", "[contour][notification]")
{
    // org.freedesktop.Notifications is strictly the more capable of the two: it reports that a
    // notification was dismissed, which the portal cannot. Preferring the portal everywhere would
    // trade a working OSC 99 `c=1` close report away for uniformity.
    STATIC_CHECK(selectNotificationBackend(SandboxState::Host) == NotificationBackend::FreeDesktop);
}

TEST_CASE("selectNotificationBackend uses the portal inside a sandbox", "[contour][notification]")
{
    // Inside Flatpak the session-bus name is unreachable without --talk-name, which Flathub
    // declines to grant; the portal needs no static permission at all.
    STATIC_CHECK(selectNotificationBackend(SandboxState::Flatpak) == NotificationBackend::Portal);
}

TEST_CASE("makeNotificationTransport builds every backend it names", "[contour][notification]")
{
    // Reachable here without the sandbox that would otherwise be the only way to select the portal,
    // so the enumerator is a tested row rather than a branch nobody off Flatpak ever compiles into
    // existence. Nothing is sent: constructing a transport costs no round-trip, which is the
    // property the whole seam exists to protect (issue #2051).
    using namespace std::chrono_literals;

#ifdef __linux__
    // The D-Bus transport takes hold of the session bus as it is built: the wedged one
    // test/e2e/notification-nonblocking.sh stands up, never the machine's own.
    if (!contour::test::onPrivateSessionBus())
        SKIP("builds the D-Bus transport, which reaches the session bus: runs under "
             "test/e2e/notification-nonblocking.sh");
#endif

    CHECK(makeNotificationTransport(NotificationBackend::FreeDesktop, 10000ms) != nullptr);
    CHECK(makeNotificationTransport(NotificationBackend::Portal, 10000ms) != nullptr);
}

TEST_CASE("TrayNotifier shows a notification as a tray message, and reports nothing back",
          "[contour][notification]")
{
    auto shown = std::vector<std::pair<QString, QString>> {};
    auto clicks = std::vector<std::function<void()>> {};
    auto tray = contour::platform::TrayNotifier {
        [&](QString const& title, QString const& body, std::function<void()> onClicked) {
            shown.emplace_back(title, body);
            clicks.push_back(std::move(onClicked));
        }
    };
    QSignalSpy const closed(&tray, &contour::platform::Notifier::notificationClosed);
    QSignalSpy const activated(&tray, &contour::platform::Notifier::actionInvoked);

    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = contour::platform::finishNotificationIdentifier(7);
    notification.title = "make finished";
    notification.body = "took 3s";
    tray.notify(notification);
    tray.close(notification.identifier);

    REQUIRE(shown.size() == 1);
    CHECK(shown[0].first == QStringLiteral("make finished"));
    CHECK(shown[0].second == QStringLiteral("took 3s"));
    // A tray message cannot be withdrawn, and says nothing until it is clicked.
    CHECK(closed.isEmpty());
    CHECK(activated.isEmpty());

    // A click is the notification's activation, as a desktop's would be.
    REQUIRE(clicks.size() == 1);
    clicks[0]();
    REQUIRE(activated.count() == 1);
    CHECK(activated.at(0).at(0).toString().toStdString() == notification.identifier);
    CHECK(closed.isEmpty());
}

TEST_CASE("TrayNotifier titles an application's notification as one", "[contour][notification]")
{
    // A tray message shows no sender, so an application's would otherwise pass for one of Contour's own.
    auto shown = std::vector<std::pair<QString, QString>> {};
    auto tray = contour::platform::TrayNotifier {
        [&](QString const& title, QString const& body, std::function<void()> const& /*onClicked*/) {
            shown.emplace_back(title, body);
        }
    };

    // OSC 777 and OSC 9 name no identifier, and an OSC 99 one names its own.
    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = GENERATE(std::string {}, std::string { "build" });
    notification.title = "make finished";
    notification.body = "took 3s";
    tray.notify(notification);

    REQUIRE(shown.size() == 1);
    CHECK(shown[0].first == QStringLiteral("Application Message: make finished"));
    CHECK(shown[0].second == QStringLiteral("took 3s"));
}
