// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <vtcontrol/Describe.hpp>
#include <vtcontrol/Json.hpp>

TEST_CASE("describeApi derives schemas from the structs", "[vtcontrol][describe]")
{
    auto const split = vtcontrol::describeApi("pane.split");
    REQUIRE(split);
    auto const schema = std::string_view { split->inputSchema.text };
    CHECK(schema.contains(R"("direction")"));
    CHECK(schema.contains(R"("below")"));
    CHECK(vtcontrol::describeApi("block.finished")->outputSchema.text.contains(R"("block")"));
    CHECK(vtcontrol::describeApi("block.finished")->inputSchema.text == "{}");
    CHECK(vtcontrol::describeApi("nope").error().code == vtcontrol::ControlErrorCode::MethodNotFound);
    CHECK(vtcontrol::listApi().methods.size() == 16);
    CHECK(vtcontrol::listApi().events.size() == 12);
}

TEST_CASE("a description encodes to valid, decodable JSON", "[vtcontrol][describe]")
{
    for (auto const name: { "daemon.info", "workspace.list", "pane.send", "pane.bell", "events.dropped" })
    {
        auto const description = vtcontrol::describeApi(name);
        REQUIRE(description);
        auto const json = vtcontrol::encodeJson(*description);
        auto const back = vtcontrol::decodeJson<vtcontrol::ApiDescription>(json);
        INFO(name);
        CHECK(back.has_value());
    }
}

TEST_CASE("listApi names every method and event once", "[vtcontrol][describe]")
{
    auto const list = vtcontrol::listApi();
    CHECK(list.methods.front().name == "daemon.info");
    CHECK(list.methods.back().name == "api.describe");
    CHECK(list.events.front().name == "block.finished");
    CHECK(list.events.back().name == "events.dropped");
}
