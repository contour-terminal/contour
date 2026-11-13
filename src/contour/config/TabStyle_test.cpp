// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/config/GlobalOverrides.hpp>
#include <contour/config/TabStyle.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <vtbackend/core/Color.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

using contour::config::Config;
using contour::config::DefaultsEdition;
using contour::config::LengthUnit;
using contour::config::parseTabFill;
using contour::config::resolveTabStyle;
using contour::config::TabActiveMark;
using contour::config::TabCorners;
using contour::config::TabFill;
using contour::config::TabFillKind;
using contour::config::TabIndicator;
using contour::config::TabLook;
using contour::config::TabSeat;
using contour::config::TabStyleConfig;
using contour::config::TabStyleRow;
using contour::config::YAMLConfigWriter;
using contour::test::loadConfigFromYaml;

namespace
{
/// A custom fill of @p rgb.
[[nodiscard]] TabFill customFill(uint32_t rgb)
{
    return TabFill { .kind = TabFillKind::Custom, .color = vtbackend::RGBColor(rgb) };
}

/// A native row that draws the line between two tabs at @p separatorPercent.
/// The native2026 rule: the line only between tabs at rest, at the row's own tab-separator strength.
[[nodiscard]] constexpr TabStyleRow pixelRow(int separatorPercent) noexcept
{
    return TabStyleRow { .separatorPercent = separatorPercent,
                         .classicSeparatorPercent = 10,
                         .separators = contour::config::TabSeparators::BetweenResting,
                         .unit = LengthUnit::Pixel };
}
} // namespace

TEST_CASE("tabStyle.aFillIsAKeywordOrAColour", "[config][tabstyle]")
{
    // Every keyword reads back as its kind, in any case, and is written as its token.
    for (auto const& keyword: contour::config::TabFillKeywords)
    {
        INFO(keyword.token);
        auto const fill = parseTabFill(keyword.token);
        REQUIRE(fill.has_value());
        CHECK(fill->kind == keyword.kind);
        CHECK(contour::config::tabFillText(*fill) == keyword.token);
    }
    CHECK(parseTabFill("ACCENT") == TabFill { .kind = TabFillKind::Accent, .color = {} });
    // Every kind but Custom has a keyword.
    CHECK(contour::config::TabFillKeywords.size() == contour::config::TabFillKindCount - 1);

    auto const blue = parseTabFill("#3B82F6");
    REQUIRE(blue.has_value());
    CHECK(*blue == customFill(0x3B82F6));
    CHECK(contour::config::tabFillText(*blue) == "#3B82F6");
    CHECK(parseTabFill(contour::config::tabFillText(*blue)) == blue);

    for (auto const* refused: { "", "none", "#3B8", "3B82F6", "#3B82F6FF", "#GGGGGG", "rgb:3b/82/f6" })
    {
        INFO(refused);
        CHECK_FALSE(parseTabFill(refused).has_value());
    }
}

TEST_CASE("tabStyle.theSectionIsLaidOverTheRow", "[config][tabstyle]")
{
    // Untouched, the section leaves everything to the row.
    auto const row = resolveTabStyle(pixelRow(20), {});
    CHECK(row.look == contour::config::TabLookTraits {});
    CHECK(row.spacing == 0);
    CHECK(row.separatorPercent == 20);
    CHECK(row.activeFill.kind == TabFillKind::Auto);
    CHECK(row.inactiveFill.kind == TabFillKind::Auto);
    CHECK(row.indicator == TabIndicator::None);

    auto const styled = resolveTabStyle(pixelRow(20),
                                        TabStyleConfig { .shape = TabLook::Pill,
                                                         .spacing = 6,
                                                         .activeFill = customFill(0x112233),
                                                         .inactiveFill = { .kind = TabFillKind::Subtle },
                                                         .activeIndicator = TabIndicator::TopLine });
    CHECK(styled.look == contour::config::tabLookTraits(TabLook::Pill));
    CHECK(styled.spacing == 6);
    CHECK(styled.activeFill == customFill(0x112233));
    CHECK(styled.inactiveFill.kind == TabFillKind::Subtle);
    CHECK(styled.indicator == TabIndicator::TopLine);
}

