// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Capabilities.h>

#include <crispy/utils.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

using namespace std::string_view_literals;
using crispy::fromHexString;

TEST_CASE("Capabilities.codeFromName")
{
    terminal::capabilities::StaticDatabase tcap;
    auto const capName = fromHexString("62656c"sv).value();
    auto const tn = tcap.codeFromName(capName);
    REQUIRE(tn.has_value());
    CHECK(tn.value().code == 0x626c);
    CHECK(tn.value().hex() == "626C");
}

TEST_CASE("Capabilities.get")
{
    terminal::capabilities::StaticDatabase tcap;
    auto const rgb = tcap.stringCapability("RGB");
    REQUIRE(rgb == "8/8/8");

    auto const colors = tcap.numericCapability("colors");
    REQUIRE(colors == 256);

    auto const bce = tcap.numericCapability("bce");
    REQUIRE(bce);
}
