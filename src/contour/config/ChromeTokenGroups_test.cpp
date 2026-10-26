// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <vector>

using namespace vtbackend;

namespace
{
constexpr auto AllStyles()
{
    return std::views::iota(std::size_t { 0 }, contour::config::detail::ChromeStyleCount);
}
} // namespace

TEST_CASE("chromeTokens.componentTokens", "[chrometokens]")
{
    using namespace contour::config;
    using enum ChromeStyle;
    auto const mac = resolveChromeTokens(MacOS, {});
    CHECK(mac.size.toast == 36);
    CHECK(mac.radii.toast == 18);
    CHECK(mac.motion.fastMs == 100);
    CHECK(mac.motion.standardMs == 160);
    CHECK(mac.material.titleBand == Material::Glass);
    CHECK(mac.layout.dialog == DialogPlacement::Sheet);
    auto const tui = resolveChromeTokens(Terminal, {});
    CHECK(tui.size.toast == 1);
    CHECK(tui.radii.toast == 0);
    CHECK(tui.material.popover == Material::None);
    CHECK(tui.tui.inactivePaneOpacity == Catch::Approx(0.62));
    CHECK(tui.tui.toastStyle == ToastStyle::Inverse);
    CHECK(tui.elevation.levels[2].offsetX == 1);
    CHECK(tui.elevation.levels[2].blur == 0);
    CHECK(tui.colors[static_cast<size_t>(ChromeColorRole::BannerFill)].slot == SchemeSlot::Bar);
}

