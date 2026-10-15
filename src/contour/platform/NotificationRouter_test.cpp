// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for NotificationRouter — the transport-free OSC-id ⇄ server-id bookkeeping and
// urgency policy the desktop-notification backends share. Exercises the replace-in-place, close,
// and server-event dispatch paths that the D-Bus backend would otherwise only reach with a live
// notification server.

#include <contour/platform/NotificationIdMap.hpp>
#include <contour/platform/NotificationRouter.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>

using contour::platform::NotificationRouter;
using ServerId = NotificationRouter::ServerId;
using namespace std::chrono_literals;

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

TEST_CASE("NotificationRouter assumes a close only for a notification that asked to hear of it",
          "[notification]")
{
    auto notification = vtbackend::DesktopNotification {};

    SECTION("no close report asked for: never assumed closed, whatever the delays say")
    {
        // A finish notice is one of these: a default notification, w=-1, no c=1. Assuming it
        // closed would retire it while it still sits in the desktop's history, unclickable and
        // impossible to withdraw.
        CHECK(NotificationRouter::assumedCloseDelay(notification, 10000ms) == 0ms);

        notification.timeout = 4200;
        CHECK(NotificationRouter::assumedCloseDelay(notification, 10000ms) == 0ms);
    }

    SECTION("a close report asked for: the delay resolves as it always did")
    {
        notification.closeEventRequested = true;
        CHECK(NotificationRouter::assumedCloseDelay(notification, 10000ms) == 10000ms);

        notification.timeout = 4200;
        CHECK(NotificationRouter::assumedCloseDelay(notification, 10000ms) == 4200ms);

        notification.timeout = 0;
        CHECK(NotificationRouter::assumedCloseDelay(notification, 10000ms) == 0ms);
    }
}

TEST_CASE("NotificationRouter tracks a fresh notification and resolves its server event", "[notification]")
{
    NotificationRouter router;

    // A never-seen identifier has no replacement.
    CHECK(router.replacementFor("osc-a") == 0);

    CHECK_FALSE(router.onSent("osc-a", /*serverId*/ 100, /*replacedId*/ 0).has_value());

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
    CHECK_FALSE(router.onSent("osc-a", 100, 0).has_value());

    // Re-sending the same OSC id must replace server id 100.
    CHECK(router.replacementFor("osc-a") == 100);
    CHECK_FALSE(router.onSent("osc-a", 200, /*replacedId*/ 100).has_value());

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
    CHECK_FALSE(router.onSent("osc-a", 100, /*replacedId*/ 0).has_value());

    // The earlier popup is still on screen, superseded but unnamed: the caller is told to withdraw
    // it, or it would stay there, orphaned.
    CHECK(router.onSent("osc-a", 200, /*replacedId*/ 0) == std::optional<ServerId> { 100 });

    CHECK_FALSE(router.takeForServerEvent(100).has_value());
    CHECK(router.replacementFor("osc-a") == 200);

    auto const oscId = router.takeForServerEvent(200);
    REQUIRE(oscId.has_value());
    CHECK(*oscId == "osc-a");
}

TEST_CASE("NotificationRouter accepts a server id reissued under a new identifier", "[notification]")
{
    // A notification server is free to hand out an id again once the notification wearing it is
    // gone, and nothing obliges it to tell us the first one closed -- an assumed close retires our
    // side only. The incoming id must therefore arrive cleared of whatever it used to name, or the
    // two directions disagree about who owns it.
    NotificationRouter router;
    CHECK_FALSE(router.onSent("osc-a", 100, /*replacedId*/ 0).has_value());
    CHECK_FALSE(router.onSent("osc-b", 100, /*replacedId*/ 0).has_value());

    CHECK(router.replacementFor("osc-a") == 0);
    CHECK(router.replacementFor("osc-b") == 100);

    auto const oscId = router.takeForServerEvent(100);
    REQUIRE(oscId.has_value());
    CHECK(*oscId == "osc-b");
}

TEST_CASE("NotificationRouter close resolves and forgets the mapping", "[notification]")
{
    NotificationRouter router;
    CHECK_FALSE(router.onSent("osc-a", 100, 0).has_value());

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
    CHECK_FALSE(router.onSent("osc-a", 100, 0).has_value());

    // A server id we never sent (another app's notification) resolves to nothing.
    CHECK_FALSE(router.takeForServerEvent(999).has_value());
    // Our own mapping is untouched.
    CHECK(router.replacementFor("osc-a") == 100);
}

TEST_CASE("NotificationRouter withdraws a notification closed before its send was answered", "[notification]")
{
    // notify() and close() are both posted, but the server id arrives only with the reply. A close
    // in between has nothing to close yet; were it dropped, the reply would then record the popup as
    // live, and it would stay on the desktop for good.
    NotificationRouter router;
    router.onSending("osc-a");

    CHECK_FALSE(router.takeForClose("osc-a").has_value());

    // The reply names the id, and that is the notification to withdraw.
    CHECK(router.onSent("osc-a", 7, /*replacedId*/ 0) == std::optional<ServerId> { 7 });

    // Withdrawn means forgotten: nothing is live under the identifier, and a late close event or
    // click for it is not ours any more.
    CHECK(router.replacementFor("osc-a") == 0);
    CHECK_FALSE(router.takeForServerEvent(7).has_value());

    // And the in-flight bookkeeping is settled: a later send stays live.
    router.onSending("osc-a");
    CHECK_FALSE(router.onSent("osc-a", 8, /*replacedId*/ 0).has_value());
    CHECK(router.replacementFor("osc-a") == 8);
}