TEST_CASE("tabStyle.aGapReplacesTheLineBetweenTwoTabs", "[config][tabstyle]")
{
    // Tabs that touch keep the row's line; tabs that do not need nothing drawn between them.
    CHECK(resolveTabStyle(pixelRow(20), TabStyleConfig { .spacing = 0 }).separatorPercent == 20);
    CHECK(resolveTabStyle(pixelRow(20), TabStyleConfig { .spacing = 1 }).separatorPercent == 0);

    // A gap that did not come through the loader is still held to the range.
    CHECK(resolveTabStyle(pixelRow(20), TabStyleConfig { .spacing = 500 }).spacing
          == contour::config::MaxTabSpacing);
    CHECK(resolveTabStyle(pixelRow(20), TabStyleConfig { .spacing = -3 }).spacing == 0);
    // And so is a row's line.
    CHECK(resolveTabStyle(pixelRow(250), {}).separatorPercent == 100);
}

TEST_CASE("tabStyle.theSeparatorRuleIsTheDefaultsEditions", "[config][tabstyle]")
{
    using contour::config::TabSeparators;

    // Existing configurations keep their look: the classic edition draws the line after every tab at rest,
    // at the chrome's separator strength; native2026 draws it only between tabs at rest, at the row's own.
    CHECK(contour::config::editionDefaults(DefaultsEdition::Classic).tabSeparators == TabSeparators::Classic);
    CHECK(contour::config::editionDefaults(DefaultsEdition::Native2026).tabSeparators
          == TabSeparators::BetweenResting);

    auto const classic = TabStyleRow { .separatorPercent = 20,
                                       .classicSeparatorPercent = 10,
                                       .separators = TabSeparators::Classic };
    auto const resting = TabStyleRow { .separatorPercent = 20,
                                       .classicSeparatorPercent = 10,
                                       .separators = TabSeparators::BetweenResting };
    CHECK(resolveTabStyle(classic, {}).separatorPercent == 10);
    CHECK(resolveTabStyle(classic, {}).separators == TabSeparators::Classic);
    CHECK(resolveTabStyle(resting, {}).separatorPercent == 20);
    CHECK(resolveTabStyle(resting, {}).separators == TabSeparators::BetweenResting);
    // Zero is the classic rule, so a row that names none keeps the old look.
    CHECK(TabStyleRow {}.separators == TabSeparators::Classic);
    // A gap replaces the line under either rule.
    CHECK(resolveTabStyle(classic, TabStyleConfig { .spacing = 4 }).separatorPercent == 0);
}

TEST_CASE("tabStyle.aRowOfCellsTakesNeitherALookNorAGap", "[config][tabstyle]")
{
    // The Terminal row's tabs are character cells: a rounded, floating tab or a pixel gap has no meaning
    // on the grid, so both are left out and the row's own line stays.
    auto const cells = TabStyleRow { .separatorPercent = 10,
                                     .classicSeparatorPercent = 10,
                                     .separators = contour::config::TabSeparators::Classic,
                                     .unit = LengthUnit::Cell };
    auto const styled = resolveTabStyle(
        cells,
        TabStyleConfig { .shape = TabLook::Outlined, .spacing = 8, .activeFill = customFill(0x112233) });
    CHECK(styled.look == contour::config::TabLookTraits {});
    CHECK(styled.spacing == 0);
    CHECK(styled.separatorPercent == 10);
    // What is a colour rather than a shape still applies.
    CHECK(styled.activeFill == customFill(0x112233));
}