namespace
{
/// Every numeric field of a row, in declaration order, so a whole row compares against one list.
std::vector<double> numbersOf(contour::config::ChromeTokenRow const& row)
{
    auto out = std::vector<double> {};
    auto const append = [&out](std::initializer_list<double> values) {
        out.insert(out.end(), values.begin(), values.end());
    };
    auto const& size = row.size;
    append({ double(size.titleBar),
             double(size.tabWell),
             double(size.tab),
             double(size.control),
             double(size.paneHeader),
             double(size.toast),
             double(size.dialogWidth),
             double(size.swatchWidth),
             double(size.swatchHeight),
             double(size.settingsWidth),
             double(size.settingsHeight),
             double(size.settingsMinWidth),
             double(size.settingsMinHeight) });
    append({ double(size.tabIcon),
             double(size.tabClose),
             double(size.badge),
             double(size.badgeDot),
             double(size.progressBar),
             double(size.hostChip),
             double(size.hostDot),
             double(size.toolbarButton),
             double(size.toolbarChip),
             double(size.flyoutSwatch) });
    append({ double(size.bareBand),
             double(size.grabber),
             double(size.grabberLength),
             double(size.paneHeaderAction),
             double(size.paneFooter),
             double(size.footerButton) });
    append({ double(row.space.paneGap), double(row.space.splitHit), double(row.space.paneInset) });
    for (auto const v: row.space.scale)
        out.push_back(v);
    append({ double(row.space.dragReserve),
             double(row.space.revealEdge),
             double(row.space.toolbarChipPadding) });
    append(
        { double(row.space.paneGapRows), double(row.space.paneHeaderGap), double(row.space.footerLeading) });
    auto const& radii = row.radii;
    append({ double(radii.control),
             double(radii.popover),
             double(radii.menu),
             double(radii.paneCard),
             double(radii.toast) });
    append({ double(radii.tab),
             double(radii.tabWell),
             double(radii.tabIcon),
             double(radii.tabClose),
             double(radii.badge),
             double(radii.hostChip),
             double(radii.toolbarChip) });
    out.push_back(radii.grabber);
    append({ row.type.caption, row.type.footnote, row.type.body, row.type.tab, row.type.title });
    append({ row.type.paneHeader, double(row.type.paneHeaderWeight) });
    append({ double(row.motion.fastMs),
             double(row.motion.standardMs),
             double(row.motion.emphasizedMs),
             double(row.motion.travel),
             double(row.motion.spinnerMs),
             double(row.motion.pulseMs),
             double(row.motion.headerRevealMs) });
    for (auto const& level: row.elevation.levels)
        append({ double(level.offsetX),
                 double(level.offsetY),
                 double(level.blur),
                 double(level.opacityPercent) });
    out.push_back(row.contrast.separatorPercent);
    out.push_back(row.contrast.activeCardRing);
    out.push_back(row.contrast.paneDimPercent.value_or(-1));
    out.push_back(row.tui.inactivePaneOpacity);
    out.push_back(row.tint.tabPercent);
    append({ double(row.dim.darkPercent), double(row.dim.lightPercent) });
    auto const& minimal = row.minimal;
    append({ double(minimal.paneHeader),
             double(minimal.paneGap),
             double(minimal.paneGapRows),
             double(minimal.splitHit),
             double(minimal.paneInset),
             double(minimal.paneCardRadius) });
    return out;
}

/// @return The concatenation of @p parts.
std::vector<double> joined(std::initializer_list<std::vector<double>> parts)
{
    auto out = std::vector<double> {};
    for (auto const& part: parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

struct ExpectedRow
{
    contour::config::ChromeStyle style = contour::config::ChromeStyle::MacOS;
    std::vector<double> numbers {};
    contour::config::Material titleBand = contour::config::Material::None;
    contour::config::Material popover = contour::config::Material::None;
    contour::config::ToastStyle toast = contour::config::ToastStyle::Floating;
    contour::config::DialogPlacement dialog = contour::config::DialogPlacement::Centered;
    contour::config::MenuPresentation menu = contour::config::MenuPresentation::InScene;
    contour::config::TabShape tabShape = contour::config::TabShape::Flat;
    contour::config::ProfileMenuAffordance profileMenu = contour::config::ProfileMenuAffordance::Caret;
    contour::config::TabColorSource tabColors = contour::config::TabColorSource::Palette;
    contour::config::ActiveTabWeight activeTabWeight = contour::config::ActiveTabWeight::Regular;
    contour::config::PaneActionsReveal paneActions = contour::config::PaneActionsReveal::OnHover;
};
} // namespace

TEST_CASE("chromeTokens.everyRowMatchesTheDesignKit", "[chrometokens]")
{
    using namespace contour::config;
    // size(13+10+6) space(3) scale(10) space(3+3) radii(5+7+1) type(5+2) motion(7) elevation e0..e3 (16)
    // separator ring dim opacity tint, dim(2) minimal(6)
    auto const native = std::vector<double> { 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 };
    auto const nativeBand = std::vector<double> { 40, 2, 9 };
    // tabIcon tabClose badge hostChip toolbarChip grabber
    auto const nativeRadii = std::vector<double> { 3, 8, 7, 12, 11, 1 };
    auto const nativeMotion = std::vector<double> { 100, 160, 220, 8, 1200, 700, 500 };
    // bareBand grabber grabberLength paneHeaderAction paneFooter footerButton, after the band's sizes
    auto const nativePaneSizes = std::vector<double> { 2, 32, 22, 34, 24 };
    // ... e3, separator ring dim opacity tint, pane.dim 15/10, Minimal { 0, 1, 1, 8, 0, 0 }
    auto const nativeTail = std::vector<double> { 0,  0,  0, 0,  0,   1,  2,  12, 0, 4, 16, 25, 0, 12, 40,
                                                  35, 10, 0, -1, 1.0, 20, 15, 10, 0, 1, 1,  8,  0, 0 };
    auto const rows = std::vector<ExpectedRow> {
        { ChromeStyle::MacOS,
          joined({ { 38, 28, 24, 28, 22, 36, 280, 16, 16, 960, 820, 720, 480 },
                   { 14, 16, 14, 6, 2, 24, 7, 28, 22, 30 },
                   { 32 },
                   nativePaneSizes,
                   { 6, 10, 6 },
                   native,
                   nativeBand,
                   { 6, 6, 14 },
                   { 6, 14, 10, 10, 18, 12, 14 },
                   nativeRadii,
                   { 10, 11, 13, 12, 15, 11, 500 },
                   nativeMotion,
                   nativeTail }),
          Material::Glass,
          Material::Glass,
          ToastStyle::Floating,
          DialogPlacement::Sheet,
          MenuPresentation::Native,
          TabShape::Pill,
          ProfileMenuAffordance::SecondaryClick,
          TabColorSource::Roles,
          ActiveTabWeight::Regular },
        { ChromeStyle::Fluent,
          joined({ { 40, 0, 34, 32, 28, 36, 280, 16, 16, 1110, 920, 720, 480 },
                   { 14, 16, 14, 6, 2, 24, 7, 32, 22, 30 },
                   { 28 },
                   nativePaneSizes,
                   { 4, 10, 4 },
                   native,
                   nativeBand,
                   { 4, 8, 14 },
                   { 4, 8, 8, 4, 18, 6, 0 },
                   nativeRadii,
                   { 12, 12, 14, 12, 20, 12, 600 },
                   nativeMotion,
                   nativeTail }),
          Material::Mica,
          Material::Solid,
          ToastStyle::Floating,
          DialogPlacement::Centered,
          MenuPresentation::InScene,
          TabShape::Flat,
          ProfileMenuAffordance::Caret,
          TabColorSource::Palette,
          ActiveTabWeight::Regular },
        { ChromeStyle::ContourLinux,
          joined({ { 46, 0, 34, 34, 28, 36, 280, 16, 16, 1100, 920, 720, 480 },
                   { 14, 16, 14, 6, 2, 24, 7, 34, 22, 30 },
                   { 28 },
                   nativePaneSizes,
                   { 6, 10, 6 },
                   native,
                   nativeBand,
                   { 6, 8, 14 },
                   { 8, 12, 10, 6, 18, 8, 0 },
                   nativeRadii,
                   { 12, 12.5, 14, 13.5, 15, 12.5, 600 },
                   nativeMotion,
                   nativeTail }),
          Material::Solid,
          Material::Solid,
          ToastStyle::Floating,
          DialogPlacement::Centered,
          MenuPresentation::InScene,
          TabShape::Flat,
          ProfileMenuAffordance::Caret,
          TabColorSource::Palette,
          ActiveTabWeight::Regular },
        { ChromeStyle::Terminal,
          joined({ { 1, 1, 1, 2, 1, 1, 32, 2, 1, 120, 40, 90, 24 },
                   { 1, 1, 1, 1, 1, 1, 1, 2, 1, 2 },
                   { 1, 0, 0, 1, 1, 1 },
                   { 1, 1, 0 },
                   { 0, 0, 0, 1, 1, 1, 1, 1, 2, 3 },
                   { 4, 1, 1 },
                   { 1, 1, 1 },
                   { 0, 0, 0, 0, 0, 0, 0 },
                   { 0, 0, 0, 0, 0, 0 },
                   { 0, 0, 0, 0, 0, 0, 400 },
                   { 100, 160, 220, 0, 1200, 700, 500 },
                   { 0, 0, 0, 0, 1, 1, 0, 60, 1, 1, 0, 60, 1, 1, 0, 60, 10, 0.0, -1, 0.62, 0 },
                   { 15, 10, 0, 1, 1, 1, 0, 0 } }),
          Material::None,
          Material::None,
          ToastStyle::Inverse,
          DialogPlacement::Centered,
          MenuPresentation::InScene,
          TabShape::Flat,
          ProfileMenuAffordance::Caret,
          TabColorSource::Roles,
          ActiveTabWeight::Bold,
          PaneActionsReveal::OnHoverOrFocus },
    };

    for (auto const& expected: rows)
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const row = resolveChromeTokens(expected.style, {});
        auto const actual = numbersOf(row);
        REQUIRE(actual.size() == expected.numbers.size());
        for (auto const i: std::views::iota(std::size_t { 0 }, actual.size()))
        {
            CAPTURE(i);
            CHECK(actual[i] == Catch::Approx(expected.numbers[i]));
        }
        CHECK(row.material.titleBand == expected.titleBand);
        CHECK(row.material.popover == expected.popover);
        CHECK(row.tui.toastStyle == expected.toast);
        CHECK(row.layout.dialog == expected.dialog);
        CHECK(row.layout.menu == expected.menu);
        CHECK(row.layout.tabShape == expected.tabShape);
        CHECK(row.layout.profileMenu == expected.profileMenu);
        CHECK(row.layout.tabColors == expected.tabColors);
        CHECK(row.layout.activeTabWeight == expected.activeTabWeight);
        CHECK(row.layout.paneActions == expected.paneActions);

        auto const& fill = row.colors[static_cast<size_t>(ChromeColorRole::BannerFill)];
        auto const& text = row.colors[static_cast<size_t>(ChromeColorRole::BannerText)];
        auto const& glyph = row.colors[static_cast<size_t>(ChromeColorRole::BannerGlyph)];
        auto const& toastFill = row.colors[static_cast<size_t>(ChromeColorRole::ToastFill)];
        auto const& toastText = row.colors[static_cast<size_t>(ChromeColorRole::ToastText)];
        auto const& swatchRing = row.colors[static_cast<size_t>(ChromeColorRole::SwatchRing)];
        if (expected.style == ChromeStyle::Terminal)
        {
            CHECK(fill.kind == ColorSourceKind::Scheme);
            CHECK(fill.slot == SchemeSlot::Bar);
            CHECK(text.kind == ColorSourceKind::Scheme);
            CHECK(text.slot == SchemeSlot::BarInk);
            CHECK(glyph.kind == ColorSourceKind::Scheme);
            CHECK(glyph.slot == SchemeSlot::Warning);
            CHECK(toastFill.kind == ColorSourceKind::Scheme);
            CHECK(toastFill.slot == SchemeSlot::Foreground);
            CHECK(toastText.kind == ColorSourceKind::Scheme);
            CHECK(toastText.slot == SchemeSlot::Background);
            // The scheme's own foreground, not Qt's palette: the ring sits on scheme-coloured swatches.
            CHECK(swatchRing.kind == ColorSourceKind::Scheme);
            CHECK(swatchRing.slot == SchemeSlot::Foreground);
        }
        else
        {
            CHECK(fill.kind == ColorSourceKind::PaletteWindow);
            CHECK(text.kind == ColorSourceKind::PaletteWindowText);
            CHECK(glyph.kind == ColorSourceKind::Status);
            CHECK(glyph.status == StatusColorRole::Warning);
            CHECK(toastFill.kind == ColorSourceKind::Fixed);
            CHECK(toastFill.rgb == 0x28282C);
            CHECK(toastFill.alphaPercent == 92);
            CHECK(toastText.kind == ColorSourceKind::Fixed);
            CHECK(toastText.rgb == 0xFFFFFF);
            CHECK(toastText.alphaPercent == 100);
            CHECK(swatchRing.kind == ColorSourceKind::PaletteWindowText);
        }
    }
}

TEST_CASE("chromeTokens.allOverridesTogether", "[chrometokens]")
{
    using namespace contour::config;
    auto const preferences = AccessibilityPreferences { .motion = MotionPreference::Reduced,
                                                        .transparency = TransparencyPreference::Reduced,
                                                        .contrast = ContrastPreference::Increased };
    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        auto const base = resolveChromeTokens(style, {});
        auto const tokens = resolveChromeTokens(style, preferences);
        CHECK(tokens.motion.fastMs == ReducedMotionCrossFadeMs);
        CHECK(tokens.motion.travel == 0);
        CHECK(tokens.material.titleBand != Material::Glass);
        CHECK(tokens.material.titleBand != Material::Mica);
        CHECK(tokens.contrast.separatorPercent == 2 * base.contrast.separatorPercent);
        CHECK(tokens.contrast.activeCardRing == Catch::Approx(1.5));
        CHECK(tokens.contrast.paneDimPercent == 5);
        CHECK(tokens.size == base.size);
        CHECK(tokens.colors == base.colors);
        CHECK(tokens.tint == base.tint);
        CHECK(tokens.statusInk == base.statusInk);
    }
}

