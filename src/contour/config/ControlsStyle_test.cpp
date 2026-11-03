// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ControlsStyle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

using contour::config::chooseControlsStyle;
using contour::config::ControlsStyleOrigin;

TEST_CASE("chooseControlsStyle keeps an installed style", "[controlsstyle]")
{
    auto const choice = chooseControlsStyle("ContourTui", "Fusion", [](std::string_view) { return true; });
    CHECK(choice.style == "ContourTui");
    CHECK(choice.origin == ControlsStyleOrigin::Requested);
}

TEST_CASE("chooseControlsStyle falls back when the style is missing on this platform", "[controlsstyle]")
{
    // A style the pinned row names but this Qt build does not ship (FluentWinUI3 on an older Qt) must not
    // reach QQuickStyle::setStyle(): its module would fail the whole QML load.
    auto const choice =
        chooseControlsStyle("Windows", "Fusion", [](std::string_view style) { return style == "Fusion"; });
    CHECK(choice.style == "Fusion");
    CHECK(choice.origin == ControlsStyleOrigin::FellBackToDefault);
}

TEST_CASE("controlsStyleQmldirs names a style of Qt's own, then a module of the style's name",
          "[controlsstyle]")
{
    CHECK(contour::config::controlsStyleQmldirs("FluentWinUI3")
          == std::array<std::string, 2> { "QtQuick/Controls/FluentWinUI3/qmldir", "FluentWinUI3/qmldir" });
    CHECK(contour::config::controlsStyleQmldirs("ContourTui")
          == std::array<std::string, 2> { "QtQuick/Controls/ContourTui/qmldir", "ContourTui/qmldir" });
}