TEST_CASE("tabStyle.everyLookHasItsTraits", "[config][tabstyle]")
{
    using contour::config::tabLookTraits;
    using Edge = contour::config::TabBarEdge;

    // Auto draws nothing of its own: every trait is the row's.
    CHECK(tabLookTraits(TabLook::Auto) == contour::config::TabLookTraits {});

    struct Expected
    {
        TabLook look;
        TabCorners corners;
        TabSeat seat;
        TabActiveMark active;
        contour::config::TabBarEdge edge;
    };
    constexpr auto Rows = std::array {
        Expected { TabLook::Pill, TabCorners::Round, TabSeat::Floating, TabActiveMark::Wash, Edge::Row },
        Expected { TabLook::Flat, TabCorners::Square, TabSeat::Row, TabActiveMark::Wash, Edge::Ruled },
        Expected {
            TabLook::Underline, TabCorners::Square, TabSeat::Row, TabActiveMark::Underline, Edge::Ruled },
        Expected {
            TabLook::Outlined, TabCorners::Round, TabSeat::Floating, TabActiveMark::Outline, Edge::Row },
    };
    // Every look but Auto is listed, and each marks the active tab its own way.
    CHECK(Rows.size() == contour::config::TabLookCount - 1);
    for (auto const& row: Rows)
    {
        CAPTURE(static_cast<int>(row.look));
        auto const& traits = tabLookTraits(row.look);
        CHECK(traits.corners == row.corners);
        CHECK(traits.seat == row.seat);
        CHECK(traits.active == row.active);
        // A look whose tabs are square has nothing joining the content, so the bar's foot is ruled.
        CHECK(traits.edge == row.edge);
        CHECK((traits.edge == Edge::Ruled) == (traits.corners == TabCorners::Square));
        CHECK(traits.active != TabActiveMark::Row);
    }

    // Every look has a spelling, in enumerator order.
    auto const values = contour::config::configEnumValues<TabLook>();
    REQUIRE(values.size() == contour::config::TabLookCount);
    for (auto const index: std::views::iota(size_t { 0 }, values.size()))
        CHECK(static_cast<size_t>(values[index].value) == index);
}

