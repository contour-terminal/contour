// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>

#include <vthost/CellPixelBounds.hpp>

using vthost::MaxCellPixelExtent;
using vthost::sanePixelSize;

TEST_CASE("sanePixelSize accepts each extent in 1..MaxCellPixelExtent only", "[vthost][cellsize]")
{
    STATIC_CHECK(MaxCellPixelExtent == 1024);
    auto const extent = GENERATE(as<uint32_t> {}, 0, 1, 1024, 1025);
    auto const valid = extent >= 1 && extent <= 1024;
    INFO("extent " << extent);

    // The probed extent on each axis, the other one valid.
    auto const wide = sanePixelSize(extent, 20);
    auto const tall = sanePixelSize(10, extent);
    CHECK(wide.has_value() == valid);
    CHECK(tall.has_value() == valid);
    if (valid)
    {
        CHECK(*wide->width == extent);
        CHECK(*wide->height == 20);
        CHECK(*tall->width == 10);
        CHECK(*tall->height == extent);
    }
}
