// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <variant>

#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>

TEST_CASE("events round-trip by their catalog name", "[vtcontrol][events]")
{
    auto const event = vtcontrol::Event { .origin = { .pane = "pane:p" },
                                          .payload = vtcontrol::BlockFinishedEvent {
                                              .block = { .id = "block:p/1", .exitCode = 3 } } };
    auto const json = vtcontrol::encodeEvent(event);
    CHECK(json.starts_with(R"({"type":"block.finished")"));
    auto const back = vtcontrol::decodeEvent(json);
    REQUIRE(back);
    CHECK(back->origin.pane == "pane:p");
    CHECK(std::get<vtcontrol::BlockFinishedEvent>(back->payload).block.exitCode == 3);
    CHECK(vtcontrol::decodeEvent(R"({"type":"pane.teleported","payload":{}})").error().code
          == vtcontrol::ControlErrorCode::BadRequest);
}

TEST_CASE("events without a payload body decode", "[vtcontrol][events]")
{
    auto const json = vtcontrol::encodeEvent(
        vtcontrol::Event { .origin = { .pane = "pane:p" }, .payload = vtcontrol::BellEvent {} });
    CHECK(json == R"({"type":"pane.bell","window":"","tab":"","pane":"pane:p","payload":{}})");
    auto const back = vtcontrol::decodeEvent(json);
    REQUIRE(back);
    CHECK(std::holds_alternative<vtcontrol::BellEvent>(back->payload));
    auto const dropped = vtcontrol::decodeEvent(R"({"type":"events.dropped","payload":{"count":9}})");
    REQUIRE(dropped);
    CHECK(std::get<vtcontrol::EventsDroppedEvent>(dropped->payload).count == 9);
}

TEST_CASE("a malformed event envelope is a bad request", "[vtcontrol][events]")
{
    CHECK(vtcontrol::decodeEvent("not json").error().code == vtcontrol::ControlErrorCode::BadRequest);
    CHECK(vtcontrol::decodeEvent(R"({"type":"pane.bell","extra":1})").error().code
          == vtcontrol::ControlErrorCode::BadRequest);
    CHECK(vtcontrol::decodeEvent(R"({"type":"events.dropped","payload":{"count":"x"}})").error().code
          == vtcontrol::ControlErrorCode::BadRequest);
}

TEST_CASE("every payload alternative has exactly one catalog row with a unique name", "[vtcontrol][events]")
{
    STATIC_REQUIRE(std::tuple_size_v<decltype(vtcontrol::EventCatalog)>
                   == std::variant_size_v<vtcontrol::EventPayload>);

    auto names = std::set<std::string_view> {};
    std::apply([&](auto const&... row) { (names.insert(row.name), ...); }, vtcontrol::EventCatalog);
    CHECK(names.size() == std::variant_size_v<vtcontrol::EventPayload>);

    // The row at index I describes the payload at index I.
    auto const rowsMatch = []<std::size_t... Index>(std::index_sequence<Index...> /*unused*/) {
        return std::array { (vtcontrol::eventName(vtcontrol::EventPayload { std::in_place_index<Index> })
                             == std::get<Index>(vtcontrol::EventCatalog).name)... };
    }(std::make_index_sequence<std::variant_size_v<vtcontrol::EventPayload>> {});
    CHECK(std::ranges::all_of(rowsMatch, std::identity {}));
}

TEST_CASE("every event names the permission a subscriber needs", "[vtcontrol][events][grants]")
{
    CHECK(vtcontrol::eventPermission("block.finished") == vtcontrol::Permission::Screen);
    CHECK(vtcontrol::eventPermission("pane.bell") == vtcontrol::Permission::Read);
    CHECK(vtcontrol::eventPermission("pane.programStatusChanged") == vtcontrol::Permission::Screen);
    CHECK_FALSE(vtcontrol::eventPermission("pane.teleported"));
}
