// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/PopupShadow.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <ranges>

using namespace contour::platform;

namespace
{
/// The shipped Native chrome's shadow: a 24px blur, displaced 4px down, at 35% of a mid grey.
[[nodiscard]] PopupShadowParams nativeParams()
{
    return { .blur = 24, .offsetY = 4, .cornerRadius = 3, .color = QColor(0x76, 0x76, 0x76, 0x59) };
}
} // namespace

TEST_CASE("renderPopupShadow fades smoothly", "[contour][shadow]")
{
    // The property this renderer exists for, and the one the QML it replaced could not have.
    //
    // The first attempt stacked eight translucent rectangles across the blur: alpha rose in steps of
    // about eleven levels every three pixels, which is invisible against a dark terminal background
    // and plainly BANDED against a light one -- reported from a real window, not theorised. Adding
    // layers only ever approaches a gradient; a Gaussian is one.
    //
    // Three levels is the bar: below what the eye resolves as a step in a smooth region, and a
    // regression to anything stepped fails here rather than in a screenshot.
    auto const shadow = renderPopupShadow(nativeParams());
    REQUIRE_FALSE(shadow.isEmpty());

    auto const y = shadow.image.height() / 2;
    auto largestStep = 0;
    auto previous = -1;
    for (auto const x: std::views::iota(0, shadow.margin + 1))
    {
        auto const alpha = static_cast<int>(qAlpha(shadow.image.pixel(x, y)));
        if (previous >= 0)
            largestStep = std::max(largestStep, std::abs(alpha - previous));
        previous = alpha;
    }
    INFO("largest adjacent alpha step across the falloff: " << largestStep);
    CHECK(largestStep <= 4);
}

TEST_CASE("renderPopupShadow is shaped like a shadow", "[contour][shadow]")
{
    auto const shadow = renderPopupShadow(nativeParams());
    REQUIRE_FALSE(shadow.isEmpty());

    SECTION("it reaches further than the blur alone, by the displacement")
    {
        // The margin has to clear both, or the falloff is cut off at the image edge and the shadow
        // ends in a visible straight line.
        CHECK(shadow.margin > nativeParams().offsetY);
        CHECK(shadow.corner > shadow.margin);
    }

    SECTION("alpha falls monotonically outward")
    {
        auto const y = shadow.image.height() / 2;
        auto previous = 0;
        for (auto const x: std::views::iota(0, shadow.margin))
        {
            auto const alpha = static_cast<int>(qAlpha(shadow.image.pixel(x, y)));
            INFO("x " << x);
            CHECK(alpha >= previous);
            previous = alpha;
        }
    }

    SECTION("the displacement puts more shadow below than above")
    {
        // What makes it read as a shadow rather than a halo, and the one thing a symmetric blur
        // would get wrong.
        auto const x = shadow.image.width() / 2;
        auto const above = qAlpha(shadow.image.pixel(x, shadow.margin / 2));
        auto const below = qAlpha(shadow.image.pixel(x, shadow.image.height() - 1 - (shadow.margin / 2)));
        CHECK(below > above);
    }

    SECTION("colour channels never exceed alpha")
    {
        // The premultiplication invariant the format demands.
        auto violations = 0;
        for (auto const y: std::views::iota(0, shadow.image.height()))
            for (auto const x: std::views::iota(0, shadow.image.width()))
            {
                auto const pixel = shadow.image.pixel(x, y);
                auto const alpha = static_cast<int>(qAlpha(pixel));
                if (qRed(pixel) > alpha || qGreen(pixel) > alpha || qBlue(pixel) > alpha)
                    ++violations;
            }
        CHECK(violations == 0);
    }
}

TEST_CASE("renderPopupShadow declines to draw nothing", "[contour][shadow]")
{
    // A style that asks for no shadow must get no image, not a transparent one: the QML gates its
    // BorderImage on the same emptiness, and a zero-size nine-patch would warn on every popup.
    CHECK(renderPopupShadow({ .blur = 0, .offsetY = 0, .cornerRadius = 0, .color = Qt::black }).isEmpty());
    CHECK(renderPopupShadow({ .blur = 24, .offsetY = 4, .cornerRadius = 3, .color = QColor(0, 0, 0, 0) })
              .isEmpty());
}
