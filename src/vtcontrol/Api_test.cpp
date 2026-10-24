// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Json.hpp>

TEST_CASE("API JSON keeps the member spelling", "[vtcontrol][api]")
{
    auto const json = vtcontrol::encodeJson(vtcontrol::SplitRequest {
        .pane = "pane:x", .direction = vtcontrol::SplitDirection::Below, .ratioPercent = 30 });
    CHECK(
        json
        == R"({"pane":"pane:x","direction":"below","ratioPercent":30,"cwd":"","command":[],"label":"","focus":"take"})");
    CHECK_FALSE(vtcontrol::decodeJson<vtcontrol::SplitRequest>(R"({"direction":"left"})"));
    CHECK_FALSE(vtcontrol::decodeJson<vtcontrol::SplitRequest>(R"({"ratio":0.3})"));
}

TEST_CASE("API replies round-trip through JSON", "[vtcontrol][api]")
{
    auto const reply = vtcontrol::ListReply {
        .windows = { vtcontrol::WindowInfo {
            .id = "window:w",
            .tabs = { vtcontrol::TabInfo {
                .id = "tab:t",
                .isActive = true,
                .panes = { vtcontrol::PaneInfo { .id = "pane:p",
                                                 .size = { .columns = 80, .lines = 24 } } } } } } }
    };
    auto const back = vtcontrol::decodeJson<vtcontrol::ListReply>(vtcontrol::encodeJson(reply));
    REQUIRE(back);
    REQUIRE(back->windows.size() == 1);
    CHECK(back->windows[0].tabs[0].isActive);
    CHECK(back->windows[0].tabs[0].panes[0].size.columns == 80);
}

TEST_CASE("an empty request decodes from nothing and from an empty object", "[vtcontrol][api]")
{
    CHECK(vtcontrol::decodeJson<vtcontrol::DaemonInfoRequest>(""));
    CHECK(vtcontrol::decodeJson<vtcontrol::DaemonInfoRequest>("{}"));
    CHECK_FALSE(vtcontrol::decodeJson<vtcontrol::DaemonInfoRequest>(R"({"x":1})"));
    CHECK(vtcontrol::encodeJson(vtcontrol::EmptyReply {}) == "{}");
}

TEST_CASE("ApiDescription carries schemas and examples as raw JSON", "[vtcontrol][api]")
{
    auto description = vtcontrol::ApiDescription { .name = "list" };
    description.inputSchema.text = R"({"type":"object"})";
    auto const json = vtcontrol::encodeJson(description);
    CHECK(json.contains(R"("inputSchema":{"type":"object"})"));
}

TEST_CASE("every API record is tagged", "[vtcontrol][api]")
{
    STATIC_REQUIRE(vtcontrol::ApiRecord<vtcontrol::SplitRequest>);
    STATIC_REQUIRE(vtcontrol::ApiRecord<vtcontrol::EmptyReply>);
    STATIC_REQUIRE(vtcontrol::ApiRecord<vtcontrol::BlockInfo>);
    STATIC_REQUIRE_FALSE(vtcontrol::ApiRecord<int>);
}

TEST_CASE("API enums are spelled by their tables", "[vtcontrol][api]")
{
    CHECK(vtcontrol::nameOf(vtcontrol::SendWait::CommandFinished) == "commandFinished");
    CHECK(vtcontrol::nameOf(vtcontrol::ProgramStatusReason::ProcessExit) == "processExit");
    CHECK(vtcontrol::enumFromName<vtcontrol::BlockState>("superseded") == vtcontrol::BlockState::Superseded);
}