TEST_CASE("tabStyle.aMaterialIsGrantedByTheRowsOwnMaterial", "[config][tabstyle][chrometokens]")
{
    using namespace contour::config;

    // The wish is named for what the user sees; the Windows row grants it with the compositor's two
    // backdrops, whichever of them the row has at the moment.
    CHECK(bandMaterialFor(ChromeStyle::Fluent, Material::Mica, TabBarMaterial::Auto) == Material::Mica);
    CHECK(bandMaterialFor(ChromeStyle::Fluent, Material::Mica, TabBarMaterial::Solid) == Material::Solid);
    CHECK(bandMaterialFor(ChromeStyle::Fluent, Material::Mica, TabBarMaterial::Frosted) == Material::Mica);
    CHECK(bandMaterialFor(ChromeStyle::Fluent, Material::Mica, TabBarMaterial::Glass) == Material::Acrylic);
    // The Terminal row's band is cells: no wish gives it a material.
    CHECK(bandMaterialFor(ChromeStyle::Terminal, Material::None, TabBarMaterial::Solid) == Material::None);
    CHECK(bandMaterialFor(ChromeStyle::Terminal, Material::None, TabBarMaterial::Glass) == Material::None);

    // Every grant names a material that is not opaque: a grant of Solid would be the Solid wish.
    for (auto const& grant: BandMaterialGrants)
    {
        CAPTURE(static_cast<int>(grant.style), static_cast<int>(grant.wish));
        CHECK(opaqueMaterialOf(grant.material) != grant.material);
        CHECK(grant.wish != TabBarMaterial::Auto);
        CHECK(grant.wish != TabBarMaterial::Solid);
    }

    // On every row: a wish the row has no grant for keeps the row's own material.
    auto const grantedBy = [](ChromeStyle style, TabBarMaterial wish) -> std::optional<Material> {
        auto const grant = std::ranges::find_if(BandMaterialGrants, [&](BandMaterialGrant const& candidate) {
            return candidate.style == style && candidate.wish == wish;
        });
        return grant != BandMaterialGrants.end() ? std::optional { grant->material } : std::nullopt;
    };
    for (auto const index: std::views::iota(size_t { 0 }, ChromeTokenRows.size()))
    {
        CAPTURE(index);
        auto const style = static_cast<ChromeStyle>(index);
        auto const own = resolveChromeTokens(style, {}).material.titleBand;
        auto const wished = [&](TabBarMaterial wish, AccessibilityPreferences preferences = {}) {
            return resolveChromeTokens(
                       style, preferences, ChromeDensity::Comfortable, MaterialSupport::Unknown, wish)
                .material.titleBand;
        };
        CHECK(wished(TabBarMaterial::Auto) == own);
        CHECK(wished(TabBarMaterial::Solid) == opaqueMaterialOf(own));
        for (auto const wish: { TabBarMaterial::Frosted, TabBarMaterial::Glass })
            CHECK(wished(wish) == grantedBy(style, wish).value_or(own));
        // Windows grants both today.
        if (style == ChromeStyle::Fluent)
            CHECK(wished(TabBarMaterial::Glass) == Material::Acrylic);
        // Reduced transparency still turns a wished-for material Solid.
        CHECK(wished(TabBarMaterial::Glass, { .transparency = TransparencyPreference::Reduced })
              == opaqueMaterialOf(own));
    }

    // And so does an operating system that cannot draw it.
    CHECK(resolveChromeTokens(ChromeStyle::Fluent,
                              {},
                              ChromeDensity::Comfortable,
                              MaterialSupport::Unsupported,
                              TabBarMaterial::Glass)
              .material.titleBand
          == Material::Solid);

    // Every material has a spelling, and it parses and survives the writer.
    for (auto const& info: configEnumValues<TabBarMaterial>())
    {
        INFO(info.token);
        auto const yaml = std::string { "tab_style:\n  material: " } + std::string { info.token } + "\n";
        auto const config = loadConfigFromYaml(yaml);
        CHECK(config.tabStyle.value().material == info.value);
        auto const rendered = createString<YAMLConfigWriter>(config);
        CHECK(rendered.contains(std::string { "material: " } + std::string { info.token }));
        CHECK(loadConfigFromYaml(rendered).tabStyle.value().material == info.value);
    }
}

TEST_CASE("tabStyle.theWindowsRowDrawsAStrongerLineBetweenItsTabs", "[config][tabstyle][chrometokens]")
{
    using namespace contour::config;
    for (auto const index: std::views::iota(size_t { 0 }, ChromeTokenRows.size()))
    {
        CAPTURE(index);
        auto const style = static_cast<ChromeStyle>(index);
        auto const base = resolveChromeTokens(style, {});
        CHECK(base.contrast.tabSeparatorPercent == (style == ChromeStyle::Fluent ? 20 : 10));
        // The Increased contrast preference doubles it, as it doubles every separator.
        auto const increased = resolveChromeTokens(style, { .contrast = ContrastPreference::Increased });
        CHECK(increased.contrast.tabSeparatorPercent == 2 * base.contrast.tabSeparatorPercent);
    }
}

TEST_CASE("tabStyle.bothEditionsLeaveTheTabsToTheRow", "[config][tabstyle]")
{
    for (auto const edition: { DefaultsEdition::Classic, DefaultsEdition::Native2026 })
    {
        auto const config = Config { .defaultsEdition = edition };
        CHECK(config.tabStyle.value() == TabStyleConfig {});
    }
}

