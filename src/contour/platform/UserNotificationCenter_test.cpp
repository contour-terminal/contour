// SPDX-License-Identifier: Apache-2.0
//
// The notification centre's Qt-free halves: the authorization gate that decides whether a post asks,
// waits, goes out or is dropped, and the table that routes a click to the subscriber it belongs to.

#include <contour/platform/UserNotificationCenter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using contour::platform::AuthorizationAnswer;
using contour::platform::AuthorizationGate;
using contour::platform::AuthorizationHoldLimit;
using contour::platform::AuthorizationState;
using contour::platform::GateAction;
using contour::platform::LazyUserNotificationCenter;
using contour::platform::NullUserNotificationCenter;
using contour::platform::ReleaseTo;
using contour::platform::SystemAuthorization;
using contour::platform::UserNotificationAvailability;
using contour::platform::UserNotificationRequest;
using contour::platform::UserNotificationRoutes;

namespace
{

[[nodiscard]] UserNotificationRequest requestWithId(std::string id)
{
    return UserNotificationRequest { .requestId = std::move(id), .title = "t", .body = "b" };
}

[[nodiscard]] std::vector<std::string> idsOf(std::vector<UserNotificationRequest> const& requests)
{
    auto ids = std::vector<std::string> {};
    for (auto const& request: requests)
        ids.push_back(request.requestId);
    return ids;
}

} // namespace

TEST_CASE("AuthorizationGate: the first post asks, the ones while the prompt is up wait",
          "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };

    CHECK(gate.admit(requestWithId("a")) == GateAction::Ask);
    CHECK(gate.state() == AuthorizationState::Pending);
    CHECK(gate.admit(requestWithId("b")) == GateAction::Hold);
    CHECK(gate.admit(requestWithId("c")) == GateAction::Hold);

    auto const released = gate.answer(AuthorizationState::Granted);

    CHECK(idsOf(released) == std::vector<std::string> { "a", "b", "c" });
    CHECK(gate.state() == AuthorizationState::Granted);
    CHECK(gate.admit(requestWithId("d")) == GateAction::Send);
}

TEST_CASE("AuthorizationGate: a refusal drops what waited and everything after", "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);

    CHECK(gate.answer(AuthorizationState::Denied).empty());
    CHECK(gate.state() == AuthorizationState::Denied);
    CHECK(gate.admit(requestWithId("b")) == GateAction::Drop);
}

TEST_CASE("AuthorizationGate: a start state the system already answered never asks", "[usernotifications]")
{
    CHECK(AuthorizationGate { AuthorizationState::Granted }.admit(requestWithId("a")) == GateAction::Send);
    CHECK(AuthorizationGate { AuthorizationState::Denied }.admit(requestWithId("a")) == GateAction::Drop);
    CHECK(AuthorizationGate { AuthorizationState::Pending }.admit(requestWithId("a")) == GateAction::Hold);
}

TEST_CASE("AuthorizationGate: the hold keeps the newest requests", "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Pending };
    constexpr auto Extra = std::size_t { 2 };
    for (auto const i: std::views::iota(std::size_t { 0 }, AuthorizationHoldLimit + Extra))
        REQUIRE(gate.admit(requestWithId(std::to_string(i))) == GateAction::Hold);

    auto const released = gate.answer(AuthorizationState::Granted);

    REQUIRE(released.size() == AuthorizationHoldLimit);
    CHECK(released.front().requestId == std::to_string(Extra));
    CHECK(released.back().requestId == std::to_string(AuthorizationHoldLimit + Extra - 1));
}

TEST_CASE("AuthorizationGate: a request replaces a held one with its id", "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Pending };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Hold);
    REQUIRE(gate.admit(requestWithId("b")) == GateAction::Hold);
    auto newer = requestWithId("a");
    newer.body = "newer";
    REQUIRE(gate.admit(newer) == GateAction::Hold);

    auto const released = gate.answer(AuthorizationState::Granted);

    REQUIRE(released.size() == 2);
    CHECK(released[0].requestId == "a");
    CHECK(released[0].body == "newer");
    CHECK(released[1].requestId == "b");
}

