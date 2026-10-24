// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Dispatch.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>

using vtcontrol::ControlGrants;
using vtcontrol::Permission;

TEST_CASE("ControlGrants parse, combine and refuse unknown names", "[vtcontrol][grants]")
{
    static_assert(ControlGrants::of({ Permission::Read }).contains(Permission::Read));
    static_assert(!ControlGrants::none().contains(Permission::Read));
    static_assert(
        ControlGrants::all()
        == ControlGrants::of({ Permission::Read, Permission::Screen, Permission::Input, Permission::Write }));

    auto const readScreen = vtcontrol::parseGrants("read,screen");
    REQUIRE(readScreen);
    CHECK(*readScreen == ControlGrants::of({ Permission::Read, Permission::Screen }));
    CHECK_FALSE(readScreen->contains(Permission::Input));
    CHECK(vtcontrol::toString(*readScreen) == "read,screen");

    auto const all = vtcontrol::parseGrants("all");
    REQUIRE(all);
    CHECK(*all == ControlGrants::all());
    CHECK(vtcontrol::toString(*all) == "read,screen,input,write");
    CHECK(vtcontrol::toString(ControlGrants::none()) == "none");

    auto const unknown = vtcontrol::parseGrants("read,fly");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == vtcontrol::ControlErrorCode::BadRequest);
    CHECK(unknown.error().message.contains("'fly'"));

    CHECK_FALSE(vtcontrol::parseGrants(""));
    CHECK_FALSE(vtcontrol::parseGrants("read,,screen"));
    CHECK_FALSE(vtcontrol::parseGrants("read, screen"));
}

TEST_CASE("dispatch refuses a method outside the caller's grants", "[vtcontrol][dispatch][grants]")
{
    auto api = vtcontrol::testing::FakeControlApi {};
    api.context.granted = ControlGrants::of({ Permission::Read, Permission::Screen });

    using Result = std::optional<std::expected<std::string, vtcontrol::ControlError>>;
    auto capture = Result {};
    vtcontrol::dispatch(api,
                        api.context,
                        vtcontrol::Caller::Socket,
                        "pane.capture",
                        R"({"pane":"pane:p"})",
                        [&](auto result) { capture = std::move(result); });
    REQUIRE(capture);
    CHECK(*capture);
    CHECK(api.lastCapture);

    auto send = Result {};
    vtcontrol::dispatch(api,
                        api.context,
                        vtcontrol::Caller::Socket,
                        "pane.send",
                        R"({"pane":"pane:p","text":"rm -rf ~\n"})",
                        [&](auto result) { send = std::move(result); });
    REQUIRE(send);
    REQUIRE_FALSE(*send);
    CHECK(send->error().code == vtcontrol::ControlErrorCode::Forbidden);
    CHECK(send->error().message.contains("needs grant 'input'"));
    CHECK_FALSE(api.lastSend);
}