TEST_CASE("tabStyle.theSectionParses", "[config][tabstyle]")
{
    auto const config = loadConfigFromYaml("tab_style:\n"
                                           "  shape: Outlined\n"
                                           "  spacing: 6\n"
                                           "  active_fill: Accent\n"
                                           "  inactive_fill: '#202428'\n"
                                           "  bar_fill: '#101418'\n"
                                           "  active_indicator: Top-Line\n");
    auto const& style = config.tabStyle.value();
    CHECK(style.shape == TabLook::Outlined);
    CHECK(style.spacing == 6);
    CHECK(style.activeFill.kind == TabFillKind::Accent);
    CHECK(style.inactiveFill == customFill(0x202428));
    CHECK(style.barFill == customFill(0x101418));
    CHECK(resolveTabStyle(pixelRow(10), style).barFill == customFill(0x101418));
    // A colour or the accent puts a new ground under the bar's labels; the row's own and a wash do not.
    CHECK(contour::config::bringsItsOwnTone(style.barFill));
    CHECK(contour::config::bringsItsOwnTone({ .kind = TabFillKind::Accent }));
    CHECK_FALSE(contour::config::bringsItsOwnTone({ .kind = TabFillKind::Subtle }));
    CHECK_FALSE(contour::config::bringsItsOwnTone({}));
    CHECK(style.activeIndicator == TabIndicator::TopLine);
}

TEST_CASE("tabStyle.everyShapeParsesAndSurvivesTheWriter", "[config][tabstyle]")
{
    for (auto const& info: contour::config::configEnumValues<TabLook>())
    {
        INFO(info.token);
        auto const yaml = std::string { "tab_style:\n  shape: " } + std::string { info.token } + "\n";
        auto const config = loadConfigFromYaml(yaml);
        CHECK(config.tabStyle.value().shape == info.value);

        auto const rendered = contour::config::createString<YAMLConfigWriter>(config);
        CHECK(rendered.contains(std::string { "shape: " } + std::string { info.token }));
        CHECK(loadConfigFromYaml(rendered).tabStyle.value().shape == info.value);
    }
}

TEST_CASE("tabStyle.theIndicatorColourIsTheAccentOrAColour", "[config][tabstyle]")
{
    using contour::config::parseIndicatorColor;

    // The accent by default, in the section and in what it resolves to.
    CHECK(TabStyleConfig {}.indicatorColor.kind == TabFillKind::Accent);
    CHECK(resolveTabStyle(pixelRow(10), {}).indicatorColor.kind == TabFillKind::Accent);

    CHECK(parseIndicatorColor("Accent") == TabFill { .kind = TabFillKind::Accent, .color = {} });
    CHECK(parseIndicatorColor("#11CC88") == customFill(0x11CC88));
    // A line has no row colour to keep and no wash to be, so the fills' other two words are refused.
    for (auto const* refused: { "auto", "subtle", "", "11CC88" })
    {
        INFO(refused);
        CHECK_FALSE(parseIndicatorColor(refused).has_value());
    }

    auto const config = loadConfigFromYaml("tab_style:\n  indicator_color: '#11CC88'\n");
    CHECK(config.tabStyle.value().indicatorColor == customFill(0x11CC88));
    CHECK(resolveTabStyle(pixelRow(10), config.tabStyle.value()).indicatorColor == customFill(0x11CC88));
    // It survives the writer, quoted as every colour is.
    auto const rendered = contour::config::createString<YAMLConfigWriter>(config);
    CHECK(rendered.contains("indicator_color: '#11CC88'"));
    CHECK(loadConfigFromYaml(rendered).tabStyle.value().indicatorColor == customFill(0x11CC88));

    // A word the key does not take keeps the accent.
    CHECK(loadConfigFromYaml("tab_style:\n  indicator_color: subtle\n").tabStyle.value().indicatorColor
          == contour::config::DefaultIndicatorColor);
}

