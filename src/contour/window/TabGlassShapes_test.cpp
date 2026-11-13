// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabGlassShapes.hpp>

#include <QtGui/QColor>

#include <catch2/catch_test_macros.hpp>

using contour::platform::TabGlassShape;
using contour::window::tabGlassShapesOf;

namespace
{
/// One shape as the strip publishes it.
[[nodiscard]] QVariantMap published(double x, double width, QColor const& tint)
{
    return QVariantMap { { QStringLiteral("x"), x },         { QStringLiteral("y"), 6.0 },
                         { QStringLiteral("width"), width }, { QStringLiteral("height"), 28.0 },
                         { QStringLiteral("radius"), 8.0 },  { QStringLiteral("tint"), tint } };
}
} // namespace

TEST_CASE("tabGlassShapes.readsWhatTheStripPublishes", "[tabglass]")
{
    auto const shapes = tabGlassShapesOf(QVariantList { published(10.0, 120.0, QColor(0x007AFF)),
                                                        published(134.0, 120.0, QColor(Qt::transparent)) });
    REQUIRE(shapes.size() == 2);
    CHECK(shapes[0]
          == TabGlassShape { .x = 10.0,
                             .y = 6.0,
                             .width = 120.0,
                             .height = 28.0,
                             .radius = 8.0,
                             .tint = QColor(0x007AFF).rgba() });
    // A transparent tint tints nothing, whatever its colour channels say.
    CHECK(shapes[1].tint == 0);
    CHECK(shapes[1].x == 134.0);
}

TEST_CASE("tabGlassShapes.skipsWhatWouldDrawNothing", "[tabglass]")
{
    auto const shapes = tabGlassShapesOf(QVariantList {
        published(0.0, 0.0, QColor(Qt::red)), // no width
        QVariant(QStringLiteral("not a map")),
        published(40.0, 80.0, QColor(Qt::transparent)),
    });
    REQUIRE(shapes.size() == 1);
    CHECK(shapes[0].x == 40.0);
    CHECK(tabGlassShapesOf({}).empty());
}