TEST_CASE("NotificationRouter withdraws the first of two sends answered after both went out",
          "[notification]")
{
    // Two finishes within one round trip: the second send carries replacedId 0, because the first
    // had no id yet, so the server shows both. The first must be withdrawn, and the second must
    // stay closable and clickable.
    NotificationRouter router;
    router.onSending("osc-a");
    router.onSending("osc-a");

    CHECK_FALSE(router.onSent("osc-a", 1, /*replacedId*/ 0).has_value());
    CHECK(router.onSent("osc-a", 2, /*replacedId*/ 0) == std::optional<ServerId> { 1 });

    SECTION("the second one's click maps back to the identifier")
    {
        CHECK_FALSE(router.takeForServerEvent(1).has_value());
        CHECK(router.takeForServerEvent(2) == std::optional<std::string> { "osc-a" });
    }

    SECTION("the second one can be closed")
    {
        CHECK(router.takeForClose("osc-a") == std::optional<ServerId> { 2 });
    }
}

TEST_CASE("NotificationRouter owes a close only to the sends issued before it", "[notification]")
{
    NotificationRouter router;

    SECTION("close, then a fresh send: the fresh one stays")
    {
        router.onSending("osc-a");
        CHECK_FALSE(router.takeForClose("osc-a").has_value());
        router.onSending("osc-a");

        CHECK(router.onSent("osc-a", 1, /*replacedId*/ 0) == std::optional<ServerId> { 1 });
        CHECK_FALSE(router.onSent("osc-a", 2, /*replacedId*/ 0).has_value());

        CHECK(router.replacementFor("osc-a") == 2);
        CHECK(router.takeForServerEvent(2) == std::optional<std::string> { "osc-a" });
    }

    SECTION("a failed send settles its owed close too")
    {
        router.onSending("osc-a");
        CHECK_FALSE(router.takeForClose("osc-a").has_value());
        router.onSending("osc-a");

        CHECK_FALSE(router.onSent("osc-a", std::nullopt, /*replacedId*/ 0).has_value());
        CHECK_FALSE(router.onSent("osc-a", 2, /*replacedId*/ 0).has_value());

        CHECK(router.replacementFor("osc-a") == 2);
    }

    SECTION("a close with a live notification and a send in flight withdraws both")
    {
        CHECK_FALSE(router.onSent("osc-a", 1, /*replacedId*/ 0).has_value());
        router.onSending("osc-a");

        CHECK(router.takeForClose("osc-a") == std::optional<ServerId> { 1 });
        CHECK(router.onSent("osc-a", 2, /*replacedId*/ 1) == std::optional<ServerId> { 2 });
        CHECK(router.replacementFor("osc-a") == 0);
    }

    SECTION("another identifier's close owes nothing to this one")
    {
        router.onSending("osc-a");
        CHECK_FALSE(router.takeForClose("osc-b").has_value());

        CHECK_FALSE(router.onSent("osc-a", 1, /*replacedId*/ 0).has_value());
        CHECK(router.replacementFor("osc-a") == 1);
    }
}

TEST_CASE("NotificationRouter keeps only the newest notifications live", "[notification]")
{
    // Under the portal nothing reports a close, so an application that sends ever new identifiers and
    // never closes them must not grow the map without bound: past the capacity, the oldest is forgotten.
    using contour::platform::NotificationIdMap;
    constexpr auto Sent = NotificationIdMap::Capacity + 72;

    NotificationRouter router;
    auto const identifierOf = [](size_t n) {
        return std::format("app-{}", n);
    };
    auto const send = [&](size_t n) {
        router.onSending(identifierOf(n));
        CHECK_FALSE(router.onSent(identifierOf(n), static_cast<ServerId>(n), /*replacedId*/ 0).has_value());
    };
    auto const liveCount = [&] {
        return static_cast<size_t>(
            std::ranges::count_if(std::views::iota(size_t { 1 }, Sent + 1),
                                  [&](size_t n) { return router.replacementFor(identifierOf(n)) != 0; }));
    };

    SECTION("a burst of identifiers keeps the newest, forgetting the oldest")
    {
        for (auto const n: std::views::iota(size_t { 1 }, Sent + 1))
            send(n);

        CHECK(liveCount() == NotificationIdMap::Capacity);
        CHECK(router.replacementFor(identifierOf(Sent - NotificationIdMap::Capacity)) == 0);
        CHECK_FALSE(router.takeForServerEvent(1).has_value());
        CHECK(router.takeForServerEvent(static_cast<ServerId>(Sent)) == identifierOf(Sent));
    }

    SECTION("re-sending an identifier makes it the newest again")
    {
        // A session's finish notice is re-sent under one identifier: replaced, not left to age out.
        send(1);
        for (auto const n: std::views::iota(size_t { 2 }, NotificationIdMap::Capacity + 1))
            send(n);
        router.onSending(identifierOf(1));
        CHECK_FALSE(
            router.onSent(identifierOf(1), static_cast<ServerId>(Sent), /*replacedId*/ 1).has_value());
        send(NotificationIdMap::Capacity + 1);

        CHECK(router.replacementFor(identifierOf(1)) == static_cast<ServerId>(Sent));
        CHECK(router.replacementFor(identifierOf(2)) == 0);
        CHECK(liveCount() == NotificationIdMap::Capacity);
    }
}