TEST_CASE("tabStyle.theCloseButtonIsTheRowsUntilTheSectionNamesWhen", "[config][tabstyle]")
{
    using contour::config::closeVisibilityOf;
    using contour::config::TabCloseButton;
    using contour::config::TabCloseVisibility;

    // Auto is the row's own, whichever it is; every other setting is the same on every row.
    for (auto const rowOwn: { TabCloseVisibility::Always, TabCloseVisibility::ActiveOrHovered })
    {
        CAPTURE(static_cast<int>(rowOwn));
        CHECK(closeVisibilityOf(TabCloseButton::Auto, rowOwn) == rowOwn);
        CHECK(closeVisibilityOf(TabCloseButton::Hover, rowOwn) == TabCloseVisibility::Hovered);
        CHECK(closeVisibilityOf(TabCloseButton::Active, rowOwn) == TabCloseVisibility::ActiveOrHovered);
        CHECK(closeVisibilityOf(TabCloseButton::Always, rowOwn) == TabCloseVisibility::Always);
        CHECK(closeVisibilityOf(TabCloseButton::Never, rowOwn) == TabCloseVisibility::Never);
    }
    // One answer per setting.
    CHECK(contour::config::TabCloseVisibilities.size()
          == contour::config::configEnumValues<TabCloseButton>().size());

    auto row = pixelRow(20);
    row.closeVisibility = TabCloseVisibility::ActiveOrHovered;
    CHECK(resolveTabStyle(row, {}).closeVisibility == TabCloseVisibility::ActiveOrHovered);
    CHECK(resolveTabStyle(row, TabStyleConfig { .closeButton = TabCloseButton::Never }).closeVisibility
          == TabCloseVisibility::Never);
}

TEST_CASE("tabStyle.theCloseAndNewTabButtonsParseAndSurviveTheWriter", "[config][tabstyle]")
{
    using contour::config::NewTabButton;
    using contour::config::TabCloseButton;

    // Both default to what the window has always done.
    CHECK(TabStyleConfig {}.closeButton == TabCloseButton::Auto);
    CHECK(TabStyleConfig {}.newTabButton == NewTabButton::Shown);

    for (auto const& info: contour::config::configEnumValues<TabCloseButton>())
    {
        INFO(info.token);
        auto const config = loadConfigFromYaml(std::string { "tab_style:\n  close_button: " }
                                               + std::string { info.token } + "\n");
        CHECK(config.tabStyle.value().closeButton == info.value);
        auto const rendered = contour::config::createString<YAMLConfigWriter>(config);
        CHECK(loadConfigFromYaml(rendered).tabStyle.value().closeButton == info.value);
    }
    for (auto const& info: contour::config::configEnumValues<NewTabButton>())
    {
        INFO(info.token);
        auto const config = loadConfigFromYaml(std::string { "tab_style:\n  new_tab_button: " }
                                               + std::string { info.token } + "\n");
        CHECK(config.tabStyle.value().newTabButton == info.value);
        CHECK(resolveTabStyle(pixelRow(10), config.tabStyle.value()).newTabButton == info.value);
        auto const rendered = contour::config::createString<YAMLConfigWriter>(config);
        CHECK(loadConfigFromYaml(rendered).tabStyle.value().newTabButton == info.value);
    }
}

TEST_CASE("tabStyle.tabColorModeParsesEachValueAndFillsByDefault", "[config][tabstyle]")
{
    using contour::config::TabColorMode;

    // Fill is zero: what a coloured tab looked like before the key existed.
    CHECK(TabStyleConfig {}.tabColorMode == TabColorMode::Fill);
    CHECK(resolveTabStyle(pixelRow(10), {}).tabColorMode == TabColorMode::Fill);

    for (auto const& info: contour::config::configEnumValues<TabColorMode>())
    {
        INFO(info.token);
        auto const yaml =
            std::string { "tab_style:\n  tab_color_mode: " } + std::string { info.token } + "\n";
        auto const config = loadConfigFromYaml(yaml);
        CHECK(config.tabStyle.value().tabColorMode == info.value);
        CHECK(resolveTabStyle(pixelRow(10), config.tabStyle.value()).tabColorMode == info.value);

        // And it survives the writer.
        auto const rendered = contour::config::createString<YAMLConfigWriter>(config);
        CHECK(rendered.contains(std::string { "tab_color_mode: " } + std::string { info.token }));
        CHECK(loadConfigFromYaml(rendered).tabStyle.value().tabColorMode == info.value);
    }

    // A word no row names keeps the default.
    CHECK(loadConfigFromYaml("tab_style:\n  tab_color_mode: glow\n").tabStyle.value().tabColorMode
          == TabColorMode::Fill);
}