TEST_CASE("chromeTokens.reducedMotion", "[chrometokens]")
{
    using namespace contour::config;
    auto const preferences = AccessibilityPreferences { .motion = MotionPreference::Reduced,
                                                        .transparency = TransparencyPreference::Allowed,
                                                        .contrast = ContrastPreference::Standard };
    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        auto const base = resolveChromeTokens(style, {});
        auto const reduced = resolveChromeTokens(style, preferences);
        CHECK(reduced.motion.fastMs == ReducedMotionCrossFadeMs);
        CHECK(reduced.motion.standardMs == ReducedMotionCrossFadeMs);
        CHECK(reduced.motion.emphasizedMs == ReducedMotionCrossFadeMs);
        CHECK(reduced.motion.travel == 0);
        CHECK(reduced.size == base.size);
        CHECK(reduced.space == base.space);
        CHECK(reduced.radii == base.radii);
        CHECK(reduced.type == base.type);
        CHECK(reduced.elevation == base.elevation);
        CHECK(reduced.material == base.material);
        CHECK(reduced.contrast == base.contrast);
        CHECK(reduced.tui == base.tui);
        CHECK(reduced.layout == base.layout);
        CHECK(reduced.colors == base.colors);
        CHECK(reduced.tint == base.tint);
        CHECK(reduced.statusInk == base.statusInk);
    }
}

TEST_CASE("chromeTokens.reducedTransparency", "[chrometokens]")
{
    using namespace contour::config;
    auto const preferences = AccessibilityPreferences { .motion = MotionPreference::Full,
                                                        .transparency = TransparencyPreference::Reduced,
                                                        .contrast = ContrastPreference::Standard };
    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        auto const tokens = resolveChromeTokens(style, preferences);
        for (auto const material: { tokens.material.titleBand, tokens.material.popover })
        {
            CHECK(material != Material::Glass);
            CHECK(material != Material::Mica);
        }
    }
    auto const tui = resolveChromeTokens(ChromeStyle::Terminal, preferences);
    CHECK(tui.material.titleBand == Material::None);
    CHECK(tui.material.popover == Material::None);
}

TEST_CASE("chromeTokens.increasedContrast", "[chrometokens]")
{
    using namespace contour::config;
    auto const preferences = AccessibilityPreferences { .motion = MotionPreference::Full,
                                                        .transparency = TransparencyPreference::Allowed,
                                                        .contrast = ContrastPreference::Increased };
    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        auto const base = resolveChromeTokens(style, {});
        auto const tokens = resolveChromeTokens(style, preferences);
        CHECK(tokens.contrast.separatorPercent == 2 * base.contrast.separatorPercent);
        CHECK(tokens.contrast.activeCardRing == Catch::Approx(1.5));
        REQUIRE(tokens.contrast.paneDimPercent.has_value());
        CHECK(*tokens.contrast.paneDimPercent == 5);
    }
}

