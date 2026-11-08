// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vtcontrol/ObjectRef.hpp>

TEST_CASE("object and block references", "[vtcontrol][ids]")
{
    auto const uuid = *vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(toString(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Pane, uuid })
          == "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(vtcontrol::parseObjectRef("tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ab")
          == vtcontrol::ObjectRef { vtcontrol::ObjectKind::Tab, uuid });
    CHECK_FALSE(vtcontrol::parseObjectRef("block:018f3c4e-9a1b-7c2d-8e3f-0123456789ab"));
    CHECK_FALSE(vtcontrol::parseObjectRef("pane:nonsense"));
    CHECK_FALSE(vtcontrol::parseObjectRef("pane"));
    auto const block = vtcontrol::BlockRef { uuid, vtcontrol::blockSerial(1, 42) };
    CHECK(toString(block) == "block:018f3c4e-9a1b-7c2d-8e3f-0123456789ab/4294967338");
    CHECK(vtcontrol::parseBlockRef(toString(block)) == block);
    CHECK_FALSE(vtcontrol::parseBlockRef("block:018f3c4e-9a1b-7c2d-8e3f-0123456789ab/"));
    CHECK_FALSE(vtcontrol::parseBlockRef("block:018f3c4e-9a1b-7c2d-8e3f-0123456789ab/12x"));
    CHECK_FALSE(vtcontrol::parseBlockRef("pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab/1"));
}

TEST_CASE("ObjectRef: workspace kind round-trips, the old window prefix is rejected", "[vtcontrol][ids]")
{
    SECTION("workspace round-trips")
    {
        auto const text = std::string { "workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab" };
        auto const ref = vtcontrol::parseObjectRef(text);
        REQUIRE(ref.has_value());
        CHECK(ref->kind == vtcontrol::ObjectKind::Workspace);
        CHECK(toString(*ref) == text);
    }

    SECTION("window prefix is rejected")
    {
        CHECK_FALSE(vtcontrol::parseObjectRef("window:018f3c4e-9a1b-7c2d-8e3f-0123456789ab"));
    }
}