TEST_CASE("AuthorizationGate: a forgotten request is not released", "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Pending };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Hold);
    REQUIRE(gate.admit(requestWithId("b")) == GateAction::Hold);

    gate.forget("a");

    CHECK(idsOf(gate.answer(AuthorizationState::Granted)) == std::vector<std::string> { "b" });
}

TEST_CASE("AuthorizationGate: anything but granted or denied is not an answer", "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);

    CHECK(gate.answer(AuthorizationState::Pending).empty());
    CHECK(gate.state() == AuthorizationState::Pending);
    CHECK(idsOf(gate.answer(AuthorizationState::Granted)) == std::vector<std::string> { "a" });
}

TEST_CASE("AuthorizationGate: the settings and the prompt's answer both granting release the hold once",
          "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);

    auto const fromSettings = gate.settingsReported(SystemAuthorization::Authorized);
    auto const fromPrompt = gate.requestAnswered(AuthorizationAnswer::Granted);

    CHECK(idsOf(fromSettings.requests) == std::vector<std::string> { "a" });
    CHECK(fromSettings.destination == ReleaseTo::Centre);
    CHECK(fromPrompt.requests.empty());
    CHECK(gate.state() == AuthorizationState::Granted);
    CHECK(gate.availability() == UserNotificationAvailability::Available);
}

TEST_CASE("AuthorizationGate: settings that say denied while the prompt is up drop what waited",
          "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);
    REQUIRE(gate.admit(requestWithId("b")) == GateAction::Hold);

    auto const released = gate.settingsReported(SystemAuthorization::Denied);

    CHECK(released.requests.empty());
    CHECK(gate.state() == AuthorizationState::Denied);
    CHECK(gate.admit(requestWithId("c")) == GateAction::Drop);
}

TEST_CASE("AuthorizationGate: settings that are not determined change nothing", "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };

    SECTION("before any post")
    {
        CHECK(gate.settingsReported(SystemAuthorization::NotDetermined).requests.empty());
        CHECK(gate.state() == AuthorizationState::Unasked);
        CHECK(gate.admit(requestWithId("a")) == GateAction::Ask);
    }

    SECTION("while the prompt is up")
    {
        REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);
        CHECK(gate.settingsReported(SystemAuthorization::NotDetermined).requests.empty());
        CHECK(gate.state() == AuthorizationState::Pending);
        CHECK(idsOf(gate.requestAnswered(AuthorizationAnswer::Granted).requests)
              == std::vector<std::string> { "a" });
    }
}

TEST_CASE("AuthorizationGate: a refusal by the user releases nothing and keeps the centre available",
          "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);

    CHECK(gate.requestAnswered(AuthorizationAnswer::Refused).requests.empty());

    CHECK(gate.state() == AuthorizationState::Denied);
    CHECK(gate.availability() == UserNotificationAvailability::Available);
}

TEST_CASE("AuthorizationGate: a failed request turns unavailable and hands what waited to the fallback",
          "[usernotifications]")
{
    auto gate = AuthorizationGate { AuthorizationState::Unasked };
    REQUIRE(gate.admit(requestWithId("a")) == GateAction::Ask);
    REQUIRE(gate.admit(requestWithId("b")) == GateAction::Hold);

    auto const released = gate.requestAnswered(AuthorizationAnswer::Failed);

    CHECK(idsOf(released.requests) == std::vector<std::string> { "a", "b" });
    CHECK(released.destination == ReleaseTo::Fallback);
    CHECK(gate.availability() == UserNotificationAvailability::Unavailable);
    CHECK(gate.admit(requestWithId("c")) == GateAction::Drop);
    CHECK(gate.requestAnswered(AuthorizationAnswer::Failed).requests.empty());
}

