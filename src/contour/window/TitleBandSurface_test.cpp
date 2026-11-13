// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TitleBandSurface.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>

using namespace contour::window;
using contour::config::BandBackdrop;
using contour::config::BandFill;
using contour::config::ChromeStyle;
using contour::config::Material;
using contour::config::TitleBarGrowth;
using vtbackend::operator""_rgb;

namespace
{
[[nodiscard]] constexpr TitleBandSurface surfaceOf(ChromeStyle style,
                                                   Material material,
                                                   WindowFrame frame,
                                                   WindowMode mode) noexcept
{
    return resolveTitleBandSurface(
        TitleBandSurfaceInputs { .style = style, .material = material, .frame = frame, .mode = mode });
}

/// The composite of 1c's @c label(1) ink (88 % black on a light ground, 92 % white on a dark one) over @p
/// ground.
[[nodiscard]] vtbackend::RGBColor labelOver(vtbackend::RGBColor ground, ChromeTone tone)
{
    auto const dark = tone == ChromeTone::Dark;
    auto const alpha = dark ? 0.92 : 0.88;
    auto const ink = dark ? 255.0 : 0.0;
    auto const mix = [&](uint8_t channel) {
        return static_cast<uint8_t>(std::lround((ink * alpha) + (channel * (1 - alpha))));
    };
    return vtbackend::RGBColor { mix(ground.red), mix(ground.green), mix(ground.blue) };
}
} // namespace

