// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for NotificationRouter — the transport-free OSC-id ⇄ server-id bookkeeping and
// urgency policy the desktop-notification backends share. Exercises the replace-in-place, close,
// and server-event dispatch paths that the D-Bus backend would otherwise only reach with a live
// notification server.

#include <contour/platform/NotificationRouter.hpp>

#include <vtbackend/DesktopNotification.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using contour::platform::NotificationRouter;
using namespace std::chrono_literals;

TEST_CASE("NotificationRouter maps urgency onto the freedesktop byte", "[notification]")
{
    STATIC_CHECK(NotificationRouter::toFreedesktopUrgency(vtbackend::NotificationUrgency::Low) == 0);
    STATIC_CHECK(NotificationRouter::toFreedesktopUrgency(vtbackend::NotificationUrgency::Normal) == 1);
    STATIC_CHECK(NotificationRouter::toFreedesktopUrgency(vtbackend::NotificationUrgency::Critical) == 2);
}

TEST_CASE("NotificationRouter maps urgency onto the portal priority string", "[notification]")
{
    // The portal names four levels to freedesktop's three, so Critical lands on `urgent` -- the one
    // that survives Do-Not-Disturb -- and the portal's `high` has no source to come from.
    STATIC_CHECK(NotificationRouter::toPortalPriority(vtbackend::NotificationUrgency::Low) == "low");
    STATIC_CHECK(NotificationRouter::toPortalPriority(vtbackend::NotificationUrgency::Normal) == "normal");
    STATIC_CHECK(NotificationRouter::toPortalPriority(vtbackend::NotificationUrgency::Critical) == "urgent");
}

TEST_CASE("NotificationRouter resolves how long before a close is assumed", "[notification]")
{
    // A notification that stated its own lifetime is answered on that, whatever the configuration
    // says: the application already told us what it wanted.
    STATIC_CHECK(NotificationRouter::resolveCloseDelay(/*timeout=*/4200, 10000ms) == 4200ms);

    // OSC 99 `w=0` means "never auto-close", so there is nothing to assume, ever.
    STATIC_CHECK(NotificationRouter::resolveCloseDelay(/*timeout=*/0, 10000ms) == 0ms);

    // `w=-1` is "whatever the desktop does" -- the only case with nothing to go on, and so the only
    // one the configured fallback answers.
    STATIC_CHECK(NotificationRouter::resolveCloseDelay(/*timeout=*/-1, 10000ms) == 10000ms);

    // A zero fallback disables the assumption rather than making it instant.
    STATIC_CHECK(NotificationRouter::resolveCloseDelay(/*timeout=*/-1, 0ms) == 0ms);

    // ... but an explicit `w=` still wins over a disabled fallback.
    STATIC_CHECK(NotificationRouter::resolveCloseDelay(/*timeout=*/500, 0ms) == 500ms);

    // A configuration file is free to say -1. That must read as "disabled" rather than travel on to
    // become a negative timer interval, which Qt refuses to start and complains about instead.
    STATIC_CHECK(NotificationRouter::resolveCloseDelay(/*timeout=*/-1, -1ms) == 0ms);
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

TEST_CASE("NotificationRouter drops the stale reverse entry of a re-sent identifier", "[notification]")
{
    // A transport does not wait for a reply, so a second notification carrying the same identifier
    // can be sent while the first is still in flight -- and it is recorded with replacedId 0,
    // because there was no server id to name when it was sent. Were the earlier server id left
    // mapped, its close event would retire the LIVE notification: the popup still on screen could
    // then be neither closed nor replaced, and its close would be reported for an instance that is
    // already gone.
    NotificationRouter router;
    router.onSent("osc-a", 100, /*replacedId*/ 0);
    router.onSent("osc-a", 200, /*replacedId*/ 0);

    CHECK_FALSE(router.takeForServerEvent(100).has_value());
    CHECK(router.replacementFor("osc-a") == 200);

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
