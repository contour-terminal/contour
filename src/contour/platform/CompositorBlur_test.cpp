// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/CompositorBlur.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using contour::platform::BlurBehindWish;
using contour::platform::CompositorBlurRegion;
using contour::platform::CompositorBlurWishes;
using contour::platform::regionOf;

namespace
{

struct RegionRow
{
    CompositorBlurWishes wishes;
    CompositorBlurRegion region;
};

// The profile's whole-window blur covers the band whatever its height; the band alone blurs only with a
// height to blur; neither asks for nothing.
constexpr auto RegionRows = std::array {
    RegionRow { .wishes = { .window = BlurBehindWish::Off, .bandHeight = 0.0 },
                .region = CompositorBlurRegion::None },
    RegionRow { .wishes = { .window = BlurBehindWish::Off, .bandHeight = -4.0 },
                .region = CompositorBlurRegion::None },
    RegionRow { .wishes = { .window = BlurBehindWish::Off, .bandHeight = 40.0 },
                .region = CompositorBlurRegion::Band },
    RegionRow { .wishes = { .window = BlurBehindWish::On, .bandHeight = 0.0 },
                .region = CompositorBlurRegion::WholeWindow },
    RegionRow { .wishes = { .window = BlurBehindWish::On, .bandHeight = 40.0 },
                .region = CompositorBlurRegion::WholeWindow },
};

} // namespace

TEST_CASE("a window's blur wishes add up to one region", "[contour][platform][blur]")
{
    for (auto const& row: RegionRows)
    {
        CAPTURE(static_cast<int>(row.wishes.window), row.wishes.bandHeight);
        CHECK(regionOf(row.wishes) == row.region);
    }
}

TEST_CASE("turning the whole-window blur off keeps the band's", "[contour][platform][blur]")
{
    // The two wishes are updated apart, each keeping the other's: the band survives the profile's blur
    // being switched off, which a single shared flag would have lost.
    auto wishes = CompositorBlurWishes { .window = BlurBehindWish::On, .bandHeight = 40.0 };
    wishes.window = BlurBehindWish::Off;
    CHECK(regionOf(wishes) == CompositorBlurRegion::Band);
}