TEST_CASE("chromeTokens.statusColoursAreLargeTextSafe", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const& row: StatusColors)
    {
        if (row.source != StatusColorSource::Fixed)
            continue;
        for (auto const use: { StatusInkUse::Text, StatusInkUse::Mark })
        {
            CHECK(contrastRatio(RGBColor { row.hues(use).light }, 0xFFFFFF_rgb) >= 3.0);
            CHECK(contrastRatio(RGBColor { row.hues(use).dark }, 0x1C1C1E_rgb) >= 3.0);
        }
    }
}

TEST_CASE("chromeTokens.schemeColours", "[chrometokens]")
{
    using namespace contour::config;
    auto palette = ColorPalette {};
    palette.defaultBackground = 0x010101_rgb;
    palette.defaultForeground = 0x020202_rgb;
    palette.palette[0] = 0x030303_rgb;
    palette.palette[8] = 0x040404_rgb;
    palette.palette[1] = 0x050505_rgb;
    palette.palette[2] = 0x060606_rgb;
    palette.palette[3] = 0x070707_rgb;
    palette.palette[4] = 0x080808_rgb;
    // Light, so the chrome keeps it: either end of this dark scheme reads on it.
    palette.selection.background = 0xF9F9F9_rgb;

    auto const slotOf = [](ChromeSchemeColors const& colors, SchemeSlot slot) {
        return colors.bySlot[static_cast<size_t>(slot)];
    };

    auto const colors = chromeSchemeColorsOf(palette);
    CHECK(slotOf(colors, SchemeSlot::Background) == 0x010101_rgb);
    CHECK(slotOf(colors, SchemeSlot::Foreground) == 0x020202_rgb);
    CHECK(slotOf(colors, SchemeSlot::Bar) == 0x030303_rgb);
    CHECK(slotOf(colors, SchemeSlot::Secondary) == 0x040404_rgb);
    CHECK(slotOf(colors, SchemeSlot::Danger) == 0x050505_rgb);
    CHECK(slotOf(colors, SchemeSlot::Success) == 0x060606_rgb);
    CHECK(slotOf(colors, SchemeSlot::Warning) == 0x070707_rgb);
    CHECK(slotOf(colors, SchemeSlot::Accent) == 0x080808_rgb);
    CHECK(slotOf(colors, SchemeSlot::Selection) == 0xF9F9F9_rgb);

    // The scheme's own selection, before the chrome checks it reads: a cell-colour selection is that end.
    CHECK(schemeSelectionOf(palette) == 0xF9F9F9_rgb);
    palette.selection.background = CellForegroundColor {};
    CHECK(schemeSelectionOf(palette) == 0x020202_rgb);
    palette.selection.background = CellBackgroundColor {};
    CHECK(schemeSelectionOf(palette) == 0x010101_rgb);
}

TEST_CASE("chromeTokens.barInkIsTheFirstLegibleCandidate", "[chrometokens]")
{
    using namespace contour::config;
    auto const slotOf = [](ChromeSchemeColors const& colors, SchemeSlot slot) {
        return colors.bySlot[static_cast<size_t>(slot)];
    };
    auto const black = 0x000000_rgb;
    auto const white = 0xFFFFFF_rgb;

    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const bar = slotOf(colors, SchemeSlot::Bar);
        auto const ink = slotOf(colors, SchemeSlot::BarInk);
        CHECK(contrastRatio(ink, bar) >= MinimumTextContrast);

        // BarInkFallbacks in order: the better end of the scheme, ANSI 15, then black or white.
        auto const schemeEnd =
            contrastRatio(palette.defaultForeground, bar) >= contrastRatio(palette.defaultBackground, bar)
                ? palette.defaultForeground
                : palette.defaultBackground;
        if (contrastRatio(schemeEnd, bar) >= MinimumTextContrast)
            CHECK(ink == schemeEnd);
        else if (contrastRatio(palette.brightColor(7), bar) >= MinimumTextContrast)
            CHECK(ink == palette.brightColor(7));
        else
            CHECK((ink == black || ink == white));
    }

    auto light = ColorPalette {};
    REQUIRE(defaultColorPalettes("contour-light", light));
    CHECK(slotOf(chromeSchemeColorsOf(light), SchemeSlot::BarInk) == light.defaultBackground);

    auto dark = ColorPalette {};
    REQUIRE(defaultColorPalettes("contour-dark", dark));
    CHECK(slotOf(chromeSchemeColorsOf(dark), SchemeSlot::BarInk) == dark.defaultForeground);

    // The two schemes whose ends do not read on their own bar take a later candidate.
    for (auto const* name: { "one-dark", "solarized-light" })
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const ink = slotOf(colors, SchemeSlot::BarInk);
        CHECK(ink != palette.defaultForeground);
        CHECK(ink != palette.defaultBackground);
    }
}

TEST_CASE("chromeTokens.menuPresentationIgnoresPreferences", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const motion: { MotionPreference::Full, MotionPreference::Reduced })
            for (auto const transparency:
                 { TransparencyPreference::Allowed, TransparencyPreference::Reduced })
                for (auto const contrast: { ContrastPreference::Standard, ContrastPreference::Increased })
                {
                    auto const preferences = AccessibilityPreferences { .motion = motion,
                                                                        .transparency = transparency,
                                                                        .contrast = contrast };
                    CHECK(resolveChromeTokens(style, preferences).layout.menu
                          == ChromeTokenRows[index].layout.menu);
                }
    }
}

TEST_CASE("chromeTokens.titleBandTokens", "[chrometokens]")
{
    using namespace contour::config;
    auto const mac = resolveChromeTokens(ChromeStyle::MacOS, {});
    CHECK(mac.size.titleBar == 38);
    CHECK(mac.size.tabWell == 28);
    CHECK(mac.size.tab == 24);
    CHECK(mac.size.tabIcon == 14);
    CHECK(mac.size.tabClose == 16);
    CHECK(mac.size.badge == 14);
    CHECK(mac.size.badgeDot == 6);
    CHECK(mac.size.progressBar == 2);
    CHECK(mac.size.hostChip == 24);
    CHECK(mac.size.hostDot == 7);
    CHECK(mac.size.toolbarButton == 28);
    CHECK(mac.size.toolbarChip == 22);
    CHECK(mac.space.dragReserve == 40);
    CHECK(mac.space.revealEdge == 2);
    CHECK(mac.space.toolbarChipPadding == 9);
    CHECK(mac.radii.tab == 12);
    CHECK(mac.radii.tabWell == 14);
    CHECK(mac.radii.tabIcon == 3);
    CHECK(mac.radii.tabClose == 8);
    CHECK(mac.radii.badge == 7);
    CHECK(mac.radii.hostChip == 12);
    CHECK(mac.radii.toolbarChip == 11);
    CHECK(mac.motion.spinnerMs == 1200);
    CHECK(mac.motion.pulseMs == 700);
    CHECK(mac.motion.style == MotionStyle::Full);
    CHECK(mac.tint.tabPercent == 20);
    CHECK(mac.layout.tabShape == TabShape::Pill);
    CHECK(mac.layout.profileMenu == ProfileMenuAffordance::SecondaryClick);

    auto const tui = resolveChromeTokens(ChromeStyle::Terminal, {});
    CHECK(tui.size.tabIcon == 1);
    CHECK(tui.radii.tab == 0);
    CHECK(tui.tint.tabPercent == 0);
}

