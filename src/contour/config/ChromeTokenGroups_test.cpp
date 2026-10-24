// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

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
        return colors.slots[static_cast<size_t>(slot)];
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
