// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/VTType.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <utility>

using vtbackend::fromVTTypeNum;
using vtbackend::nameOfVTType;
using vtbackend::VTType;
using vtbackend::VTTypes;

TEST_CASE("fromVTTypeNum round-trips every table row", "[VTType]")
{
    for (auto const& [type, name]: VTTypes)
    {
        auto const decoded = fromVTTypeNum(std::to_underlying(type));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == type);
        CHECK(nameOfVTType(*decoded) == name);
    }
}

TEST_CASE("fromVTTypeNum rejects numbers no VTType uses", "[VTType]")
{
    // The numbering is sparse: the gaps below sit BETWEEN assigned values, which is exactly why a
    // cast or a range check would silently manufacture a terminal type that does not exist.
    for (auto const unassigned: { 3U, 17U, 20U, 25U, 42U, 62U, 66U, 255U, 1000U })
        CHECK_FALSE(fromVTTypeNum(unassigned).has_value());
}

TEST_CASE("VTType numbering is sparse and NOT ordered by capability", "[VTType]")
{
    // Guards the reason fromVTTypeNum exists at all. VT330 (18) numerically precedes VT320 (24)
    // while being the LATER, more capable model -- so ordering by number is not ordering by
    // conformance level, and clamping a wire value into a numeric range is meaningless.
    CHECK(std::to_underlying(VTType::VT330) < std::to_underlying(VTType::VT320));
    CHECK(vtbackend::conformanceLevelOf(VTType::VT330) == vtbackend::conformanceLevelOf(VTType::VT320));
}

TEST_CASE("the VTType formatter renders each table row's name", "[VTType]")
{
    // The formatter reads the same table, so this also pins the two together: a row added without a
    // name would render empty here.
    for (auto const& [type, name]: VTTypes)
        CHECK(std::format("{}", type) == name);
}