TEST_CASE("chromeTokens.onlyTheMacRowIsAPill", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: AllStyles())
    {
        CAPTURE(index);
        auto const isMac = static_cast<ChromeStyle>(index) == ChromeStyle::MacOS;
        CHECK((ChromeTokenRows[index].layout.tabShape == TabShape::Pill) == isMac);
    }
}

TEST_CASE("chromeTokens.tabColoursFollowTheRolesWhereTheChromeDrawsItsOwn", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: AllStyles())
    {
        CAPTURE(index);
        auto const& layout = ChromeTokenRows[index].layout;
        auto const style = static_cast<ChromeStyle>(index);
        // The native Flat rows keep today's palette look; the pills and the Terminal row draw the roles.
        auto const native = layout.tabShape == TabShape::Flat && style != ChromeStyle::Terminal;
        CHECK((layout.tabColors == TabColorSource::Palette) == native);
        // Only the Terminal row's active fill can be the bar's own colour, so only it needs the bold.
        CHECK((layout.activeTabWeight == ActiveTabWeight::Bold) == (style == ChromeStyle::Terminal));
    }
}

TEST_CASE("chromeTokens.reducedMotionCrossFades", "[chrometokens]")
{
    using namespace contour::config;
    auto const reduced = AccessibilityPreferences { .motion = MotionPreference::Reduced,
                                                    .transparency = TransparencyPreference::Allowed,
                                                    .contrast = ContrastPreference::Standard };
    for (auto const index: AllStyles())
    {
        CAPTURE(index);
        auto const style = static_cast<ChromeStyle>(index);
        auto const full = resolveChromeTokens(style, {});
        auto const crossFade = resolveChromeTokens(style, reduced);
        CHECK(full.motion.style == MotionStyle::Full);
        CHECK(crossFade.motion.style == MotionStyle::CrossFade);
        CHECK(crossFade.motion.spinnerMs == full.motion.spinnerMs);
        CHECK(crossFade.motion.pulseMs == full.motion.pulseMs);
    }
}

TEST_CASE("chromeTokens.kitAlphaTables", "[chrometokens]")
{
    using namespace contour::config;
    CHECK(FillAlphas[0] == AlphaPair { .light = 20, .dark = 24 });
    CHECK(FillAlphas[3] == AlphaPair { .light = 5, .dark = 6 });
    CHECK(LabelAlphas[0] == AlphaPair { .light = 88, .dark = 92 });
    CHECK(LabelAlphas[1] == AlphaPair { .light = 60, .dark = 62 });
    CHECK(LabelAlphas[2] == AlphaPair { .light = 45, .dark = 50 });
    CHECK(SurfaceColors[0] == SurfacePair { .light = 0xFFFFFF, .dark = 0x2C2C2E });
    CHECK(SurfaceColors[1] == SurfacePair { .light = 0xF2F2F7, .dark = 0x1C1C1E });
}

namespace
{
/// One new colour role and the source each kind of row gives it.
struct BandColorRow
{
    contour::config::ChromeColorRole role = contour::config::ChromeColorRole::TabWellFill;
    contour::config::ChromeColorSource native {};
    contour::config::ChromeColorSource terminal {};
};
} // namespace

TEST_CASE("chromeTokens.bandColourRoles", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::fill;
    using contour::config::detail::label;
    using contour::config::detail::schemeColor;
    using contour::config::detail::surface;
    using enum ChromeColorRole;
    auto const rows = std::array {
        BandColorRow { .role = TabWellFill, .native = fill(4), .terminal = schemeColor(SchemeSlot::Bar) },
        BandColorRow {
            .role = TabActiveFill, .native = surface(1), .terminal = schemeColor(SchemeSlot::Selection) },
        BandColorRow { .role = TabHoverFill, .native = fill(3), .terminal = schemeColor(SchemeSlot::Bar) },
        BandColorRow { .role = TabCloseFill, .native = fill(3), .terminal = schemeColor(SchemeSlot::Bar) },
        BandColorRow {
            .role = ControlPressedFill, .native = fill(2), .terminal = schemeColor(SchemeSlot::Selection) },
        BandColorRow { .role = TabText, .native = label(2), .terminal = schemeColor(SchemeSlot::BarInk) },
        BandColorRow {
            .role = TabTextActive, .native = label(1), .terminal = schemeColor(SchemeSlot::SelectionText) },
        BandColorRow {
            .role = TabTextDimmed, .native = label(3), .terminal = schemeColor(SchemeSlot::BarInk) },
        BandColorRow {
            .role = TabTextTinted, .native = label(1), .terminal = schemeColor(SchemeSlot::BarInk) },
        BandColorRow { .role = ChipFill, .native = fill(4), .terminal = schemeColor(SchemeSlot::Bar) },
        BandColorRow { .role = ChipText, .native = label(2), .terminal = schemeColor(SchemeSlot::BarInk) },
        BandColorRow {
            .role = ChipTextStrong, .native = label(1), .terminal = schemeColor(SchemeSlot::BarInk) },
    };
    // Every role from TabWellFill up to the pane roles has a row here (chromeTokens.paneColourRoles has
    // those).
    CHECK(rows.size() == static_cast<size_t>(PaneGapFill) - static_cast<size_t>(TabWellFill));

    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const& row: rows)
        {
            CAPTURE(static_cast<int>(row.role));
            auto const& expected = style == ChromeStyle::Terminal ? row.terminal : row.native;
            CHECK(ChromeTokenRows[index].colors[static_cast<size_t>(row.role)] == expected);
        }
    }

    CHECK(fill(4).kind == ColorSourceKind::Fill);
    CHECK(fill(4).level == 4);
    CHECK(label(1).kind == ColorSourceKind::Label);
    CHECK(surface(2).kind == ColorSourceKind::Surface);
    CHECK(surface(2).level == 2);
    CHECK(contour::config::detail::schemeOnBar(SchemeSlot::Accent).kind == ColorSourceKind::SchemeOnBar);
    CHECK(contour::config::detail::schemeOnBar(SchemeSlot::Accent).slot == SchemeSlot::Accent);
}

