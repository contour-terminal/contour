// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ShaderConfig.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("createSurfaceFormat paces swaps only when asked to", "[contour][display]")
{
    namespace display = contour::display;
    CHECK(display::createSurfaceFormat(display::SwapPacing::VSync).swapInterval() == 1);
    CHECK(display::createSurfaceFormat(display::SwapPacing::Unpaced).swapInterval() == 0);
}
