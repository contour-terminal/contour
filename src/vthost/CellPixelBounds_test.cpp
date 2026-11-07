// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>

#include <vthost/CellPixelBounds.hpp>

using vthost::DefaultCellPixelSize;
using vthost::MaxCellPixelExtent;
using vthost::sanePixelSize;

TEST_CASE("sanePixelSize accepts each extent in 1..MaxCellPixelExtent only", "[vthost][cellsize]")
{
    STATIC_CHECK(MaxCellPixelExtent == 1024);
    // Negative and beyond 32 bits too: a source's whole range comes in unclamped.
    auto const extent = GENERATE(as<int64_t> {}, -1, 0, 1, 1024, 1025, int64_t { UINT32_MAX } + 10);
    auto const valid = extent >= 1 && extent <= 1024;
    INFO("extent " << extent);

    // The probed extent on each axis, the other one valid.
    auto const wide = sanePixelSize(extent, 20);
    auto const tall = sanePixelSize(10, extent);
    CHECK(wide.has_value() == valid);
    CHECK(tall.has_value() == valid);
    if (valid)
    {
        auto const unsignedExtent = static_cast<uint32_t>(extent);
        CHECK(*wide->width == unsignedExtent);
        CHECK(*wide->height == 20);
        CHECK(*tall->width == 10);
        CHECK(*tall->height == unsignedExtent);
    }
}

TEST_CASE("DefaultCellPixelSize is the VT340's text cell, within bounds", "[vthost][cellsize]")
{
    STATIC_CHECK(DefaultCellPixelSize == vtpty::ImageSize { vtpty::Width(10), vtpty::Height(20) });
    STATIC_CHECK(sanePixelSize(10, 20) == DefaultCellPixelSize);
}