TEST_CASE("chromeTokens.statusInkPerRow", "[chrometokens]")
{
    using namespace contour::config;
    auto const schemeHue =
        std::array { SchemeSlot::Danger, SchemeSlot::Warning, SchemeSlot::Success, SchemeSlot::Accent };
    static_assert(schemeHue.size() == StatusColorRoleCount);
    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const role: std::views::iota(std::size_t { 0 }, StatusColorRoleCount))
        {
            CAPTURE(role);
            auto const& ink = ChromeTokenRows[index].statusInk[role];
            if (style == ChromeStyle::Terminal)
                CHECK(ink == contour::config::detail::schemeOnBar(schemeHue[role]));
            else
            {
                CHECK(ink.kind == ColorSourceKind::StatusMark);
                CHECK(ink.status == static_cast<StatusColorRole>(role));
                CHECK(ink.alphaPercent == 100);
            }
        }
    }
}

namespace
{
[[nodiscard]] vtbackend::RGBColor slotIn(contour::config::ChromeSchemeColors const& colors,
                                         contour::config::SchemeSlot slot)
{
    return colors.bySlot[static_cast<size_t>(slot)];
}
} // namespace

TEST_CASE("chromeTokens.selectionTextReadsOnTheSelection", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const selection = slotIn(colors, SchemeSlot::Selection);
        auto const text = slotIn(colors, SchemeSlot::SelectionText);
        // The active tab reads on every bundled scheme, not only on those that set a selection colour.
        CHECK(contrastRatio(text, selection) >= MinimumTextContrast);

        // Where the scheme's own selection reads, the chrome keeps it, with its better end on top.
        auto const own = schemeSelectionOf(palette);
        auto const bestOnOwn = std::max(contrastRatio(palette.defaultForeground, own),
                                        contrastRatio(palette.defaultBackground, own));
        if (bestOnOwn >= MinimumTextContrast)
        {
            CHECK(selection == own);
            CHECK(contrastRatio(text, selection) == bestOnOwn);
        }
    }

    // The two schemes that set a selection colour keep it.
    for (auto const* name: { "contour-light", "contour-dark" })
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        CHECK(slotIn(chromeSchemeColorsOf(palette), SchemeSlot::Selection) == schemeSelectionOf(palette));
    }

    // A scheme whose selection fails but whose two ends read on each other takes the inverse pair.
    auto inverse = ColorPalette {};
    inverse.defaultBackground = 0x101010_rgb;
    inverse.defaultForeground = 0xE0E0E0_rgb;
    inverse.selection.background = 0x707070_rgb; // 3.5:1 on the foreground, 4.0:1 on the background
    auto const inverseColors = chromeSchemeColorsOf(inverse);
    CHECK(slotIn(inverseColors, SchemeSlot::Selection) == inverse.defaultForeground);
    CHECK(slotIn(inverseColors, SchemeSlot::SelectionText) == inverse.defaultBackground);

    // one-dark's ends are too close for any scheme pair (3.47:1 at best), so the active tab is the bar
    // with a legible ink on it.
    auto oneDark = ColorPalette {};
    REQUIRE(defaultColorPalettes("one-dark", oneDark));
    auto const oneDarkColors = chromeSchemeColorsOf(oneDark);
    CHECK(slotIn(oneDarkColors, SchemeSlot::Selection) == oneDark.normalColor(0));
    CHECK(slotIn(oneDarkColors, SchemeSlot::SelectionText) == slotIn(oneDarkColors, SchemeSlot::BarInk));
}

TEST_CASE("chromeTokens.statusInkLegibleOnTheBar", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const bar = slotIn(colors, SchemeSlot::Bar);
        for (auto const hue:
             { SchemeSlot::Accent, SchemeSlot::Danger, SchemeSlot::Success, SchemeSlot::Warning })
        {
            CAPTURE(static_cast<int>(hue));
            auto const ink = legibleOnBar(colors, hue);
            CHECK((ink == slotIn(colors, hue) || ink == slotIn(colors, SchemeSlot::BarInk)));
            CHECK(contrastRatio(ink, bar) >= MinimumTextContrast);
        }
    }

    auto light = ColorPalette {};
    REQUIRE(defaultColorPalettes("contour-light", light));
    auto const lightColors = chromeSchemeColorsOf(light);
    CHECK(legibleOnBar(lightColors, SchemeSlot::Accent) == slotIn(lightColors, SchemeSlot::BarInk));

    auto dark = ColorPalette {};
    REQUIRE(defaultColorPalettes("contour-dark", dark));
    CHECK(legibleOnBar(chromeSchemeColorsOf(dark), SchemeSlot::Accent) == dark.normalColor(4));
}