TEST_CASE("UserNotificationRoutes: a request that cannot be raised goes back to its owner",
          "[usernotifications]")
{
    auto routes = std::make_shared<UserNotificationRoutes>();
    auto seven = std::vector<std::string> {};
    auto seventy = std::vector<std::string> {};
    auto const subscribeSeven = UserNotificationRoutes::subscribe(
        routes,
        "contour-session-7/",
        [](std::string_view) {},
        [&](UserNotificationRequest const& request) { seven.push_back(request.requestId); });
    auto const subscribeSeventy = UserNotificationRoutes::subscribe(
        routes,
        "contour-session-70/",
        [](std::string_view) {},
        [&](UserNotificationRequest const& request) { seventy.push_back(request.requestId); });

    CHECK(routes->undeliver(requestWithId("contour-session-70/x")));
    CHECK_FALSE(routes->undeliver(requestWithId("contour-session-8/x")));

    CHECK(seven.empty());
    CHECK(seventy == std::vector<std::string> { "contour-session-70/x" });
}

namespace
{
/// A factory that counts how often it was asked to build a centre.
[[nodiscard]] std::function<std::unique_ptr<contour::platform::IUserNotificationCenter>()> countingFactory(
    int& builds)
{
    return [&builds] {
        ++builds;
        return std::make_unique<NullUserNotificationCenter>();
    };
}
} // namespace

TEST_CASE("LazyUserNotificationCenter: nothing is built until the centre is used", "[usernotifications]")
{
    auto builds = 0;
    auto center = LazyUserNotificationCenter { countingFactory(builds) };

    SECTION("constructed, and withdrawn from, builds nothing")
    {
        center.withdraw("contour-session-7/1");
        CHECK(builds == 0);
        CHECK_FALSE(center.isBuilt());
    }

    SECTION("a post builds it, once")
    {
        center.post(requestWithId("a"));
        center.post(requestWithId("b"));
        CHECK(builds == 1);
        CHECK(center.isBuilt());
    }

    SECTION("a subscription builds it")
    {
        auto const subscription =
            center.subscribe("p/", [](std::string_view) {}, [](UserNotificationRequest const&) {});
        CHECK(builds == 1);
    }

    SECTION("asking for availability builds it and relays the answer")
    {
        CHECK(center.availability() == UserNotificationAvailability::Unavailable);
        CHECK(builds == 1);
    }
}

TEST_CASE("UserNotificationRoutes: a click reaches the subscriber whose prefix it carries",
          "[usernotifications]")
{
    auto routes = std::make_shared<UserNotificationRoutes>();
    auto seven = std::vector<std::string> {};
    auto seventy = std::vector<std::string> {};
    auto const subscribeSeven = UserNotificationRoutes::subscribe(
        routes,
        "contour-session-7/",
        [&](std::string_view id) { seven.emplace_back(id); },
        [](UserNotificationRequest const&) {});
    auto const subscribeSeventy = UserNotificationRoutes::subscribe(
        routes,
        "contour-session-70/",
        [&](std::string_view id) { seventy.emplace_back(id); },
        [](UserNotificationRequest const&) {});

    CHECK(routes->route("contour-session-70/x"));

    CHECK(seven.empty());
    CHECK(seventy == std::vector<std::string> { "x" });
    CHECK_FALSE(routes->route("contour-session-8/x"));
}

TEST_CASE("UserNotificationRoutes: a released subscription takes no more clicks", "[usernotifications]")
{
    auto routes = std::make_shared<UserNotificationRoutes>();
    auto clicks = 0;
    auto subscription = UserNotificationRoutes::subscribe(
        routes, "p/", [&](std::string_view) { ++clicks; }, [](UserNotificationRequest const&) {});
    REQUIRE(routes->size() == 1);

    SECTION("released explicitly")
    {
        subscription.release();
    }

    SECTION("moved from, then destroyed")
    {
        auto const moved = std::move(subscription);
        CHECK(routes->size() == 1);
    }

    CHECK(routes->size() == 0);
    CHECK_FALSE(routes->route("p/1"));
    CHECK(clicks == 0);
}

TEST_CASE("UserNotificationRoutes: a subscription outliving its table releases nothing",
          "[usernotifications]")
{
    auto routes = std::make_shared<UserNotificationRoutes>();
    auto subscription = UserNotificationRoutes::subscribe(
        routes, "p/", [](std::string_view) {}, [](UserNotificationRequest const&) {});

    routes.reset();

    subscription.release();
    SUCCEED("the weak reference found nothing to remove");
}
