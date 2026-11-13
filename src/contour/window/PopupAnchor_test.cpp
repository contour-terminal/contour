// SPDX-License-Identifier: Apache-2.0
#include <contour/window/PopupAnchor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace contour::window;

TEST_CASE("popupBelow hangs a dropdown under its control and keeps it on screen", "[popupanchor]")
{
    constexpr auto Screen = PopupRect { .x = 0, .y = 0, .width = 1000, .height = 800 };
    constexpr auto Chip = PopupRect { .x = 100, .y = 10, .width = 80, .height = 24 };
    struct Case
    {
        char const* what = "";
        PopupRect control {};
        int width = 0;
        int height = 0;
        PopupPoint expected {};
    };
    for (auto const& [what, control, width, height, expected]: std::array {
             Case { "below, on the control's leading edge", Chip, 300, 200, { .x = 100, .y = 38 } },
             Case { "pushed in from the right edge",
                    PopupRect { .x = 900, .y = 10, .width = 80, .height = 24 },
                    300,
                    200,
                    { .x = 700, .y = 38 } },
             Case { "above, where only the space above fits",
                    PopupRect { .x = 100, .y = 700, .width = 80, .height = 24 },
                    300,
                    200,
                    { .x = 100, .y = 496 } },
             Case { "below and clamped, where neither fits", Chip, 300, 900, { .x = 100, .y = 0 } },
         })
    {
        INFO(what);
        CHECK(popupBelow(control, width, height, 4, Screen) == expected);
    }
}
