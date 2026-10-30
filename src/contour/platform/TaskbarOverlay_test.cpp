// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TaskbarOverlay.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <utility>

using namespace contour::platform;

TEST_CASE("The overlay spells small counts and caps large ones", "[contour][taskbaroverlay]")
{
    constexpr auto Rows = std::array<std::pair<unsigned, std::pair<char const*, char const*>>, 5> { {
        { 0, { "", "" } },
        { 1, { "1", "1 tab waiting for you" } },
        { 9, { "9", "9 tabs waiting for you" } },
        { 10, { "9+", "10 tabs waiting for you" } },
        { 250, { "9+", "250 tabs waiting for you" } },
    } };
    for (auto const& [waiting, expected]: Rows)
    {
        CHECK(overlayBadgeText(waiting) == expected.first);
        CHECK(overlayDescription(waiting) == expected.second);
    }
}

TEST_CASE("The overlay's ink reads on its disc", "[contour][taskbaroverlay]")
{
    auto const ink = overlayInkFor(OverlayDiscColor);
    CHECK(vtbackend::contrastRatio(ink, OverlayDiscColor) >= 4.5);
    // The choice follows the disc, not a fixed answer.
    CHECK(overlayInkFor(vtbackend::RGBColor { 0, 0, 0 }) == vtbackend::RGBColor { 0xFF, 0xFF, 0xFF });
    CHECK(overlayInkFor(vtbackend::RGBColor { 0xFF, 0xFF, 0xFF }) == vtbackend::RGBColor { 0, 0, 0 });
}

TEST_CASE("The overlay's disc or its ring reads on the light and the dark taskbar",
          "[contour][taskbaroverlay]")
{
    for (auto const& taskbar: OverlayTaskbarBackgrounds)
    {
        auto const edge = std::max(vtbackend::contrastRatio(OverlayDiscColor, taskbar),
                                   vtbackend::contrastRatio(OverlayOutlineColor, taskbar));
        CHECK(edge >= 3.0);
    }
    // The disc alone is not enough on the light taskbar, which is why the ring exists.
    CHECK(vtbackend::contrastRatio(OverlayDiscColor, OverlayTaskbarBackgrounds.front()) < 3.0);
    CHECK(vtbackend::contrastRatio(OverlayOutlineColor, OverlayTaskbarBackgrounds.front()) >= 3.0);
}

TEST_CASE("The overlay sets two characters smaller than one", "[contour][taskbaroverlay]")
{
    for (auto const iconSize: { 16, 24, 32 })
    {
        auto const single = overlayTextPixelSize(iconSize, overlayBadgeText(5).size());
        auto const doubled = overlayTextPixelSize(iconSize, overlayBadgeText(50).size());
        CHECK(doubled < single);
        CHECK(single <= iconSize);
        CHECK(doubled > 0);
        // Longer text than any row spells keeps the smallest size.
        CHECK(overlayTextPixelSize(iconSize, 5) == doubled);
    }
    CHECK(overlayTextPixelSize(16, 1) == 12);
    CHECK(overlayTextPixelSize(16, 2) == 8);
}
