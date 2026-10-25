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
    auto out = std::vector<double> { double(row.size.titleBar),
                                     double(row.size.tabWell),
                                     double(row.size.tab),
                                     double(row.size.control),
                                     double(row.size.paneHeader),
                                     double(row.size.toast),
                                     double(row.size.dialogWidth),
                                     double(row.size.swatchWidth),
                                     double(row.size.swatchHeight),
                                     double(row.size.settingsWidth),
                                     double(row.size.settingsHeight),
                                     double(row.size.settingsMinWidth),
                                     double(row.size.settingsMinHeight),
                                     double(row.space.paneGap),
                                     double(row.space.splitHit),
                                     double(row.space.paneInset) };
    for (auto const v: row.space.scale)
        out.push_back(v);
    for (auto const v:
         { row.radii.control, row.radii.popover, row.radii.menu, row.radii.paneCard, row.radii.toast })
        out.push_back(v);
    for (auto const v: { row.type.caption, row.type.footnote, row.type.body, row.type.tab, row.type.title })
        out.push_back(v);
    for (auto const v:
         { row.motion.fastMs, row.motion.standardMs, row.motion.emphasizedMs, row.motion.travel })
        out.push_back(v);
    for (auto const& level: row.elevation.levels)
        for (auto const v: { level.offsetX, level.offsetY, level.blur, level.opacityPercent })
            out.push_back(v);
    out.push_back(row.contrast.separatorPercent);
    out.push_back(row.contrast.activeCardRing);
    out.push_back(row.contrast.paneDimPercent.value_or(-1));
    out.push_back(row.tui.inactivePaneOpacity);
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
};
} // namespace

TEST_CASE("chromeTokens.everyRowMatchesTheDesignKit", "[chrometokens]")
{
    using namespace contour::config;
    // size(13) space(3) scale(10) radii(5) type(5) motion(4) elevation e0..e3 (16) separator ring dim opacity
    auto const native = std::vector<double> { 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 };
    auto const nativeTail = std::vector<double> { 100, 160, 220, 8,  0, 0,  0,  0,  0,  1,   2,  12,
                                                  0,   4,   16,  25, 0, 12, 40, 35, 10, 0.0, -1, 1.0 };
    auto const join = [](std::vector<double> head,
                         std::vector<double> const& scale,
                         std::vector<double> const& mid,
                         std::vector<double> const& tail) {
        head.insert(head.end(), scale.begin(), scale.end());
        head.insert(head.end(), mid.begin(), mid.end());
        head.insert(head.end(), tail.begin(), tail.end());
        return head;
    };
    auto const rows = std::vector<ExpectedRow> {
        { ChromeStyle::MacOS,
          join({ 38, 28, 24, 28, 22, 36, 280, 16, 16, 960, 820, 720, 480, 6, 10, 6 },
               native,
               { 6, 14, 10, 10, 18, 10, 11, 13, 12, 15 },
               nativeTail),
          Material::Glass,
          Material::Glass,
          ToastStyle::Floating,
          DialogPlacement::Sheet,
          MenuPresentation::Native },
        { ChromeStyle::Fluent,
          join({ 40, 0, 34, 32, 28, 36, 280, 16, 16, 1110, 920, 720, 480, 4, 10, 4 },
               native,
               { 4, 8, 8, 4, 18, 12, 12, 14, 12, 20 },
               nativeTail),
          Material::Mica,
          Material::Solid,
          ToastStyle::Floating,
          DialogPlacement::Centered,
          MenuPresentation::InScene },
        { ChromeStyle::ContourLinux,
          join({ 46, 0, 34, 34, 28, 36, 280, 16, 16, 1100, 920, 720, 480, 6, 10, 6 },
               native,
               { 8, 12, 10, 6, 18, 12, 12.5, 14, 13.5, 15 },
               nativeTail),
          Material::Solid,
          Material::Solid,
          ToastStyle::Floating,
          DialogPlacement::Centered,
          MenuPresentation::InScene },
        { ChromeStyle::Terminal,
          join({ 1, 1, 1, 2, 1, 1, 32, 2, 1, 120, 40, 90, 24, 1, 1, 0 },
               { 0, 0, 0, 1, 1, 1, 1, 1, 2, 3 },
               { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
               { 100, 160, 220, 0, 0, 0, 0, 0, 1, 1, 0, 60, 1, 1, 0, 60, 1, 1, 0, 60, 10, 0.0, -1, 0.62 }),
          Material::None,
          Material::None,
          ToastStyle::Inverse,
          DialogPlacement::Centered,
          MenuPresentation::InScene },
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
            CHECK(text.slot == SchemeSlot::BarText);
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
        CHECK(contrastRatio(RGBColor { row.light }, 0xFFFFFF_rgb) >= 3.0);
        CHECK(contrastRatio(RGBColor { row.dark }, 0x1C1C1E_rgb) >= 3.0);
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
    palette.selection.background = 0x090909_rgb;

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
    CHECK(slotOf(colors, SchemeSlot::Selection) == 0x090909_rgb);

    palette.selection.background = CellForegroundColor {};
    CHECK(slotOf(chromeSchemeColorsOf(palette), SchemeSlot::Selection) == 0x020202_rgb);
    palette.selection.background = CellBackgroundColor {};
    CHECK(slotOf(chromeSchemeColorsOf(palette), SchemeSlot::Selection) == 0x010101_rgb);
}

TEST_CASE("chromeTokens.barTextReadsOnTheBar", "[chrometokens]")
{
    using namespace contour::config;
    auto const slotOf = [](ChromeSchemeColors const& colors, SchemeSlot slot) {
        return colors.bySlot[static_cast<size_t>(slot)];
    };

    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const bar = slotOf(colors, SchemeSlot::Bar);
        auto const text = slotOf(colors, SchemeSlot::BarText);
        CHECK((text == palette.defaultForeground || text == palette.defaultBackground));
        CHECK(contrastRatio(text, bar) >= std::max(contrastRatio(palette.defaultForeground, bar),
                                                   contrastRatio(palette.defaultBackground, bar)));
    }

    auto light = ColorPalette {};
    REQUIRE(defaultColorPalettes("contour-light", light));
    CHECK(slotOf(chromeSchemeColorsOf(light), SchemeSlot::BarText) == light.defaultBackground);
    CHECK(contrastRatio(light.defaultBackground, light.normalColor(0)) >= 4.5);

    auto dark = ColorPalette {};
    REQUIRE(defaultColorPalettes("contour-dark", dark));
    CHECK(slotOf(chromeSchemeColorsOf(dark), SchemeSlot::BarText) == dark.defaultForeground);
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