namespace
{
/// The canvas's Comfortable pane values for one row (the plan's Global Constraints table).
struct PaneTokenRow
{
    contour::config::ChromeStyle style = contour::config::ChromeStyle::MacOS;
    int paneHeader = 0;
    int paneGap = 0;
    int splitHit = 0;
    int paneInset = 0;
    int paneCardRadius = 0;
    int bareBand = 0;
    int grabber = 0;
    int grabberLength = 0;
    int grabberRadius = 0;
    int paneHeaderAction = 0;
    int paneHeaderGap = 0;
    double paneHeaderType = 0.0;
    int paneHeaderWeight = 0;
    int paneFooter = 0;
    int footerButton = 0;
    int footerLeading = 0;
    int headerRevealMs = 0;
    int dimDark = 0;
    int dimLight = 0;
    contour::config::PaneActionsReveal paneActions = contour::config::PaneActionsReveal::OnHover;
};

using contour::config::ChromeStyle;
using contour::config::PaneActionsReveal;

/// One row per ChromeStyle, in enumerator order.
constexpr auto PaneTokenRows = std::array {
    PaneTokenRow { .style = ChromeStyle::MacOS,
                   .paneHeader = 22,
                   .paneGap = 6,
                   .splitHit = 10,
                   .paneInset = 6,
                   .paneCardRadius = 10,
                   .bareBand = 32,
                   .grabber = 2,
                   .grabberLength = 32,
                   .grabberRadius = 1,
                   .paneHeaderAction = 22,
                   .paneHeaderGap = 6,
                   .paneHeaderType = 11,
                   .paneHeaderWeight = 500,
                   .paneFooter = 34,
                   .footerButton = 24,
                   .footerLeading = 14,
                   .headerRevealMs = 500,
                   .dimDark = 15,
                   .dimLight = 10,
                   .paneActions = PaneActionsReveal::OnHover },
    PaneTokenRow { .style = ChromeStyle::Fluent,
                   .paneHeader = 28,
                   .paneGap = 4,
                   .splitHit = 10,
                   .paneInset = 4,
                   .paneCardRadius = 4,
                   .bareBand = 28,
                   .grabber = 2,
                   .grabberLength = 32,
                   .grabberRadius = 1,
                   .paneHeaderAction = 22,
                   .paneHeaderGap = 8,
                   .paneHeaderType = 12,
                   .paneHeaderWeight = 600,
                   .paneFooter = 34,
                   .footerButton = 24,
                   .footerLeading = 14,
                   .headerRevealMs = 500,
                   .dimDark = 15,
                   .dimLight = 10,
                   .paneActions = PaneActionsReveal::OnHover },
    PaneTokenRow { .style = ChromeStyle::ContourLinux,
                   .paneHeader = 28,
                   .paneGap = 6,
                   .splitHit = 10,
                   .paneInset = 6,
                   .paneCardRadius = 6,
                   .bareBand = 28,
                   .grabber = 2,
                   .grabberLength = 32,
                   .grabberRadius = 1,
                   .paneHeaderAction = 22,
                   .paneHeaderGap = 8,
                   .paneHeaderType = 12.5,
                   .paneHeaderWeight = 600,
                   .paneFooter = 34,
                   .footerButton = 24,
                   .footerLeading = 14,
                   .headerRevealMs = 500,
                   .dimDark = 15,
                   .dimLight = 10,
                   .paneActions = PaneActionsReveal::OnHover },
    PaneTokenRow { .style = ChromeStyle::Terminal,
                   .paneHeader = 1,
                   .paneGap = 1,
                   .splitHit = 1,
                   .paneInset = 0,
                   .paneCardRadius = 0,
                   .bareBand = 1,
                   .grabber = 0,
                   .grabberLength = 0,
                   .grabberRadius = 0,
                   .paneHeaderAction = 1,
                   .paneHeaderGap = 1,
                   .paneHeaderType = 0,
                   .paneHeaderWeight = 400,
                   .paneFooter = 1,
                   .footerButton = 1,
                   .footerLeading = 1,
                   .headerRevealMs = 500,
                   .dimDark = 15,
                   .dimLight = 10,
                   .paneActions = PaneActionsReveal::OnHoverOrFocus },
};
static_assert(PaneTokenRows.size() == contour::config::detail::ChromeStyleCount);
} // namespace

TEST_CASE("chromeTokens.paneTokens", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const& expected: PaneTokenRows)
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const row = resolveChromeTokens(expected.style, {});
        CHECK(row.size.paneHeader == expected.paneHeader);
        CHECK(row.space.paneGap == expected.paneGap);
        // The vertical gap is the horizontal one, counted in the vertical unit.
        CHECK(row.space.paneGapRows == expected.paneGap);
        CHECK(row.space.splitHit == expected.splitHit);
        CHECK(row.space.paneInset == expected.paneInset);
        CHECK(row.radii.paneCard == expected.paneCardRadius);
        CHECK(row.size.bareBand == expected.bareBand);
        CHECK(row.size.grabber == expected.grabber);
        CHECK(row.size.grabberLength == expected.grabberLength);
        CHECK(row.radii.grabber == expected.grabberRadius);
        CHECK(row.size.paneHeaderAction == expected.paneHeaderAction);
        CHECK(row.space.paneHeaderGap == expected.paneHeaderGap);
        CHECK(row.type.paneHeader == Catch::Approx(expected.paneHeaderType));
        CHECK(row.type.paneHeaderWeight == expected.paneHeaderWeight);
        CHECK(row.size.paneFooter == expected.paneFooter);
        CHECK(row.size.footerButton == expected.footerButton);
        CHECK(row.space.footerLeading == expected.footerLeading);
        CHECK(row.motion.headerRevealMs == expected.headerRevealMs);
        CHECK(row.dim.darkPercent == expected.dimDark);
        CHECK(row.dim.lightPercent == expected.dimLight);
        CHECK(row.layout.paneActions == expected.paneActions);
    }
}

TEST_CASE("chromeTokens.minimalSubRow", "[chrometokens]")
{
    using namespace contour::config;
    auto const mac = resolveChromeTokens(ChromeStyle::MacOS, {}, ChromeDensity::Minimal);
    CHECK(mac.size.paneHeader == 0);
    CHECK(mac.space.paneGap == 1);
    CHECK(mac.space.paneGapRows == 1);
    CHECK(mac.space.splitHit == 8);
    CHECK(mac.space.paneInset == 0);
    CHECK(mac.radii.paneCard == 0);
    CHECK(mac.size.titleBar == 38);

    CHECK(resolveChromeTokens(ChromeStyle::Terminal, {}, ChromeDensity::Minimal).space.splitHit == 1);

    for (auto const index: AllStyles())
    {
        CAPTURE(index);
        auto const style = static_cast<ChromeStyle>(index);
        auto const comfortable = resolveChromeTokens(style, {}, ChromeDensity::Comfortable);
        auto const twoArgument = resolveChromeTokens(style, {});
        CHECK(numbersOf(comfortable) == numbersOf(twoArgument));
        CHECK(comfortable.layout == twoArgument.layout);
        CHECK(comfortable.colors == twoArgument.colors);

        // Minimal moves the six pane extents its sub-row names, and nothing else.
        auto const minimal = resolveChromeTokens(style, {}, ChromeDensity::Minimal);
        auto const& sub = ChromeTokenRows[index].minimal;
        CHECK(minimal.size.paneHeader == sub.paneHeader);
        CHECK(minimal.space.paneGap == sub.paneGap);
        CHECK(minimal.space.paneGapRows == sub.paneGapRows);
        CHECK(minimal.space.splitHit == sub.splitHit);
        CHECK(minimal.space.paneInset == sub.paneInset);
        CHECK(minimal.radii.paneCard == sub.paneCardRadius);
        auto restored = minimal;
        restored.size.paneHeader = comfortable.size.paneHeader;
        restored.space = comfortable.space;
        restored.radii.paneCard = comfortable.radii.paneCard;
        CHECK(numbersOf(restored) == numbersOf(comfortable));
    }
}

