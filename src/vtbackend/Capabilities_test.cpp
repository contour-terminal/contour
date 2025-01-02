// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Capabilities.h>

#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

using namespace std::string_view_literals;
using crispy::fromHexString;

TEST_CASE("Capabilities.codeFromName")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const capName = fromHexString("62656c"sv).value();
    auto const tn = tcap.codeFromName(capName);
    REQUIRE(tn.has_value());
    CHECK(tn.value().code == 0x626c);
    CHECK(tn.value().hex() == "626C");
}

TEST_CASE("Capabilities.get")
{
    vtbackend::capabilities::StaticDatabase const tcap;
    auto const rgb = tcap.stringCapability("RGB");
    REQUIRE(rgb == "8/8/8");

    auto const colors = tcap.numericCapability("colors");
    REQUIRE(colors == std::numeric_limits<int16_t>::max());

    auto const bce = tcap.numericCapability("bce");
    REQUIRE(bce);
}