TEST_CASE("tabStyle.aKeyLeftOutKeepsItsDefault", "[config][tabstyle]")
{
    auto const config = loadConfigFromYaml("tab_style:\n  active_indicator: underline\n");
    auto expected = TabStyleConfig {};
    expected.activeIndicator = TabIndicator::Underline;
    CHECK(config.tabStyle.value() == expected);
}

TEST_CASE("tabStyle.aValueTheLoaderCannotReadKeepsWhatWasThere", "[config][tabstyle]")
{
    // Each bad leaf is refused on its own; the good one beside it is still read.
    auto const config = loadConfigFromYaml("tab_style:\n"
                                           "  shape: hexagon\n"
                                           "  spacing: 150\n"
                                           "  active_fill: mauve\n"
                                           "  inactive_fill: [subtle]\n"
                                           "  active_indicator: top-line\n");
    auto expected = TabStyleConfig {};
    expected.activeIndicator = TabIndicator::TopLine;
    CHECK(config.tabStyle.value() == expected);

    // Neither is a gap below zero, nor one that is no number.
    CHECK(loadConfigFromYaml("tab_style:\n  spacing: -1\n").tabStyle.value().spacing == 0);
    CHECK(loadConfigFromYaml("tab_style:\n  spacing: wide\n").tabStyle.value().spacing == 0);
}

TEST_CASE("tabStyle.theWrittenConfigReadsBackTheSame", "[config][tabstyle]")
{
    auto original = Config {};
    auto const rendered = contour::config::createString<YAMLConfigWriter>(original);
    // The generated configuration states the section, with every key at the row's own.
    CHECK(rendered.contains("tab_style:"));
    CHECK(rendered.contains("shape: auto"));
    CHECK(rendered.contains("spacing: 0"));
    CHECK(rendered.contains("active_indicator: none"));
    CHECK(loadConfigFromYaml(rendered).tabStyle.value() == TabStyleConfig {});

    original.tabStyle = TabStyleConfig { .shape = TabLook::Pill,
                                         .spacing = 4,
                                         .activeFill = customFill(0x3B82F6),
                                         .inactiveFill = { .kind = TabFillKind::Subtle },
                                         .activeIndicator = TabIndicator::Underline };
    auto const styled = contour::config::createString<YAMLConfigWriter>(original);
    // A colour is quoted: a plain YAML scalar cannot begin with `#`.
    CHECK(styled.contains("active_fill: '#3B82F6'"));
    CHECK(loadConfigFromYaml(styled).tabStyle.value() == original.tabStyle.value());
}

TEST_CASE("tabStyle.aSettingsYmlOverrideOfOneLeafLeavesItsSiblings", "[config][tabstyle]")
{
    // The section's row covers each leaf, so the Settings window can save one.
    auto const* row = contour::config::globalOverrideFor("tab_style.active_fill");
    REQUIRE(row != nullptr);
    CHECK(row->key == "tab_style");
    CHECK_FALSE(row->covers("tab_style"));

    auto const yaml = contour::config::emitGuiSettingsYaml(
        { .defaultProfile = std::nullopt,
          .globalOverrides = { { "tab_style.active_fill", "#3B82F6" }, { "tab_style.spacing", "8" } },
          .collectionOverrides = {} });
    INFO(yaml);

    contour::test::TempDir const dir;
    REQUIRE(dir.isValid());
    {
        auto out = std::ofstream(dir.path() / "contour.yml");
        out << "tab_style:\n  active_indicator: top-line\n  spacing: 2\n";
    }
    {
        auto out = std::ofstream(dir.path() / "settings.yml");
        out << yaml;
    }
    auto config = Config {};
    contour::config::loadConfigFromFile(config, dir.path() / "contour.yml");
    auto const& style = config.tabStyle.value();
    CHECK(style.activeFill == customFill(0x3B82F6));
    CHECK(style.spacing == 8);
    // contour.yml's own leaf, which settings.yml does not name, stays.
    CHECK(style.activeIndicator == TabIndicator::TopLine);
}

