// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TitleBandGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace contour::window;
using contour::config::ChromeStyle;
using contour::config::Material;

namespace
{
constexpr auto Mac = TitleBandTokens {
    .style = ChromeStyle::MacOS, .material = Material::Glass, .height = 38, .cellHeight = 0
};
constexpr auto Tui = TitleBandTokens {
    .style = ChromeStyle::Terminal, .material = Material::None, .height = 17, .cellHeight = 17
};
constexpr auto Flat34 = TitleBandTokens {
    .style = ChromeStyle::ContourLinux, .material = Material::Solid, .height = 34, .cellHeight = 0
};

[[nodiscard]] TitleBandGeometry geometryOf(
    TitleBandTokens tokens, WindowMode mode, double dpr, double nativeHeight, double fullScreenTop = 0)
{
    return resolveTitleBandGeometry(TitleBandGeometryInputs { .tokens = tokens,
                                                              .mode = mode,
                                                              .devicePixelRatio = dpr,
                                                              .nativeHeight = nativeHeight,
                                                              .fullScreenTop = fullScreenTop });
}
} // namespace

TEST_CASE("geometry.nativeRows", "[titlebandgeometry]")
{
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 28).height == 38);
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 52).height == 52);
    // No native title bar: the token, as off macOS.
    CHECK(geometryOf(Mac, WindowMode::Windowed, 1, 0).height == 38);
}

TEST_CASE("geometry.devicePixelSnap", "[titlebandgeometry]")
{
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 38.25).height == 38.5);
    CHECK(geometryOf(Mac, WindowMode::Windowed, 1, 38.25).height == 39);
    for (auto const dpr: { 1.0, 2.0 })
        for (auto const native: { 0.0, 27.5, 28.0, 32.25, 38.0, 38.25, 52.0 })
        {
            CAPTURE(dpr, native);
            auto const scaled = geometryOf(Mac, WindowMode::Windowed, dpr, native).height * dpr;
            CHECK(scaled == std::floor(scaled));
        }
    // Without a native title bar nothing is snapped, so a Flat band keeps its extent at a fractional scale.
    CHECK(geometryOf(Flat34, WindowMode::Windowed, 1.25, 0).height == 34);
}

TEST_CASE("geometry.revealTop", "[titlebandgeometry]")
{
    CHECK(geometryOf(Mac, WindowMode::FullScreen, 2, 28, 24).revealTop == 24);
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 28, 24).revealTop == 0);
    // Window-relative, never above the window's own top.
    CHECK(geometryOf(Mac, WindowMode::FullScreen, 2, 28, -5).revealTop == 0);
}

TEST_CASE("geometry.terminalRow", "[titlebandgeometry]")
{
    static_assert(TerminalRowTrafficLightFit == TrafficLightFit::GrowBandToCells);
    CHECK(geometryOf(Tui, WindowMode::Windowed, 2, 28) == TitleBandGeometry { .height = 34, .revealTop = 0 });
    CHECK(geometryOf(Tui, WindowMode::Windowed, 2, 0).height == 17);
}
