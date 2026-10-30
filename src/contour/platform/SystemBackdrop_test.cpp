// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/platform/SystemBackdrop.hpp>
#include <contour/window/TitleBandSurface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace contour::platform;
using contour::config::BackdropKind;
using contour::config::BandBackdrop;
using contour::config::BandFill;
using contour::config::ChromeStyle;
using contour::config::MaterialSupport;

namespace
{

struct BackdropCase
{
    BandBackdrop band;
    BackdropKind kind;
    BlurBehindWish blur;
    DwmBackdrop expected;
};

constexpr auto BackdropCases = std::array {
    BackdropCase {
        BandBackdrop::NativeMaterial, BackdropKind::Tabbed, BlurBehindWish::Off, DwmBackdrop::TabbedWindow },
    BackdropCase {
        BandBackdrop::NativeMaterial, BackdropKind::Main, BlurBehindWish::Off, DwmBackdrop::MainWindow },
    BackdropCase { BandBackdrop::Client, BackdropKind::Tabbed, BlurBehindWish::Off, DwmBackdrop::None },
    BackdropCase { BandBackdrop::Client, BackdropKind::Main, BlurBehindWish::Off, DwmBackdrop::None },
    BackdropCase {
        BandBackdrop::Client, BackdropKind::Tabbed, BlurBehindWish::On, DwmBackdrop::TransientWindow },
    BackdropCase { BandBackdrop::NativeMaterial,
                   BackdropKind::Tabbed,
                   BlurBehindWish::On,
                   DwmBackdrop::TransientWindow },
};

} // namespace

TEST_CASE("backdrop.table", "[systembackdrop]")
{
    for (auto const& [band, kind, blur, expected]: BackdropCases)
    {
        CAPTURE(static_cast<int>(band), static_cast<int>(kind), static_cast<int>(blur));
        CHECK(systemBackdropFor(band, kind, blur) == expected);
    }
    // The values are DWM's, not ours to renumber.
    static_assert(static_cast<int>(DwmBackdrop::TabbedWindow) == 4);
    static_assert(static_cast<int>(DwmBackdrop::MainWindow) == 2);
    static_assert(DwmwaSystemBackdropType == 38);
}

TEST_CASE("backdrop.blurOffRestoresMica", "[systembackdrop]")
{
    // Turning `background.blur` off hands the window back to Mica Alt, not to no backdrop at all.
    CHECK(systemBackdropFor(BandBackdrop::NativeMaterial, BackdropKind::Tabbed, BlurBehindWish::On)
          == DwmBackdrop::TransientWindow);
    CHECK(systemBackdropFor(BandBackdrop::NativeMaterial, BackdropKind::Tabbed, BlurBehindWish::Off)
          == DwmBackdrop::TabbedWindow);
}

TEST_CASE("margins.clientKeepsOnePixel", "[systembackdrop]")
{
    CHECK(frameMarginsFor(BandBackdrop::Client, BlurBehindWish::Off, WindowDecoration::Client)
          == FrameMargins { .left = 0, .right = 0, .top = 1, .bottom = 0 });
    CHECK(frameMarginsFor(BandBackdrop::NativeMaterial, BlurBehindWish::Off, WindowDecoration::Client)
          == FrameMargins { .left = -1, .right = -1, .top = -1, .bottom = -1 });
    CHECK(frameMarginsFor(BandBackdrop::Client, BlurBehindWish::On, WindowDecoration::Client)
          == SheetOfGlassMargins);
}

TEST_CASE("margins.serverFrameExtendsNothing", "[systembackdrop]")
{
    // Back on the native title bar, the window extends no frame at all: the one-pixel margin is the kept
    // client frame's, and a server-framed window never had it (F32).
    CHECK(frameMarginsFor(BandBackdrop::Client, BlurBehindWish::Off, WindowDecoration::Server)
          == FrameMargins { .left = 0, .right = 0, .top = 0, .bottom = 0 });
    CHECK(frameMarginsFor(BandBackdrop::NativeMaterial, BlurBehindWish::Off, WindowDecoration::Server)
          == ServerFrameMargins);
    CHECK(frameMarginsFor(BandBackdrop::Client, BlurBehindWish::On, WindowDecoration::Server)
          == ServerFrameMargins);
}

TEST_CASE("backdrop.windows10IsSolid", "[systembackdrop]")
{
    // Below 22H2 the OS publishes no material support: the band is the solid tint over no system backdrop,
    // and the frame keeps its one-pixel margin (no black glass).
    auto const material = contour::config::resolveChromeTokens(ChromeStyle::Fluent,
                                                               {},
                                                               contour::config::ChromeDensity::Comfortable,
                                                               MaterialSupport::Unsupported)
                              .material.titleBand;
    auto const surface = contour::window::resolveTitleBandSurface(
        contour::window::TitleBandSurfaceInputs { .style = ChromeStyle::Fluent,
                                                  .material = material,
                                                  .frame = contour::window::WindowFrame::ClientBand,
                                                  .mode = contour::window::WindowMode::Windowed });
    CHECK(surface.backdrop == BandBackdrop::Client);
    CHECK(surface.fill == BandFill::Tint);
    CHECK(systemBackdropFor(surface.backdrop, BackdropKind::Tabbed, BlurBehindWish::Off)
          == DwmBackdrop::None);
    CHECK(frameMarginsFor(surface.backdrop, BlurBehindWish::Off, WindowDecoration::Client)
          == ClientFrameMargins);
}

TEST_CASE("backdrop.unsupportedIsExpectedOnlyUnderBlur", "[systembackdrop]")
{
    // Windows 10 knows no Acrylic: blur reaches its look through the accent-blur fallback, so every re-apply
    // failing as unsupported is expected there. Without blur the material asked for is gone, which is news.
    CHECK(unsupportedBackdropReportFor(BlurBehindWish::On) == UnsupportedBackdropReport::Quiet);
    CHECK(unsupportedBackdropReportFor(BlurBehindWish::Off) == UnsupportedBackdropReport::Log);
}
