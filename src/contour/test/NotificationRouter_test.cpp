// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for NotificationRouter — the transport-free OSC-id ⇄ server-id bookkeeping and
// urgency policy the desktop-notification backends share. Exercises the replace-in-place, close,
// and server-event dispatch paths that the D-Bus backend would otherwise only reach with a live
// notification server.

#include <contour/NotificationRouter.h>

#include <vtbackend/DesktopNotification.h>

#include <catch2/catch_test_macros.hpp>

using contour::NotificationRouter;

TEST_CASE("NotificationRouter maps urgency onto the freedesktop byte", "[notification]")
{
    STATIC_CHECK(NotificationRouter::toFreedesktopUrgency(vtbackend::NotificationUrgency::Low) == 0);
    STATIC_CHECK(NotificationRouter::toFreedesktopUrgency(vtbackend::NotificationUrgency::Normal) == 1);
    STATIC_CHECK(NotificationRouter::toFreedesktopUrgency(vtbackend::NotificationUrgency::Critical) == 2);
}

TEST_CASE("NotificationRouter tracks a fresh notification and resolves its server event", "[notification]")
{
    NotificationRouter router;

    // A never-seen identifier has no replacement.
    CHECK(router.replacementFor("osc-a") == 0);

    router.onSent("osc-a", /*serverId*/ 100, /*replacedId*/ 0);

    // A server close event resolves back to the OSC id and retires the mapping.
    auto const oscId = router.takeForServerEvent(100);
    REQUIRE(oscId.has_value());
    CHECK(*oscId == "osc-a");
    // The mapping is gone now: a second event for the same server id is unknown.
    CHECK_FALSE(router.takeForServerEvent(100).has_value());
}

TEST_CASE("NotificationRouter replaces a live notification in place", "[notification]")
{
    NotificationRouter router;
    router.onSent("osc-a", 100, 0);

    // Re-sending the same OSC id must replace server id 100.
    CHECK(router.replacementFor("osc-a") == 100);
    router.onSent("osc-a", 200, /*replacedId*/ 100);

    // The stale reverse mapping (100) is dropped; a server event for 100 is now unknown, but 200
    // resolves.
    CHECK_FALSE(router.takeForServerEvent(100).has_value());
    auto const oscId = router.takeForServerEvent(200);
    REQUIRE(oscId.has_value());
    CHECK(*oscId == "osc-a");
}

TEST_CASE("NotificationRouter close resolves and forgets the mapping", "[notification]")
{
    NotificationRouter router;
    router.onSent("osc-a", 100, 0);

    // Closing a live identifier yields its server id and forgets it.
    auto const serverId = router.takeForClose("osc-a");
    REQUIRE(serverId.has_value());
    CHECK(*serverId == 100);

    // Closing again (or an unknown id) yields nothing.
    CHECK_FALSE(router.takeForClose("osc-a").has_value());
    CHECK_FALSE(router.takeForClose("never-sent").has_value());
    // And a late server event for the closed id is a no-op.
    CHECK_FALSE(router.takeForServerEvent(100).has_value());
}

TEST_CASE("NotificationRouter ignores server events for foreign notifications", "[notification]")
{
    NotificationRouter router;
    router.onSent("osc-a", 100, 0);

    // A server id we never sent (another app's notification) resolves to nothing.
    CHECK_FALSE(router.takeForServerEvent(999).has_value());
    // Our own mapping is untouched.
    CHECK(router.replacementFor("osc-a") == 100);
}