TEST_CASE("surface.macRows", "[titlebandsurface]")
{
    using contour::config::BackdropKind;
    using contour::config::TabSurface;

    // Frosted is the title bar's own material in a view of ours behind the content -- never AppKit's
    // title-bar material, whose NSTitlebarView takes the band's clicks (F13) -- with nothing painted over it,
    // as Apple asks of bars.
    CHECK(surfaceOf(ChromeStyle::MacOS, Material::Frost, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface { .backdrop = BandBackdrop::Blurred,
                                .fill = BandFill::Clear,
                                .growth = TitleBarGrowth::CompactToolbar });
    // Glass is on the tabs, not across the band: the band is the terminal's own background, and the system
    // draws each tab as Liquid Glass.
    CHECK(surfaceOf(ChromeStyle::MacOS, Material::Glass, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface { .backdrop = BandBackdrop::Client,
                                .fill = BandFill::Background,
                                .growth = TitleBarGrowth::CompactToolbar,
                                .kind = BackdropKind::TabbedGlass,
                                .tabs = TabSurface::Glass });
    // In full screen neither: no material behind terminal text, and the tabs painted again.
    auto const fullGlass =
        surfaceOf(ChromeStyle::MacOS, Material::Glass, WindowFrame::ClientBand, WindowMode::FullScreen);
    CHECK(fullGlass.tabs == TabSurface::Painted);
    CHECK(fullGlass.growth == TitleBarGrowth::None);
    // No macOS row selects the native material any more, and only the glass band has glass tabs.
    for (auto const& row: TitleBandSurfaceRows)
    {
        if (row.style == ChromeStyle::MacOS)
            CHECK(row.surface.backdrop != BandBackdrop::NativeMaterial);
        CHECK((row.surface.tabs == TabSurface::Glass)
              == (row.style == ChromeStyle::MacOS && row.material == Material::Glass));
    }
    CHECK(surfaceOf(ChromeStyle::MacOS, Material::Solid, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface { .backdrop = BandBackdrop::Client,
                                .fill = BandFill::Tint,
                                .growth = TitleBarGrowth::CompactToolbar });
}

TEST_CASE("surface.fluentRows", "[titlebandsurface]")
{
    // Mica: the band is cleared over the native backdrop; Solid (no Mica, or Reduced transparency): tinted.
    CHECK(surfaceOf(ChromeStyle::Fluent, Material::Mica, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface { .backdrop = BandBackdrop::NativeMaterial,
                                .fill = BandFill::Clear,
                                .growth = TitleBarGrowth::None });
    CHECK(surfaceOf(ChromeStyle::Fluent, Material::Solid, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface {
              .backdrop = BandBackdrop::Client, .fill = BandFill::Tint, .growth = TitleBarGrowth::None });
    // No material behind terminal text in full screen.
    CHECK(surfaceOf(ChromeStyle::Fluent, Material::Mica, WindowFrame::ClientBand, WindowMode::FullScreen)
          == TitleBandSurface {
              .backdrop = BandBackdrop::Client, .fill = BandFill::Tint, .growth = TitleBarGrowth::None });
    CHECK(surfaceOf(ChromeStyle::Fluent, Material::Mica, WindowFrame::NativeFrame, WindowMode::Windowed)
          == TitleBandSurface {});
}

TEST_CASE("surface.fluentAcrylicIsMicasBandOverAnotherBackdrop", "[titlebandsurface]")
{
    using contour::config::BackdropKind;

    // Acrylic (tab_style.material) clears the band over the native backdrop as Mica does, and differs only
    // in which backdrop the window asks the compositor for.
    auto const acrylic =
        surfaceOf(ChromeStyle::Fluent, Material::Acrylic, WindowFrame::ClientBand, WindowMode::Windowed);
    auto const mica =
        surfaceOf(ChromeStyle::Fluent, Material::Mica, WindowFrame::ClientBand, WindowMode::Windowed);
    CHECK(acrylic.backdrop == BandBackdrop::NativeMaterial);
    CHECK(acrylic.fill == BandFill::Clear);
    CHECK(acrylic.growth == mica.growth);
    CHECK(acrylic.kind == BackdropKind::TabbedGlass);
    CHECK(mica.kind == BackdropKind::Tabbed);

    // No material behind terminal text in full screen, whichever was asked for.
    auto const fullScreen =
        surfaceOf(ChromeStyle::Fluent, Material::Acrylic, WindowFrame::ClientBand, WindowMode::FullScreen);
    CHECK(fullScreen.backdrop == BandBackdrop::Client);
    CHECK(fullScreen.fill == BandFill::Tint);

    // A row with no compositor material has no Acrylic row, so it keeps today's band.
    CHECK(surfaceOf(ChromeStyle::MacOS, Material::Acrylic, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface {});
}

TEST_CASE("surface.otherRowsKeepToday", "[titlebandsurface]")
{
    for (auto const style: std::views::iota(size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        // The rows with a band material of their own are tested above; the Terminal row has none.
        if (static_cast<ChromeStyle>(style) == ChromeStyle::MacOS
            || static_cast<ChromeStyle>(style) == ChromeStyle::ContourLinux
            || static_cast<ChromeStyle>(style) == ChromeStyle::Fluent)
            continue;
        for (auto const material: std::views::iota(size_t { 0 }, contour::config::MaterialCount))
            for (auto const frame: std::views::iota(size_t { 0 }, WindowFrameCount))
                for (auto const mode: std::views::iota(size_t { 0 }, WindowModeCount))
                {
                    CAPTURE(style, material, frame, mode);
                    CHECK(surfaceOf(static_cast<ChromeStyle>(style),
                                    static_cast<Material>(material),
                                    static_cast<WindowFrame>(frame),
                                    static_cast<WindowMode>(mode))
                          == TitleBandSurface {});
                }
    }
}

TEST_CASE("surface.linuxRowsBlurBehindATranslucentBand", "[titlebandsurface]")
{
    using contour::config::BackdropKind;

    // KDE Plasma's frosted and glass bands: KWin's blur under the two tints, with no title bar to grow.
    CHECK(surfaceOf(ChromeStyle::ContourLinux, Material::Frost, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface { .backdrop = BandBackdrop::Blurred,
                                .fill = BandFill::FrostTint,
                                .growth = TitleBarGrowth::None });
    CHECK(surfaceOf(ChromeStyle::ContourLinux, Material::Glass, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface { .backdrop = BandBackdrop::Blurred,
                                .fill = BandFill::GlassTint,
                                .growth = TitleBarGrowth::None,
                                .kind = BackdropKind::TabbedGlass });
    // The solid band stays today's: no row, the palette's own paint.
    CHECK(surfaceOf(ChromeStyle::ContourLinux, Material::Solid, WindowFrame::ClientBand, WindowMode::Windowed)
          == TitleBandSurface {});

    // No blur behind terminal text in full screen: the client's own tint, opaque.
    for (auto const material: { Material::Frost, Material::Glass })
    {
        auto const full =
            surfaceOf(ChromeStyle::ContourLinux, material, WindowFrame::ClientBand, WindowMode::FullScreen);
        CHECK(full.backdrop == BandBackdrop::Client);
        CHECK(full.fill == BandFill::Tint);
    }

    // The tints are the only translucent fills, and glass is the more see-through of the two.
    using contour::config::bandFillOpacityPercent;
    CHECK(bandFillOpacityPercent(BandFill::GlassTint) < bandFillOpacityPercent(BandFill::FrostTint));
    CHECK(bandFillOpacityPercent(BandFill::FrostTint) < 100);
    for (auto const fill: { BandFill::Palette, BandFill::Clear, BandFill::Tint, BandFill::Background })
        CHECK(bandFillOpacityPercent(fill) == 100);
}

TEST_CASE("surface.nativeFrameHasNoBand", "[titlebandsurface]")
{
    CHECK(surfaceOf(ChromeStyle::MacOS, Material::Glass, WindowFrame::NativeFrame, WindowMode::Windowed)
          == TitleBandSurface {});
}

TEST_CASE("surface.fullScreenIsSolid", "[titlebandsurface]")
{
    auto const windowed =
        surfaceOf(ChromeStyle::MacOS, Material::Frost, WindowFrame::ClientBand, WindowMode::Windowed);
    auto const full =
        surfaceOf(ChromeStyle::MacOS, Material::Frost, WindowFrame::ClientBand, WindowMode::FullScreen);
    CHECK(full.backdrop == BandBackdrop::Client);
    CHECK(full.fill == BandFill::Tint);
    CHECK(windowed.growth == TitleBarGrowth::CompactToolbar);
    // Ruling S7: full screen detaches the compact toolbar, so AppKit puts no NSToolbarFullScreenWindow strip
    // over the band and the top rows to take their clicks.
    CHECK(full.growth == TitleBarGrowth::None);
    CHECK(
        surfaceOf(ChromeStyle::MacOS, Material::Solid, WindowFrame::ClientBand, WindowMode::FullScreen).growth
        == TitleBarGrowth::None);
}

TEST_CASE("surface.minimalBareBandIsTheBackground", "[titlebandsurface]")
{
    using contour::config::ChromeDensity;
    auto const bare = [](BarCentre centre, WindowMode mode, ChromeStyle style = ChromeStyle::MacOS) {
        return resolveTitleBandSurface(TitleBandSurfaceInputs { .style = style,
                                                                .material = Material::Solid,
                                                                .frame = WindowFrame::ClientBand,
                                                                .mode = mode,
                                                                .centre = centre,
                                                                .density = ChromeDensity::Minimal });
    };
    auto const comfortable =
        surfaceOf(ChromeStyle::MacOS, Material::Solid, WindowFrame::ClientBand, WindowMode::Windowed);

    // The bare band: the client's own paint in the terminal background, over the bare title bar (ruling P3:
    // no compact toolbar, so AppKit's title bar is 32 pt).
    auto const windowed = bare(BarCentre::Empty, WindowMode::Windowed);
    CHECK(windowed.backdrop == BandBackdrop::Client);
    CHECK(windowed.fill == BandFill::Background);
    CHECK(windowed.growth == TitleBarGrowth::None);
    CHECK(windowed == BareBandSurface);

    // With tabs (or a title) in the middle, Minimal's band is the Comfortable one.
    CHECK(bare(BarCentre::Tabs, WindowMode::Windowed) == comfortable);
    CHECK(bare(BarCentre::Title, WindowMode::Windowed) == comfortable);

    // Full screen keeps the client's paint and the terminal background.
    auto const full = bare(BarCentre::Empty, WindowMode::FullScreen);
    CHECK(full.backdrop == BandBackdrop::Client);
    CHECK(full.fill == BandFill::Background);

    // The Fluent row's bare band is the same; a row the table does not name keeps today's band, and a
    // native frame has none.
    CHECK(bare(BarCentre::Empty, WindowMode::Windowed, ChromeStyle::Fluent) == BareBandSurface);
    CHECK(bare(BarCentre::Empty, WindowMode::Windowed, ChromeStyle::ContourLinux) == TitleBandSurface {});
    CHECK(resolveTitleBandSurface(TitleBandSurfaceInputs { .style = ChromeStyle::MacOS,
                                                           .material = Material::Solid,
                                                           .frame = WindowFrame::NativeFrame,
                                                           .mode = WindowMode::Windowed,
                                                           .centre = BarCentre::Empty,
                                                           .density = ChromeDensity::Minimal })
          == TitleBandSurface {});
}

TEST_CASE("surface.rowsAreUnique", "[titlebandsurface]")
{
    for (auto const i: std::views::iota(size_t { 0 }, TitleBandSurfaceRows.size()))
        for (auto const j: std::views::iota(i + 1, TitleBandSurfaceRows.size()))
        {
            CAPTURE(i, j);
            CHECK((TitleBandSurfaceRows[i].style != TitleBandSurfaceRows[j].style
                   || TitleBandSurfaceRows[i].material != TitleBandSurfaceRows[j].material));
        }
}

TEST_CASE("tint.values", "[titlebandsurface]")
{
    CHECK(bandTintOf(0x000000_rgb) == 0x0A0A0A_rgb);
    CHECK(bandTintOf(0xFFFFFF_rgb) == 0xF4F4F4_rgb); // hslToRgb truncates 244.8
    CHECK(bandTintOf(0x1E1E1E_rgb) == 0x282828_rgb);

    for (auto const name: vtbackend::defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = vtbackend::ColorPalette {};
        REQUIRE(vtbackend::defaultColorPalettes(name, palette));
        auto const background = palette.defaultBackground;
        auto const tint = bandTintOf(background);
        CHECK(toneOf(tint) == toneOf(background));
        CHECK(tint != background);
    }
}

TEST_CASE("background.legibility", "[titlebandsurface]")
{
    // The bare band paints the terminal background only where it has the chrome's tone.
    CHECK(legibleBandBackground(0x1E1E1E_rgb, ChromeTone::Light) == std::nullopt);
    CHECK(legibleBandBackground(0x1E1E1E_rgb, ChromeTone::Dark) == 0x1E1E1E_rgb);
    CHECK(legibleBandBackground(0xFAFAFA_rgb, ChromeTone::Light) == 0xFAFAFA_rgb);
    CHECK(legibleBandBackground(0xFAFAFA_rgb, ChromeTone::Dark) == std::nullopt);
}

TEST_CASE("tint.legibility", "[titlebandsurface]")
{
    CHECK(legibleBandTint(0x1E1E1E_rgb, ChromeTone::Light) == std::nullopt);
    CHECK(legibleBandTint(0xFAFAFA_rgb, ChromeTone::Light) == bandTintOf(0xFAFAFA_rgb));

    for (auto const name: vtbackend::defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = vtbackend::ColorPalette {};
        REQUIRE(vtbackend::defaultColorPalettes(name, palette));
        for (auto const tone: std::views::iota(size_t { 0 }, ChromeToneCount))
        {
            CAPTURE(tone);
            auto const chrome = static_cast<ChromeTone>(tone);
            auto const tint = legibleBandTint(palette.defaultBackground, chrome);
            if (!tint)
                continue;
            CHECK(vtbackend::contrastRatio(*tint, labelOver(*tint, chrome)) >= 4.5);
        }
    }
}
