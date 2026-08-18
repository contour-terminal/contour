// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the notification backend selection — which desktop-notification service a given
// sandbox state can actually reach. Pure and constexpr, so the choice is pinned without needing a
// Flatpak sandbox to run inside of. @see issue #2074.

#include <contour/platform/NotificationTransport.hpp>
#include <contour/platform/Notifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using contour::platform::makeNotificationTransport;
using contour::platform::makePortalIdPrefix;
using contour::platform::NotificationBackend;
using contour::platform::SandboxState;
using contour::platform::selectNotificationBackend;

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

    CHECK(makeNotificationTransport(NotificationBackend::FreeDesktop, 10000ms) != nullptr);
    CHECK(makeNotificationTransport(NotificationBackend::Portal, 10000ms) != nullptr);
}

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