TEST_CASE("tabStyle.tabWidthKeepsTheRowsOwnUntilAsked", "[config][tabstyle]")
{
    using contour::config::TabWidth;
    using contour::config::TabWidthLayout;

    auto row = pixelRow(10);
    row.widths = TabWidthLayout::Fit;
    row.minTabWidth = 80;
    row.maxTabWidth = 240;

    // Auto, and zero limits, are the row's own.
    auto const plain = resolveTabStyle(row, {});
    CHECK(plain.widths == TabWidthLayout::Fit);
    CHECK(plain.minTabWidth == 80);
    CHECK(plain.maxTabWidth == 240);

    // Equal and fit name the layout; the limits replace the row's, within the setting's range.
    CHECK(resolveTabStyle(row, TabStyleConfig { .tabWidth = TabWidth::Equal }).widths
          == TabWidthLayout::Equal);
    auto const limited = resolveTabStyle(row, TabStyleConfig { .minTabWidth = 120, .maxTabWidth = 5000 });
    CHECK(limited.minTabWidth == 120);
    CHECK(limited.maxTabWidth == contour::config::MaxTabWidthSetting);

    // A maximum under the minimum is the minimum.
    auto const crossed = resolveTabStyle(row, TabStyleConfig { .minTabWidth = 200, .maxTabWidth = 100 });
    CHECK(crossed.maxTabWidth == 200);

    // A row of equal pills keeps them equal, whatever is asked.
    row.widths = TabWidthLayout::Equal;
    CHECK(resolveTabStyle(row, TabStyleConfig { .tabWidth = TabWidth::Fit }).widths == TabWidthLayout::Equal);

    // A row counted in cells takes none of it: its tabs are cells.
    auto cells = row;
    cells.unit = LengthUnit::Cell;
    cells.widths = TabWidthLayout::Fit;
    auto const cellTabs = resolveTabStyle(
        cells, TabStyleConfig { .tabWidth = TabWidth::Equal, .minTabWidth = 300, .maxTabWidth = 400 });
    CHECK(cellTabs.widths == TabWidthLayout::Fit);
    CHECK(cellTabs.minTabWidth == 80);
    CHECK(cellTabs.maxTabWidth == 240);
}

TEST_CASE("tabStyle.theWidthKeysParseRejectOutOfRangeAndSurviveTheWriter", "[config][tabstyle]")
{
    using contour::config::TabWidth;

    for (auto const& info: contour::config::configEnumValues<TabWidth>())
    {
        INFO(info.token);
        auto const config = loadConfigFromYaml(std::string { "tab_style:\n  tab_width: " }
                                               + std::string { info.token } + "\n");
        CHECK(config.tabStyle.value().tabWidth == info.value);
        auto const rendered = contour::config::createString<YAMLConfigWriter>(config);
        CHECK(loadConfigFromYaml(rendered).tabStyle.value().tabWidth == info.value);
    }

    auto const config = loadConfigFromYaml("tab_style:\n  min_tab_width: 90\n  max_tab_width: 320\n");
    CHECK(config.tabStyle.value().minTabWidth == 90);
    CHECK(config.tabStyle.value().maxTabWidth == 320);
    auto const rendered = loadConfigFromYaml(contour::config::createString<YAMLConfigWriter>(config));
    CHECK(rendered.tabStyle.value().minTabWidth == 90);
    CHECK(rendered.tabStyle.value().maxTabWidth == 320);

    // Out of range is a typo: the default stays.
    auto const wild = loadConfigFromYaml("tab_style:\n  min_tab_width: -4\n  max_tab_width: 99999\n");
    CHECK(wild.tabStyle.value().minTabWidth == 0);
    CHECK(wild.tabStyle.value().maxTabWidth == 0);
}