TEST_CASE("chromeTokens.increasedContrastCapsTheDim", "[chrometokens]")
{
    using namespace contour::config;
    auto const increased = AccessibilityPreferences { .motion = MotionPreference::Full,
                                                      .transparency = TransparencyPreference::Allowed,
                                                      .contrast = ContrastPreference::Increased };
    for (auto const index: AllStyles())
        for (auto const density: std::views::iota(size_t { 0 }, ChromeDensityCount))
        {
            CAPTURE(index, density);
            auto const tokens = resolveChromeTokens(
                static_cast<ChromeStyle>(index), increased, static_cast<ChromeDensity>(density));
            CHECK(tokens.contrast.paneDimPercent == 5);
            CHECK(tokens.contrast.activeCardRing == Catch::Approx(1.5));
        }
}

TEST_CASE("chromeTokens.gapShade", "[chrometokens]")
{
    using namespace contour::config;
    auto const lightnessOf = [](RGBColor color) {
        return contour::config::detail::hslOf(color).lightness * 100.0;
    };

    auto const background = 0x1C1E24_rgb;
    auto const gap = shadeLightness(background, -4);
    CHECK(relativeLuminance(gap) < relativeLuminance(background));
    CHECK(lightnessOf(background) - lightnessOf(gap) == Catch::Approx(4.0).margin(0.5));
    CHECK(shadeLightness(background, 0) == background);

    // A shade that would leave the range goes the other way, so it always stands off its source.
    CHECK(relativeLuminance(shadeLightness(0x000000_rgb, -4)) > 0.0);
    CHECK(relativeLuminance(shadeLightness(0xFFFFFF_rgb, 4)) < 1.0);

    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        CHECK(shadeLightness(palette.defaultBackground, -4) != palette.defaultBackground);
    }
}

namespace
{
/// One pane colour role and the source each kind of row gives it.
struct PaneColorRow
{
    contour::config::ChromeColorRole role = contour::config::ChromeColorRole::PaneGapFill;
    contour::config::ChromeColorSource native {};
    contour::config::ChromeColorSource terminal {};
};
} // namespace

TEST_CASE("chromeTokens.paneColourRoles", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::fill;
    using contour::config::detail::label;
    using contour::config::detail::schemeColor;
    using contour::config::detail::schemeOnBar;
    using contour::config::detail::schemeShade;
    using contour::config::detail::separator;
    using enum ChromeColorRole;
    auto const transparent = ChromeColorSource { .kind = ColorSourceKind::Fixed,
                                                 .rgb = 0,
                                                 .status = StatusColorRole::Danger,
                                                 .slot = SchemeSlot::Background,
                                                 .alphaPercent = 0,
                                                 .level = 0 };
    auto const rows = std::array {
        PaneColorRow { .role = PaneGapFill,
                       .native = schemeShade(SchemeSlot::Background, -4),
                       .terminal = schemeColor(SchemeSlot::Background) },
        PaneColorRow {
            .role = PaneCardBorder, .native = separator(), .terminal = schemeColor(SchemeSlot::Background) },
        PaneColorRow {
            .role = PaneHeaderFill, .native = transparent, .terminal = schemeColor(SchemeSlot::Bar) },
        PaneColorRow {
            .role = PaneHeaderText, .native = label(1), .terminal = schemeColor(SchemeSlot::BarInk) },
        PaneColorRow { .role = PaneHeaderTextDimmed,
                       .native = label(2),
                       .terminal = schemeOnBar(SchemeSlot::Secondary) },
        PaneColorRow {
            .role = PaneSplitLine, .native = separator(), .terminal = schemeColor(SchemeSlot::Secondary) },
        PaneColorRow { .role = PaneGrabber, .native = fill(1), .terminal = schemeColor(SchemeSlot::Accent) },
        PaneColorRow { .role = PaneFooterFill, .native = fill(3), .terminal = schemeColor(SchemeSlot::Bar) },
        PaneColorRow {
            .role = PaneFooterText, .native = label(1), .terminal = schemeColor(SchemeSlot::BarInk) },
    };
    // Every role from PaneGapFill to the end of the enumeration has a row here.
    CHECK(rows.size() == ChromeColorRoleCount - static_cast<size_t>(PaneGapFill));

    for (auto const index: AllStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const& row: rows)
        {
            CAPTURE(static_cast<int>(row.role));
            auto const& expected = style == ChromeStyle::Terminal ? row.terminal : row.native;
            CHECK(ChromeTokenRows[index].colors[static_cast<size_t>(row.role)] == expected);
        }
    }

    CHECK(schemeShade(SchemeSlot::Background, -4).kind == ColorSourceKind::SchemeShade);
    CHECK(schemeShade(SchemeSlot::Background, -4).level == -4);
    CHECK(separator().kind == ColorSourceKind::Separator);
}

TEST_CASE("chromeTokens.legibleOnBarReadsOnEveryScheme", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const bar = slotIn(colors, SchemeSlot::Bar);
        for (auto const slot: std::views::iota(std::size_t { 0 }, SchemeSlotCount))
        {
            CAPTURE(slot);
            auto const hue = static_cast<SchemeSlot>(slot);
            auto const ink = legibleOnBar(colors, hue);
            CHECK(contrastRatio(ink, bar) >= MinimumTextContrast);
            // Where the scheme's own hue reads, it is kept; else the bar's one ink.
            if (contrastRatio(slotIn(colors, hue), bar) >= MinimumTextContrast)
                CHECK(ink == slotIn(colors, hue));
            else
                CHECK(ink == slotIn(colors, SchemeSlot::BarInk));
        }
    }
}
