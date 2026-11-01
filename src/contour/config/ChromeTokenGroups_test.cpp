// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <utility>
#include <vector>

using namespace vtbackend;

namespace
{
constexpr auto allStyles()
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
    // Ruling S6: the band is the client's paint over the compact toolbar, so its tabs take clicks; the
    // popovers keep the glass.
    CHECK(mac.material.titleBand == Material::Solid);
    CHECK(mac.material.popover == Material::Glass);
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
    append({ row.type.paneHeader, double(row.type.paneHeaderWeight), row.type.paneHeaderAction });
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
    // The command palette's tokens, across their groups.
    append({ double(size.paletteWidth),
             double(size.paletteMaxHeight),
             double(size.paletteField),
             double(size.paletteRow),
             double(size.paletteGroupHeader),
             double(size.paletteFooter),
             double(row.space.paletteInset),
             row.type.paletteInput,
             row.type.shortcut,
             double(row.motion.popoverScalePercent),
             double(row.layout.paletteTopPercent) });
    append({ double(size.findBar),
             double(size.findBarWidth),
             double(size.findToggleWidth),
             double(size.findToggleHeight),
             double(size.findButtonWidth),
             double(size.findButtonHeight),
             double(size.findIcon),
             double(row.space.findBarInset),
             double(row.space.findBarInsetRows),
             double(row.space.findBarCountGap),
             double(radii.findBar) });
    // The overlays' tokens: the link pill, the overlay scrollbar, and the size indicator's hold.
    append({ double(size.linkPill),
             double(size.scrollbar),
             double(row.space.scrollbarInset),
             double(radii.linkPill),
             double(radii.scrollbar),
             double(row.motion.lingerMs) });
    // The secure-input lock's icon edge; the Terminal row sets the lock as cell text instead.
    out.push_back(size.secureInputGlyph);
    // The command blocks' tokens: the strip and its bar, the mark, the actions button, the chips and the
    // scrollbar ticks.
    append({ double(size.rail),
             double(size.railMark),
             double(size.blockActions),
             double(size.foldChip),
             double(size.foldChipHit),
             double(size.scrollTick),
             double(size.scrollTickOutline),
             double(row.space.blockGutter),
             double(row.space.railInset),
             double(row.space.railEnd),
             double(radii.rail),
             double(radii.blockActions),
             double(radii.chip) });
    // The joined strip's buttons and leading inset, 0 on the rows that do not join.
    append({ double(size.stripButton), double(size.profileButton), double(row.space.tabStripLeading) });
    // The header bar and its window buttons.
    append({ double(size.windowButton),
             double(size.windowButtonGlyph),
             double(size.headerIcon),
             double(size.controlsSeparator),
             double(radii.window),
             double(radii.windowButton),
             double(radii.menuRow) });
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
    contour::config::FindBarHost findBar = contour::config::FindBarHost::Overlay;
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
    // linkPill scrollbar scrollbarInset, radii linkPill scrollbar, motion lingerMs (Fluent and ContourLinux
    // take the macOS extents: they have no board of their own).
    auto const nativeOverlay = std::vector<double> { 22, 8, 4, 11, 4, 600 };
    // rail railMark blockActions foldChip foldChipHit scrollTick scrollTickOutline, blockGutter railInset
    // railEnd, radii rail blockActions chip (the canvas's `blocks`; every native row alike).
    auto const nativeBlocks = std::vector<double> { 3, 13, 22, 20, 24, 3, 1, 22, 8, 2, 2, 6, 10 };
    auto const nativeTail = std::vector<double> { 0,  0,  0, 0,  0,   1,  2,  12, 0, 4, 16, 25, 0, 12, 40,
                                                  35, 10, 0, -1, 1.0, 20, 15, 10, 0, 1, 1,  8,  0, 0 };
    // the find bar: size(7) inset(2) count gap(1) radius(1), mac-find-menu
    auto const nativeFind = std::vector<double> { 32, 380, 24, 22, 22, 22, 14, 8, 8, 0, 8 };
    // ContourLinux's find bar is the kit's 34, its controls' height.
    auto const linuxFind = std::vector<double> { 34, 380, 24, 22, 22, 22, 14, 8, 8, 0, 8 };
    // windowButton windowButtonGlyph headerIcon controlsSeparator, radii window windowButton menuRow
    // (kit-tokens; Fluent's window corner is 8, the separator is Linux's alone, and the other native rows
    // take ContourLinux's round buttons for window_control_style: linux).
    auto const macHeader = std::vector<double> { 24, 10, 16, 0, 0, 12, 5 };
    auto const fluentHeader = std::vector<double> { 24, 10, 16, 0, 8, 12, 4 };
    auto const linuxHeader = std::vector<double> { 24, 10, 16, 20, 12, 12, 6 };
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
                   { 10, 11, 13, 12, 15, 11, 500, 15 },
                   nativeMotion,
                   nativeTail,
                   { 640, 420, 44, 32, 24, 28, 8, 17, 12, 97, 18 },
                   nativeFind,
                   nativeOverlay,
                   { 11 },
                   nativeBlocks,
                   { 0, 0, 0 },
                   macHeader }),
          Material::Solid,
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
                   { 16, 24, 14, 6, 2, 24, 7, 32, 22, 30 },
                   { 28 },
                   nativePaneSizes,
                   { 4, 10, 4 },
                   native,
                   nativeBand,
                   { 4, 8, 14 },
                   { 4, 8, 8, 4, 18, 6, 0 },
                   // tabIcon tabClose badge hostChip toolbarChip grabber (win: the close button's 4)
                   { 3, 4, 7, 12, 11, 1 },
                   { 12, 12, 14, 12, 20, 12, 600, 15 },
                   nativeMotion,
                   nativeTail,
                   { 640, 420, 44, 36, 24, 28, 8, 16, 12, 97, 18 },
                   nativeFind,
                   nativeOverlay,
                   { 11 },
                   nativeBlocks,
                   // win: the + and the profile caret 30 tall, the caret 28 wide, the strip led by 8
                   { 30, 28, 8 },
                   fluentHeader }),
          Material::Mica,
          Material::Solid,
          ToastStyle::Floating,
          DialogPlacement::Centered,
          MenuPresentation::InScene,
          TabShape::Joined,
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
                   { 12, 12.5, 14, 13.5, 15, 12.5, 600, 15 },
                   nativeMotion,
                   nativeTail,
                   { 640, 420, 44, 38, 24, 28, 8, 16, 12.5, 97, 18 },
                   linuxFind,
                   nativeOverlay,
                   { 11 },
                   nativeBlocks,
                   { 0, 0, 0 },
                   linuxHeader }),
          Material::Solid,
          Material::Solid,
          ToastStyle::Floating,
          DialogPlacement::Centered,
          MenuPresentation::InScene,
          // The header bar's pills; the "+" is its newTab item, a secondary click on it the profiles.
          TabShape::Pill,
          ProfileMenuAffordance::SecondaryClick,
          TabColorSource::Roles,
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
                   { 0, 0, 0, 0, 0, 0, 400, 0 },
                   { 100, 160, 220, 0, 1200, 700, 500 },
                   { 0, 0, 0, 0, 1, 1, 0, 60, 1, 1, 0, 60, 1, 1, 0, 60, 10, 0.0, -1, 0.62, 0 },
                   { 15, 10, 0, 1, 1, 1, 0, 0 },
                   { 48, 20, 1, 1, 1, 1, 1, 0, 0, 100, 18 },
                   // the find bar, in cells and rows (tui)
                   { 1, 48, 4, 1, 2, 1, 0, 1, 1, 1, 0 },
                   // A one-cell pill and track, set flush, square, and the hold every row shares.
                   { 1, 1, 0, 0, 0, 600 },
                   // No icon: the lock is `[secure]` in cells.
                   { 0 },
                   // The block column is today's glyph cells: one-cell marks, chips and ticks, no strip.
                   { 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0 },
                   { 0, 0, 0 },
                   // A window button and its glyph are one cell, the separator a row; nothing is round.
                   { 1, 1, 1, 1, 0, 0, 0 } }),
          Material::None,
          Material::None,
          ToastStyle::Inverse,
          DialogPlacement::Centered,
          MenuPresentation::InScene,
          TabShape::Flat,
          ProfileMenuAffordance::Caret,
          TabColorSource::Roles,
          ActiveTabWeight::Bold,
          PaneActionsReveal::OnHoverOrFocus,
          FindBarHost::PaneHeaderRow },
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
        CHECK(row.layout.findBar == expected.findBar);

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
            CHECK(glyph.kind == ColorSourceKind::SchemeOnBar);
            CHECK(glyph.slot == SchemeSlot::Warning);
            CHECK(toastFill.kind == ColorSourceKind::Scheme);
            CHECK(toastFill.slot == SchemeSlot::Toast);
            CHECK(toastText.kind == ColorSourceKind::Scheme);
            CHECK(toastText.slot == SchemeSlot::ToastInk);
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
    for (auto const index: allStyles())
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
    for (auto const index: allStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        auto const base = resolveChromeTokens(style, {});
        auto const reduced = resolveChromeTokens(style, preferences);
        CHECK(reduced.motion.fastMs == ReducedMotionCrossFadeMs);
        CHECK(reduced.motion.standardMs == ReducedMotionCrossFadeMs);
        CHECK(reduced.motion.emphasizedMs == ReducedMotionCrossFadeMs);
        CHECK(reduced.motion.travel == 0);
        // A popover opens at full size: the open is a fade only.
        CHECK(reduced.motion.popoverScalePercent == 100);
        // A hold is no animation: the size indicator stays as long as without Reduce Motion.
        CHECK(reduced.motion.lingerMs == base.motion.lingerMs);
        CHECK(reduced.motion.lingerMs == 600);
        CHECK(base.motion.popoverScalePercent == (style == ChromeStyle::Terminal ? 100 : 97));
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
    for (auto const index: allStyles())
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
    for (auto const index: allStyles())
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

        // vtbackend::InkFallbacks in order: the better end of the scheme, ANSI 15, then black or white.
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
    for (auto const index: allStyles())
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

TEST_CASE("chromeTokens.menuRadii", "[chrometokens]")
{
    // The kit's `radius.menu · row`, per row.
    using namespace contour::config;
    struct MenuRadii
    {
        ChromeStyle style;
        int menu;
        int menuRow;
    };
    constexpr auto Expected = std::array {
        MenuRadii { .style = ChromeStyle::MacOS, .menu = 10, .menuRow = 5 },
        MenuRadii { .style = ChromeStyle::Fluent, .menu = 8, .menuRow = 4 },
        MenuRadii { .style = ChromeStyle::ContourLinux, .menu = 10, .menuRow = 6 },
        MenuRadii { .style = ChromeStyle::Terminal, .menu = 0, .menuRow = 0 },
    };
    static_assert(Expected.size() == contour::config::detail::ChromeStyleCount,
                  "one expectation per chrome row");
    for (auto const& expected: Expected)
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const& radii = ChromeTokenRows[static_cast<std::size_t>(expected.style)].radii;
        CHECK(radii.menu == expected.menu);
        CHECK(radii.menuRow == expected.menuRow);
    }
}

TEST_CASE("chromeTokens.menusAreNativeOnlyOnMac", "[chrometokens]")
{
    // R-a of the Step 5a plan: Windows keeps in-scene menus (the swatch row and the terminal scheme's
    // colours survive only there). Reversing that is a deliberate edit of this test.
    using namespace contour::config;
    for (auto const index: allStyles())
    {
        CAPTURE(index);
        auto const isMac = static_cast<ChromeStyle>(index) == ChromeStyle::MacOS;
        CHECK((ChromeTokenRows[index].layout.menu == MenuPresentation::Native) == isMac);
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

TEST_CASE("chromeTokens.theMacAndLinuxRowsArePills", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: allStyles())
    {
        CAPTURE(index);
        auto const style = static_cast<ChromeStyle>(index);
        auto const isPill = style == ChromeStyle::MacOS || style == ChromeStyle::ContourLinux;
        CHECK((ChromeTokenRows[index].layout.tabShape == TabShape::Pill) == isPill);
    }
}

TEST_CASE("chromeTokens.onlyTheFluentRowJoins", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: allStyles())
    {
        CAPTURE(index);
        auto const isFluent = static_cast<ChromeStyle>(index) == ChromeStyle::Fluent;
        CHECK((ChromeTokenRows[index].layout.tabShape == TabShape::Joined) == isFluent);
    }
}

TEST_CASE("chromeTokens.fluentRowMatchesTheWinBoard", "[chrometokens]")
{
    using namespace contour::config;
    auto const fluent = resolveChromeTokens(ChromeStyle::Fluent, {});
    CHECK(fluent.size.titleBar == 40);
    CHECK(fluent.size.tab == 34);
    CHECK(fluent.size.tabIcon == 16);
    CHECK(fluent.size.tabClose == 24);
    CHECK(fluent.size.stripButton == 30);
    CHECK(fluent.size.profileButton == 28);
    CHECK(fluent.space.tabStripLeading == 8);
    CHECK(fluent.radii.tab == 6);
    CHECK(fluent.radii.tabClose == 4);
    CHECK(fluent.radii.paneCard == 4);
    CHECK(fluent.radii.control == 4);
    CHECK(fluent.space.paneGap == 4);
    CHECK(fluent.type.body == 14);
    CHECK(fluent.type.caption == 12);
    CHECK(fluent.layout.tabShape == TabShape::Joined);
    CHECK(fluent.layout.profileMenu == ProfileMenuAffordance::Caret);
}

TEST_CASE("chromeTokens.tabColoursFollowTheRolesWhereTheChromeDrawsItsOwn", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: allStyles())
    {
        CAPTURE(index);
        auto const& layout = ChromeTokenRows[index].layout;
        auto const style = static_cast<ChromeStyle>(index);
        // The native Flat and Joined rows keep today's palette look; the pills and the Terminal row draw the
        // roles.
        auto const native = layout.tabShape != TabShape::Pill && style != ChromeStyle::Terminal;
        CHECK((layout.tabColors == TabColorSource::Palette) == native);
        // Only the Terminal row's active fill can be the bar's own colour, so only it needs the bold.
        CHECK((layout.activeTabWeight == ActiveTabWeight::Bold) == (style == ChromeStyle::Terminal));
    }
}

TEST_CASE("chromeTokens.linuxColumn", "[chrometokens]")
{
    using namespace contour::config;
    // kit-tokens' ContourLinux column and the Linux board, value by value.
    auto const& linuxRow = ChromeTokenRows[static_cast<size_t>(ChromeStyle::ContourLinux)];
    CHECK(linuxRow.size.titleBar == 46);
    CHECK(linuxRow.size.tab == 34);
    CHECK(linuxRow.size.tabWell == 0);
    CHECK(linuxRow.size.control == 34);
    CHECK(linuxRow.size.toolbarButton == 34);
    CHECK(linuxRow.size.findBar == 34);
    CHECK(linuxRow.size.windowButton == 24);
    CHECK(linuxRow.size.windowButtonGlyph == 10);
    CHECK(linuxRow.size.headerIcon == 16);
    CHECK(linuxRow.size.controlsSeparator == 20);
    CHECK(linuxRow.radii.window == 12);
    CHECK(linuxRow.radii.windowButton == 12);
    CHECK(linuxRow.radii.menuRow == 6);
    CHECK(linuxRow.radii.tab == 8);
    CHECK(linuxRow.radii.control == 8);
    CHECK(linuxRow.radii.menu == 10);
    CHECK(linuxRow.radii.popover == 12);
    CHECK(linuxRow.radii.paneCard == 6);
    CHECK(linuxRow.space.paneGap == 6);
    CHECK(linuxRow.space.paneInset == 6);
    CHECK(linuxRow.type.footnote == Catch::Approx(12.5));
    CHECK(linuxRow.type.tab == Catch::Approx(13.5));
    // No blur by default: background_blur stays an opt-in.
    CHECK(linuxRow.material.titleBand == Material::Solid);
}

namespace
{
/// The header bar's tokens one row carries.
struct HeaderBarTokensRow
{
    contour::config::ChromeStyle style = contour::config::ChromeStyle::MacOS;
    int window = 0;
    int headerIcon = 0;
    int windowButton = 0;
    int controlsSeparator = 0;
};
} // namespace

TEST_CASE("chromeTokens.quickTerminalRadiusPerRow", "[chrometokens]")
{
    using namespace contour::config;
    struct Expected
    {
        ChromeStyle style;
        int radius;
    };
    // The canvas's 14 on the macOS row; the other rows' values are the plan's.
    auto const rows = std::array { Expected { .style = ChromeStyle::MacOS, .radius = 14 },
                                   Expected { .style = ChromeStyle::Fluent, .radius = 8 },
                                   Expected { .style = ChromeStyle::ContourLinux, .radius = 12 },
                                   Expected { .style = ChromeStyle::Terminal, .radius = 0 } };
    static_assert(rows.size() == contour::config::detail::ChromeStyleCount);
    for (auto const& expected: rows)
    {
        CAPTURE(static_cast<int>(expected.style));
        CHECK(ChromeTokenRows[static_cast<size_t>(expected.style)].radii.quickTerminal == expected.radius);
    }
}

TEST_CASE("chromeTokens.newFieldsPerRow", "[chrometokens]")
{
    using namespace contour::config;
    using enum ChromeStyle;
    auto const rows = std::array {
        HeaderBarTokensRow {
            .style = MacOS, .window = 0, .headerIcon = 16, .windowButton = 24, .controlsSeparator = 0 },
        HeaderBarTokensRow {
            .style = Fluent, .window = 8, .headerIcon = 16, .windowButton = 24, .controlsSeparator = 0 },
        HeaderBarTokensRow { .style = ContourLinux,
                             .window = 12,
                             .headerIcon = 16,
                             .windowButton = 24,
                             .controlsSeparator = 20 },
        HeaderBarTokensRow {
            .style = Terminal, .window = 0, .headerIcon = 1, .windowButton = 1, .controlsSeparator = 1 },
    };
    static_assert(rows.size() == contour::config::detail::ChromeStyleCount);
    for (auto const& expected: rows)
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const& row = ChromeTokenRows[static_cast<size_t>(expected.style)];
        CHECK(row.radii.window == expected.window);
        CHECK(row.size.headerIcon == expected.headerIcon);
        CHECK(row.size.windowButton == expected.windowButton);
        CHECK(row.size.controlsSeparator == expected.controlsSeparator);
        // ContourLinux is the Linux header bar; every other row keeps 1c's title band.
        CHECK(row.layout.headerBar
              == (expected.style == ChromeStyle::ContourLinux ? HeaderBarArrangement::Unified
                                                              : HeaderBarArrangement::Band));
    }
}

TEST_CASE("chromeTokens.windowButtonFill", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::fill;
    using contour::config::detail::schemeColor;
    for (auto const index: allStyles())
    {
        CAPTURE(index);
        auto const style = static_cast<ChromeStyle>(index);
        auto const terminal = style == ChromeStyle::Terminal;
        auto const& colors = ChromeTokenRows[index].colors;
        auto const expected = terminal ? schemeColor(SchemeSlot::Bar) : fill(3);
        CHECK(colors[static_cast<size_t>(ChromeColorRole::WindowButtonFill)] == expected);
        // Hovered fill.2, pressed fill.1, the glyph the primary label; a cell has no fill, and its ink is the
        // bar's.
        CHECK(colors[static_cast<size_t>(ChromeColorRole::WindowButtonHoverFill)]
              == (terminal ? schemeColor(SchemeSlot::Bar) : fill(2)));
        CHECK(colors[static_cast<size_t>(ChromeColorRole::WindowButtonPressedFill)]
              == (terminal ? schemeColor(SchemeSlot::Bar) : fill(1)));
        CHECK(colors[static_cast<size_t>(ChromeColorRole::WindowButtonGlyph)]
              == (terminal ? schemeColor(SchemeSlot::BarInk) : contour::config::detail::label(1)));
    }
    CHECK(ChromeTokenRows[static_cast<size_t>(ChromeStyle::ContourLinux)]
              .colors[static_cast<size_t>(ChromeColorRole::TabActiveFill)]
          == fill(3));
}

TEST_CASE("chromeTokens.reducedMotionCrossFades", "[chrometokens]")
{
    using namespace contour::config;
    auto const reduced = AccessibilityPreferences { .motion = MotionPreference::Reduced,
                                                    .transparency = TransparencyPreference::Allowed,
                                                    .contrast = ContrastPreference::Standard };
    for (auto const index: allStyles())
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
        BandColorRow {
            .role = CaptionWash, .native = fill(4), .terminal = contour::config::detail::noColor() },
        BandColorRow {
            .role = CaptionGlyphPressed, .native = label(2), .terminal = schemeColor(SchemeSlot::BarInk) },
        BandColorRow { .role = CaptionGlyphInactive,
                       .native = label(3),
                       .terminal = contour::config::detail::schemeOnBar(SchemeSlot::Secondary) },
    };
    // Every role from TabWellFill up to the pane roles has a row here (chromeTokens.paneColourRoles has
    // those).
    CHECK(rows.size() == static_cast<size_t>(PaneGapFill) - static_cast<size_t>(TabWellFill));

    for (auto const index: allStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const& row: rows)
        {
            CAPTURE(static_cast<int>(row.role));
            auto expected = style == ChromeStyle::Terminal ? row.terminal : row.native;
            // The Linux board's active tab is white 11 % on the bar, not the kit's elevated surface.
            if (style == ChromeStyle::ContourLinux && row.role == TabActiveFill)
                expected = fill(3);
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
    for (auto const index: allStyles())
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

namespace
{

/// The shell-integration card's four tokens on one row (R-l).
struct NoticeCardTokenRow
{
    contour::config::ChromeStyle style = contour::config::ChromeStyle::MacOS;
    int width = 0;
    int radius = 0;
    int padX = 0;
    int padY = 0;
};

/// One row per ChromeStyle, in enumerator order.
constexpr auto NoticeCardTokenRows = std::array {
    NoticeCardTokenRow {
        .style = contour::config::ChromeStyle::MacOS, .width = 330, .radius = 12, .padX = 14, .padY = 12 },
    NoticeCardTokenRow {
        .style = contour::config::ChromeStyle::Fluent, .width = 330, .radius = 8, .padX = 14, .padY = 12 },
    NoticeCardTokenRow { .style = contour::config::ChromeStyle::ContourLinux,
                         .width = 330,
                         .radius = 12,
                         .padX = 14,
                         .padY = 12 },
    NoticeCardTokenRow {
        .style = contour::config::ChromeStyle::Terminal, .width = 0, .radius = 0, .padX = 1, .padY = 0 },
};

} // namespace

TEST_CASE("chromeTokens.noticeCard", "[chrometokens]")
{
    static_assert(NoticeCardTokenRows.size() == contour::config::ChromeTokenRows.size());
    for (auto const& expected: NoticeCardTokenRows)
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const row = contour::config::resolveChromeTokens(expected.style, {});
        CHECK(row.size.noticeCard == expected.width);
        CHECK(row.radii.noticeCard == expected.radius);
        CHECK(row.space.noticeCardPadX == expected.padX);
        CHECK(row.space.noticeCardPadY == expected.padY);
    }
}

TEST_CASE("chromeTokens.unsupportedMaterialIsSolid", "[chrometokens]")
{
    using namespace contour::config;
    constexpr auto Comfortable = ChromeDensity::Comfortable;
    // Without OS support for its backdrop, the Fluent band's Mica is Solid, as under Reduced transparency.
    CHECK(resolveChromeTokens(ChromeStyle::Fluent, {}, Comfortable, MaterialSupport::Unsupported)
              .material.titleBand
          == Material::Solid);
    for (auto const materials: { MaterialSupport::Supported, MaterialSupport::Unknown })
        CHECK(resolveChromeTokens(ChromeStyle::Fluent, {}, Comfortable, materials).material.titleBand
              == Material::Mica);
    // Both slots: the macOS popover's Glass too.
    CHECK(resolveChromeTokens(ChromeStyle::MacOS, {}, Comfortable).material.popover == Material::Glass);
    CHECK(resolveChromeTokens(ChromeStyle::MacOS, {}, Comfortable, MaterialSupport::Unsupported)
              .material.popover
          == Material::Solid);
    // A row without a material keeps none.
    CHECK(resolveChromeTokens(ChromeStyle::Terminal, {}, Comfortable, MaterialSupport::Unsupported)
              .material.titleBand
          == Material::None);
}

TEST_CASE("chromeTokens.defaultMaterialSupportIsUnknown", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const index: allStyles())
        for (auto const transparency: { TransparencyPreference::Allowed, TransparencyPreference::Reduced })
        {
            CAPTURE(index, static_cast<int>(transparency));
            auto const style = static_cast<ChromeStyle>(index);
            auto const preferences = AccessibilityPreferences { .motion = MotionPreference::Full,
                                                                .transparency = transparency,
                                                                .contrast = ContrastPreference::Standard };
            auto const defaulted = resolveChromeTokens(style, preferences, ChromeDensity::Comfortable);
            auto const unknown =
                resolveChromeTokens(style, preferences, ChromeDensity::Comfortable, MaterialSupport::Unknown);
            CHECK(defaulted.material == unknown.material);
            CHECK(numbersOf(defaulted) == numbersOf(unknown));
            CHECK(defaulted.layout == unknown.layout);
            CHECK(defaulted.colors == unknown.colors);
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

    for (auto const index: allStyles())
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
    for (auto const index: allStyles())
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
        PaneColorRow { .role = PaneHeaderActionHover, .native = fill(1), .terminal = transparent },
    };
    // Every pane role has a row here.
    CHECK(rows.size() == PaneColorRoleEnd - PaneColorRoleBegin);

    for (auto const index: allStyles())
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

TEST_CASE("chromeTokens.findColourRoles", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::label;
    using contour::config::detail::schemeColor;
    using contour::config::detail::schemeOnBar;
    using enum ChromeColorRole;
    auto const rows = std::array {
        // The find bar (F13): a lit toggle is an accent wash under the primary label, ringed in the accent
        // (UiStyleProvider::statusMarkOn), or its cells in inverse video with no ring; an unlit one and the
        // count, the secondary label or the bar's inks.
        PaneColorRow { .role = FindToggleOnFill,
                       .native = ChromeColorSource { .kind = ColorSourceKind::StatusMark,
                                                     .rgb = 0,
                                                     .status = StatusColorRole::Running,
                                                     .slot = SchemeSlot::Background,
                                                     .alphaPercent = 22,
                                                     .level = 0 },
                       .terminal = schemeColor(SchemeSlot::BarInk) },
        PaneColorRow {
            .role = FindToggleOnText, .native = label(1), .terminal = schemeColor(SchemeSlot::Bar) },
        PaneColorRow {
            .role = FindToggleText, .native = label(2), .terminal = schemeOnBar(SchemeSlot::Secondary) },
        PaneColorRow {
            .role = FindCountText, .native = label(2), .terminal = schemeColor(SchemeSlot::BarInk) },
        PaneColorRow { .role = FindToggleOnRing,
                       .native = ChromeColorSource { .kind = ColorSourceKind::StatusMarkOnWindow,
                                                     .rgb = 0,
                                                     .status = StatusColorRole::Running,
                                                     .slot = SchemeSlot::Background,
                                                     .alphaPercent = 100,
                                                     .level = 0 },
                       .terminal = contour::config::detail::noColor() },
        PaneColorRow { .role = FindErrorText,
                       .native = ChromeColorSource { .kind = ColorSourceKind::Status,
                                                     .rgb = 0,
                                                     .status = StatusColorRole::Danger,
                                                     .slot = SchemeSlot::Background,
                                                     .alphaPercent = 100,
                                                     .level = 0 },
                       .terminal = schemeOnBar(SchemeSlot::Danger) },
    };
    // Every find bar role has a row here.
    CHECK(rows.size() == FindColorRoleEnd - FindColorRoleBegin);

    for (auto const index: allStyles())
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
}

TEST_CASE("chromeTokens.toastReadsOnEveryScheme", "[chrometokens]")
{
    using namespace contour::config;
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        CHECK(contrastRatio(slotIn(colors, SchemeSlot::ToastInk), slotIn(colors, SchemeSlot::Toast))
              >= MinimumTextContrast);
        // Inverse video where it reads, as the row's toast style says; the bar where it does not.
        if (contrastRatio(palette.defaultBackground, palette.defaultForeground) >= MinimumTextContrast)
            CHECK(slotIn(colors, SchemeSlot::Toast) == palette.defaultForeground);
        else
            CHECK(slotIn(colors, SchemeSlot::Toast) == slotIn(colors, SchemeSlot::Bar));
    }
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

TEST_CASE("chromeTokens.paletteColourRoles", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::label;
    using contour::config::detail::schemeColor;
    using contour::config::detail::schemeOnBar;
    using contour::config::detail::separator;
    using contour::config::detail::sourceOf;
    using contour::config::detail::surface;
    using enum ChromeColorRole;

    struct PaletteColorRow
    {
        ChromeColorRole role = PaletteSurface;
        ChromeColorSource native {};
        ChromeColorSource terminal {};
    };
    auto const rows = std::array {
        PaletteColorRow {
            .role = PaletteSurface, .native = surface(1), .terminal = schemeColor(SchemeSlot::Bar) },
        PaletteColorRow {
            .role = PaletteText, .native = label(1), .terminal = schemeColor(SchemeSlot::BarInk) },
        PaletteColorRow { .role = PaletteSecondaryText,
                          .native = label(2),
                          .terminal = schemeOnBar(SchemeSlot::Secondary) },
        PaletteColorRow { .role = PaletteSelectionFill,
                          .native = sourceOf(ColorSourceKind::PaletteHighlight),
                          .terminal = schemeColor(SchemeSlot::BarSelection) },
        PaletteColorRow { .role = PaletteSelectionText,
                          .native = sourceOf(ColorSourceKind::PaletteHighlightedText),
                          .terminal = schemeColor(SchemeSlot::BarSelectionText) },
        PaletteColorRow {
            .role = PaletteSeparator, .native = separator(), .terminal = schemeColor(SchemeSlot::Secondary) },
    };
    // Every palette role has a row here.
    CHECK(rows.size() == PaletteColorRoleEnd - PaletteColorRoleBegin);

    for (auto const index: allStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const& row: rows)
        {
            CAPTURE(static_cast<int>(row.role));
            auto const& expected = style == ChromeStyle::Terminal ? row.terminal : row.native;
            CHECK(resolveChromeTokens(style, {}).colors[static_cast<size_t>(row.role)] == expected);
        }
    }
}

TEST_CASE("chromeTokens.paletteTextReads", "[chrometokens]")
{
    // The Terminal row's palette inks against their grounds, on every bundled scheme. The native rows'
    // inks are the kit's labels on its elevated surface, and the OS's own selected-row pair.
    using namespace contour::config;
    using enum ChromeColorRole;
    auto const row = resolveChromeTokens(ChromeStyle::Terminal, {});
    auto const slotOf = [&](ChromeColorRole role) {
        return row.colors[static_cast<size_t>(role)].slot;
    };
    auto const pairs = std::array { std::pair { PaletteText, PaletteSurface },
                                    std::pair { PaletteSecondaryText, PaletteSurface },
                                    std::pair { PaletteSelectionText, PaletteSelectionFill } };
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        for (auto const& [ink, ground]: pairs)
        {
            CAPTURE(static_cast<int>(ink));
            auto const& source = row.colors[static_cast<size_t>(ink)];
            auto const inkColor = source.kind == ColorSourceKind::SchemeOnBar
                                      ? legibleOnBar(colors, source.slot)
                                      : slotIn(colors, slotOf(ink));
            CHECK(contrastRatio(inkColor, slotIn(colors, slotOf(ground))) >= MinimumTextContrast);
        }
    }
}

TEST_CASE("chromeTokens.barSelectionStandsOffTheBar", "[chrometokens]")
{
    // A selected row drawn on the bar (the Terminal row's command palette) is a mark against the bar as
    // well as a ground for its text, on every bundled scheme: the plain Selection may be the bar itself
    // (one-dark) and so mark nothing.
    using namespace contour::config;
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        auto const bar = slotIn(colors, SchemeSlot::Bar);
        auto const fill = slotIn(colors, SchemeSlot::BarSelection);
        auto const text = slotIn(colors, SchemeSlot::BarSelectionText);
        CHECK(contrastRatio(text, fill) >= MinimumTextContrast);
        CHECK(contrastRatio(fill, bar) >= MinimumMarkContrast);

        // The chrome's own selection is kept wherever it already marks on the bar.
        auto const selection = slotIn(colors, SchemeSlot::Selection);
        if (contrastRatio(selection, bar) >= MinimumMarkContrast)
        {
            CHECK(fill == selection);
            CHECK(text == slotIn(colors, SchemeSlot::SelectionText));
        }
    }
}

TEST_CASE("chromeTokens.overlayColourRoles", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::noColor;
    using contour::config::detail::schemeColor;
    using enum ChromeColorRole;

    struct OverlayColorRow
    {
        ChromeColorRole role = ToastTextSecondary;
        ChromeColorSource native {};
        ChromeColorSource terminal {};
    };
    // The kit's label.1 at a fraction of its own alpha (LabelAlphas): 33 % of 88 / 92 is about 30 %.
    auto const labelAt = [](int alphaPercent) {
        return ChromeColorSource { .kind = ColorSourceKind::Label,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = alphaPercent,
                                   .level = 1 };
    };
    auto const rows = std::array {
        OverlayColorRow { .role = ToastTextSecondary,
                          .native = ChromeColorSource { .kind = ColorSourceKind::Fixed,
                                                        .rgb = 0xFFFFFF,
                                                        .status = StatusColorRole::Danger,
                                                        .slot = SchemeSlot::Background,
                                                        .alphaPercent = 60,
                                                        .level = 0 },
                          .terminal = schemeColor(SchemeSlot::ToastHint) },
        OverlayColorRow { .role = ScrollbarThumb,
                          .native = labelAt(33),
                          .terminal = schemeColor(SchemeSlot::ScrollbarThumb) },
        OverlayColorRow { .role = ScrollbarThumbActive,
                          .native = labelAt(55),
                          .terminal = schemeColor(SchemeSlot::ScrollbarThumbActive) },
        OverlayColorRow { .role = ScrollbarTrack, .native = labelAt(11), .terminal = noColor() },
    };
    // Every overlay role has a row here.
    CHECK(rows.size() == OverlayColorRoleEnd - OverlayColorRoleBegin);

    for (auto const index: allStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const& row: rows)
        {
            CAPTURE(static_cast<int>(row.role));
            auto const& expected = style == ChromeStyle::Terminal ? row.terminal : row.native;
            CHECK(resolveChromeTokens(style, {}).colors[static_cast<size_t>(row.role)] == expected);
        }
    }

    // What the label sources resolve to: their percentage of label.1's light and dark alpha, about 30, 50 and
    // 10 % on either chrome (the board's values).
    struct ResolvedAlpha
    {
        ChromeColorRole role;
        int light;
        int dark;
    };
    auto const native = resolveChromeTokens(ChromeStyle::MacOS, {});
    for (auto const& [role, light, dark]: std::array { ResolvedAlpha { ScrollbarThumb, 29, 30 },
                                                       ResolvedAlpha { ScrollbarThumbActive, 48, 50 },
                                                       ResolvedAlpha { ScrollbarTrack, 9, 10 } })
    {
        CAPTURE(static_cast<int>(role));
        auto const& source = native.colors[static_cast<size_t>(role)];
        auto const& pair = LabelAlphas[static_cast<size_t>(source.level - 1)];
        CHECK(pair.light * source.alphaPercent / 100 == light);
        CHECK(pair.dark * source.alphaPercent / 100 == dark);
    }
}

TEST_CASE("chromeTokens.focusRingTokens", "[chrometokens]")
{
    using namespace contour::config;
    using enum ChromeColorRole;

    // The outset per row: the board's 3 pt on the Mac, 2 on the other native rows, none in cells.
    struct RingRow
    {
        ChromeStyle style;
        int outset;
    };
    for (auto const& [style, outset]: std::array { RingRow { ChromeStyle::MacOS, 3 },
                                                   RingRow { ChromeStyle::Fluent, 2 },
                                                   RingRow { ChromeStyle::ContourLinux, 2 },
                                                   RingRow { ChromeStyle::Terminal, 0 } })
    {
        CAPTURE(static_cast<int>(style));
        CHECK(ChromeTokenRows[static_cast<size_t>(style)].size.focusRing == outset);
    }

    // The two roles are a family of their own, the Settings window's follow them, and the window
    // controls' role closes the table.
    CHECK(FocusColorRoleEnd - FocusColorRoleBegin == 2);
    CHECK(FocusColorRoleEnd == SettingsColorRoleBegin);
    CHECK(SettingsColorRoleEnd == static_cast<size_t>(ChromeColorRole::WindowButtonFill));
    CHECK(static_cast<size_t>(ChromeColorRole::HeaderSeparator) + 1
          == static_cast<size_t>(ChromeColorRole::TooltipFill));
    CHECK(static_cast<size_t>(ChromeColorRole::TooltipEdge) + 1 == ChromeColorRoleCount);

    // Native: a ring that marks on the window and no fill. Terminal: no ring, the bar's selection as fill.
    auto const none = contour::config::detail::noColor();
    for (auto const index: allStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        auto const& colors = ChromeTokenRows[index].colors;
        if (style == ChromeStyle::Terminal)
        {
            CHECK(colors[static_cast<size_t>(FocusRing)] == none);
            CHECK(colors[static_cast<size_t>(FocusFill)]
                  == contour::config::detail::schemeColor(SchemeSlot::BarSelection));
        }
        else
        {
            CHECK(colors[static_cast<size_t>(FocusRing)].kind == ColorSourceKind::StatusMarkOnWindow);
            CHECK(colors[static_cast<size_t>(FocusRing)].status == StatusColorRole::Running);
            CHECK(colors[static_cast<size_t>(FocusFill)] == none);
        }
    }
}

TEST_CASE("chromeTokens.theLinkPillsHintReads", "[chrometokens]")
{
    // The hint sits on the Toast's fill, which sits on the pane: on the native rows, over every bundled
    // scheme's background, it reads at 4.5:1 -- the reason it is 60 % rather than the board's 50 % (R-h).
    using namespace contour::config;
    using enum ChromeColorRole;
    auto const over = [](ChromeColorSource const& source, RGBColor ground) {
        return mixColor(ground, RGBColor(source.rgb), static_cast<float>(source.alphaPercent) / 100.0f);
    };
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        for (auto const index: allStyles())
        {
            auto const style = static_cast<ChromeStyle>(index);
            CAPTURE(index);
            auto const row = resolveChromeTokens(style, {});
            auto const& fill = row.colors[static_cast<size_t>(ToastFill)];
            auto const& hint = row.colors[static_cast<size_t>(ToastTextSecondary)];
            if (style == ChromeStyle::Terminal)
            {
                // The toast's text and the hint after it are both inks that read on the toast's fill (Step
                // 3c's Toast/ToastInk pair): inverse video where it reads, a bar strip where not.
                auto const& text = row.colors[static_cast<size_t>(ToastText)];
                REQUIRE(fill.kind == ColorSourceKind::Scheme);
                REQUIRE(text.kind == ColorSourceKind::Scheme);
                REQUIRE(hint.kind == ColorSourceKind::Scheme);
                auto const colors = chromeSchemeColorsOf(palette);
                CHECK(contrastRatio(slotIn(colors, text.slot), slotIn(colors, fill.slot))
                      >= MinimumTextContrast);
                CHECK(contrastRatio(slotIn(colors, hint.slot), slotIn(colors, fill.slot))
                      >= MinimumTextContrast);
                continue;
            }
            REQUIRE(fill.kind == ColorSourceKind::Fixed);
            REQUIRE(hint.kind == ColorSourceKind::Fixed);
            auto const toast = over(fill, palette.defaultBackground);
            CHECK(contrastRatio(over(hint, toast), toast) >= MinimumTextContrast);
        }
    }
}

TEST_CASE("chromeTokens.theTerminalRowsThumbMarksOnEveryPane", "[chrometokens]")
{
    // A pane's profile may name another scheme than the default profile's: the thumb, chosen from the
    // default's slots, still stands off that pane's background at 3:1 -- for every pair of bundled schemes.
    using namespace contour::config;
    auto const schemeOf = [](std::string_view name) {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        return palette;
    };
    for (auto const defaultName: defaultColorPaletteNames())
    {
        auto const colors = chromeSchemeColorsOf(schemeOf(defaultName));
        for (auto const paneName: defaultColorPaletteNames())
        {
            INFO("default: " << defaultName << ", pane: " << paneName);
            auto const ground = schemeOf(paneName).defaultBackground;
            for (auto const& chain: SchemeMarkChains)
                CHECK(contrastRatio(markOn(colors, chain.slot, ground), ground) >= MinimumMarkContrast);
            // Not a mark: the scheme's own, on whatever ground.
            CHECK(markOn(colors, SchemeSlot::Accent, ground) == slotIn(colors, SchemeSlot::Accent));
        }
        // On the default's own background, the slot as chromeSchemeColorsOf() chose it.
        for (auto const& chain: SchemeMarkChains)
            CHECK(markOn(colors, chain.slot, schemeOf(defaultName).defaultBackground)
                  == slotIn(colors, chain.slot));
    }
}

TEST_CASE("chromeTokens.theSearchTickMarksOnTheTrack", "[chrometokens]")
{
    // The search tick is the scheme's yellow, which a light scheme's background swallows: on every bundled
    // scheme, every chrome row and both appearances, it stands off the pane and off the track drawn over it
    // at 3:1 -- the track being the row's scrollbar-track label ink, over the pane, at that appearance's
    // alpha.
    using namespace contour::config;
    using enum ChromeColorRole;
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const tick = vtbackend::scrollbarMarkColor(palette, vtbackend::ScrollbarMarkKind::SearchMatch);
        CHECK(contrastRatio(tick, palette.defaultBackground) >= MinimumMarkContrast);
        for (auto const index: allStyles())
        {
            auto const style = static_cast<ChromeStyle>(index);
            CAPTURE(index);
            auto const row = resolveChromeTokens(style, {});
            auto const& source = row.colors[static_cast<size_t>(ScrollbarTrack)];
            if (source.kind != ColorSourceKind::Label)
                continue; // The Terminal row draws no track: the tick sits on the pane itself.
            auto const& pair = LabelAlphas[static_cast<size_t>(source.level - 1)];
            for (auto const dark: { false, true })
            {
                CAPTURE(dark);
                // As the resolver computes it: a whole percentage.
                auto const wholePercent = (dark ? pair.dark : pair.light) * source.alphaPercent / 100;
                auto const alpha = static_cast<float>(wholePercent) / 100.0f;
                // The tint the fallback leaves room for covers every track the rows draw.
                CHECK(alpha <= vtbackend::ScrollbarTrackMaxTint);
                auto const ink = dark ? RGBColor { 0xFF, 0xFF, 0xFF } : RGBColor { 0x00, 0x00, 0x00 };
                auto const track = mixColor(palette.defaultBackground, ink, alpha);
                CHECK(contrastRatio(tick, track) >= MinimumMarkContrast);
            }
        }
    }
}

TEST_CASE("chromeTokens.theTerminalRowsScrollbarThumbsMark", "[chrometokens]")
{
    // The Terminal row's thumb is a column of "┃" on the pane: at rest and engaged, it stands off the pane's
    // background at 3:1 on every bundled scheme -- ANSI 8 or the accent where they do, a fallback where not.
    using namespace contour::config;
    using enum ChromeColorRole;
    auto const row = resolveChromeTokens(ChromeStyle::Terminal, {});
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const colors = chromeSchemeColorsOf(palette);
        for (auto const role: { ScrollbarThumb, ScrollbarThumbActive })
        {
            CAPTURE(static_cast<int>(role));
            auto const& source = row.colors[static_cast<size_t>(role)];
            REQUIRE(source.kind == ColorSourceKind::Scheme);
            CHECK(contrastRatio(slotIn(colors, source.slot), palette.defaultBackground)
                  >= MinimumMarkContrast);
        }
        // ANSI 8 and the accent are kept wherever they already mark.
        auto const secondary = slotIn(colors, SchemeSlot::Secondary);
        if (contrastRatio(secondary, palette.defaultBackground) >= MinimumMarkContrast)
            CHECK(slotIn(colors, SchemeSlot::ScrollbarThumb) == secondary);
        auto const accent = slotIn(colors, SchemeSlot::Accent);
        if (contrastRatio(accent, palette.defaultBackground) >= MinimumMarkContrast)
            CHECK(slotIn(colors, SchemeSlot::ScrollbarThumbActive) == accent);
    }
}

TEST_CASE("chromeTokens.blockColumnFormFollowsTheRow", "[chrometokens]")
{
    // The native rows draw the block column as a pixel strip; the Terminal row keeps the glyph cells.
    using namespace contour::config;
    for (auto const i: std::views::iota(0uz, ChromeTokenRows.size()))
    {
        CAPTURE(i);
        auto const expected = static_cast<ChromeStyle>(i) == ChromeStyle::Terminal
                                  ? vtbackend::BlockColumnForm::Glyphs
                                  : vtbackend::BlockColumnForm::Rail;
        CHECK(ChromeTokenRows[i].layout.blockColumn == expected);
    }
}

TEST_CASE("chromeTokens.blockChipsAreTheCanvasPills", "[chrometokens]")
{
    // Every native row draws the canvas's pills; the Terminal row none, its chips being terminal text. The
    // fold chip's height and radius are the row's size.foldChip and radius.chip, which the hit test and the
    // QML read too.
    using namespace contour::config;
    using vtbackend::PillStyle;
    for (auto const i: std::views::iota(0uz, ChromeTokenRows.size()))
    {
        CAPTURE(i);
        auto const& row = ChromeTokenRows[i];
        if (static_cast<ChromeStyle>(i) == ChromeStyle::Terminal)
        {
            CHECK(row.blockChips == BlockChipTokens {});
            continue;
        }
        auto const& pills = row.blockChips.pills;
        auto const& failed = pills[static_cast<std::size_t>(PillStyle::Failed)];
        CHECK(failed == PillTokens { .type = 11, .weight = 600, .padX = 8, .padY = 1, .radius = 9 });
        CHECK(pills[static_cast<std::size_t>(PillStyle::Running)] == failed);
        CHECK(pills[static_cast<std::size_t>(PillStyle::Success)]
              == PillTokens { .type = 11, .weight = 400 });
        auto const& fold = pills[static_cast<std::size_t>(PillStyle::Fold)];
        CHECK(fold.type == 11);
        CHECK(fold.weight == 400);
        CHECK(fold.padX == 10);
        CHECK(fold.padY == 0);
        CHECK(fold.ring == 0.5);
        CHECK(fold.height == row.size.foldChip);
        CHECK(fold.radius == row.radii.chip);
        CHECK(row.blockChips.markType == 9);
        CHECK(row.blockChips.markWeight == 700);
        CHECK(row.blockChips.hoverRing == 1);
        CHECK(row.size.railMark == 13);
    }
}

// {{{ The Settings window's tokens (Step 4b, Task 8)

namespace
{
/// One row of the Settings token table (the plan's Global Constraints): the layout, the traits and every
/// extent, written out as the board has them so the header's own tables cannot agree with themselves.
struct SettingsTokenRow
{
    contour::config::ChromeStyle style = contour::config::ChromeStyle::MacOS;
    contour::config::SettingsLayout layout = contour::config::SettingsLayout::InsetGrouped;
    contour::config::SettingsTokens tokens {};
};

[[nodiscard]] auto settingsTokenRows()
{
    using namespace contour::config;
    return std::array {
        SettingsTokenRow { .style = ChromeStyle::MacOS,
                           .layout = SettingsLayout::InsetGrouped,
                           .tokens = { .sidebarWidth = 230,
                                       .sidebarRow = 28,
                                       .iconTile = 20,
                                       .iconGlyph = 12,
                                       .searchField = 28,
                                       .headerBar = 52,
                                       .listMaxWidth = 640,
                                       .groupRadius = 10,
                                       .row = 44,
                                       .rowWithHelp = 52,
                                       .rowPadding = 14,
                                       .cardGap = 0,
                                       .groupGap = 16,
                                       .groupTitleGap = 6,
                                       .control = 22,
                                       .switchWidth = 32,
                                       .switchHeight = 19,
                                       .slider = 160,
                                       .stepperField = 72,
                                       .colorWell = 22,
                                       .previewHeight = 104,
                                       .previewWidth = 0,
                                       .previewRadius = 10,
                                       .disabledPercent = 35,
                                       .labelType = 13,
                                       .helpType = 11,
                                       .groupTitleType = 13,
                                       .pageTitleType = 15 } },
        SettingsTokenRow { .style = ChromeStyle::Fluent,
                           .layout = SettingsLayout::Cards,
                           .tokens = { .sidebarWidth = 290,
                                       .sidebarRow = 36,
                                       .iconTile = 0,
                                       .iconGlyph = 16,
                                       .searchField = 32,
                                       .headerBar = 64,
                                       .listMaxWidth = 0,
                                       .groupRadius = 4,
                                       .row = 68,
                                       .rowWithHelp = 68,
                                       .rowPadding = 16,
                                       .cardGap = 4,
                                       .groupGap = 20,
                                       .groupTitleGap = 6,
                                       .control = 32,
                                       .switchWidth = 40,
                                       .switchHeight = 20,
                                       .slider = 180,
                                       .stepperField = 120,
                                       .colorWell = 32,
                                       .previewHeight = 128,
                                       .previewWidth = 360,
                                       .previewRadius = 6,
                                       .disabledPercent = 35,
                                       .labelType = 14,
                                       .helpType = 12,
                                       .groupTitleType = 14,
                                       .pageTitleType = 28 } },
        SettingsTokenRow { .style = ChromeStyle::ContourLinux,
                           .layout = SettingsLayout::BoxedList,
                           .tokens = { .sidebarWidth = 270,
                                       .sidebarRow = 38,
                                       .iconTile = 0,
                                       .iconGlyph = 16,
                                       .searchField = 34,
                                       .headerBar = 46,
                                       .listMaxWidth = 640,
                                       .groupRadius = 12,
                                       .row = 52,
                                       .rowWithHelp = 60,
                                       .rowPadding = 14,
                                       .cardGap = 0,
                                       .groupGap = 22,
                                       .groupTitleGap = 10,
                                       .control = 34,
                                       .switchWidth = 40,
                                       .switchHeight = 22,
                                       .slider = 160,
                                       .stepperField = 72,
                                       .colorWell = 34,
                                       .previewHeight = 120,
                                       .previewWidth = 0,
                                       .previewRadius = 12,
                                       .disabledPercent = 35,
                                       .labelType = 14,
                                       .helpType = 12.5,
                                       .groupTitleType = 14,
                                       .pageTitleType = 15 } },
        SettingsTokenRow { .style = ChromeStyle::Terminal,
                           .layout = SettingsLayout::Cells,
                           .tokens = { .sidebarWidth = 24,
                                       .sidebarRow = 1,
                                       .iconTile = 0,
                                       .iconGlyph = 1,
                                       .searchField = 1,
                                       .headerBar = 1,
                                       .listMaxWidth = 80,
                                       .groupRadius = 0,
                                       .row = 1,
                                       .rowWithHelp = 2,
                                       .rowPadding = 1,
                                       .cardGap = 0,
                                       .groupGap = 1,
                                       .groupTitleGap = 0,
                                       .control = 1,
                                       .switchWidth = 3,
                                       .switchHeight = 1,
                                       .slider = 11,
                                       .stepperField = 7,
                                       .colorWell = 2,
                                       .previewHeight = 6,
                                       .previewWidth = 0,
                                       .previewRadius = 0,
                                       .disabledPercent = 35,
                                       .labelType = 0,
                                       .helpType = 0,
                                       .groupTitleType = 0,
                                       .pageTitleType = 0 } },
    };
}
} // namespace

TEST_CASE("chromeTokens.settingsTokensPerRow", "[chrometokens]")
{
    using namespace contour::config;
    auto const rows = settingsTokenRows();
    // Every row has its line in the table.
    CHECK(rows.size() == contour::config::detail::ChromeStyleCount);
    for (auto const& expected: rows)
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const row = resolveChromeTokens(expected.style, {});
        CHECK(row.settings == expected.tokens);
        CHECK(row.layout.settings == expected.layout);
    }
}

TEST_CASE("chromeTokens.settingsLayoutTraits", "[chrometokens]")
{
    using namespace contour::config;
    struct TraitRow
    {
        SettingsLayout layout = SettingsLayout::InsetGrouped;
        SettingsLayoutTraits traits {};
    };
    auto const rows = std::array {
        TraitRow { .layout = SettingsLayout::InsetGrouped,
                   .traits = { .header = SettingsHeader::Toolbar,
                               .rows = SettingsRowShape::Grouped,
                               .stepper = StepperArrangement::Stacked } },
        // Fluent's stepper is one field with both arrows inside it (preflight F10, ruling Q5).
        TraitRow { .layout = SettingsLayout::Cards,
                   .traits = { .header = SettingsHeader::Breadcrumb,
                               .rows = SettingsRowShape::SeparateCards,
                               .stepper = StepperArrangement::Inline } },
        TraitRow { .layout = SettingsLayout::BoxedList,
                   .traits = { .header = SettingsHeader::SplitHeaderBar,
                               .rows = SettingsRowShape::Grouped,
                               .stepper = StepperArrangement::SideBySide } },
        TraitRow { .layout = SettingsLayout::Cells,
                   .traits = { .header = SettingsHeader::Toolbar,
                               .rows = SettingsRowShape::Grouped,
                               .stepper = StepperArrangement::Stacked } },
    };
    CHECK(rows.size() == SettingsLayoutCount);
    for (auto const& row: rows)
    {
        CAPTURE(static_cast<int>(row.layout));
        CHECK(settingsLayoutTraits(row.layout) == row.traits);
    }
}

TEST_CASE("chromeTokens.settingsColourRolesPerRow", "[chrometokens]")
{
    using namespace contour::config;
    using contour::config::detail::fill;
    using contour::config::detail::label;
    using contour::config::detail::noColor;
    using contour::config::detail::schemeColor;
    using contour::config::detail::schemeOnBar;
    using contour::config::detail::separator;
    using contour::config::detail::sourceOf;
    using contour::config::detail::statusMarkSource;
    using contour::config::detail::surface;
    using enum ChromeColorRole;

    struct SettingsColorRow
    {
        ChromeColorRole role = SettingsSidebarFill;
        ChromeColorSource native {};
        ChromeColorSource terminal {};
    };
    auto const white = ChromeColorSource { .kind = ColorSourceKind::Fixed, .rgb = 0xFFFFFF };
    auto const rows = std::array {
        SettingsColorRow {
            .role = SettingsSidebarFill, .native = surface(2), .terminal = schemeColor(SchemeSlot::Bar) },
        SettingsColorRow { .role = SettingsPageFill,
                           .native = sourceOf(ColorSourceKind::PaletteWindow),
                           .terminal = schemeColor(SchemeSlot::Bar) },
        SettingsColorRow {
            .role = SettingsGroupFill, .native = surface(1), .terminal = schemeColor(SchemeSlot::Bar) },
        SettingsColorRow { .role = SettingsSeparator,
                           .native = separator(),
                           .terminal = schemeColor(SchemeSlot::Secondary) },
        SettingsColorRow { .role = SettingsLabel,
                           .native = sourceOf(ColorSourceKind::PaletteWindowText),
                           .terminal = schemeColor(SchemeSlot::BarInk) },
        SettingsColorRow {
            .role = SettingsHelp, .native = label(2), .terminal = schemeOnBar(SchemeSlot::Secondary) },
        SettingsColorRow { .role = SettingsSelectionFill,
                           .native = sourceOf(ColorSourceKind::PaletteHighlight),
                           .terminal = schemeColor(SchemeSlot::BarSelection) },
        SettingsColorRow { .role = SettingsSelectionText,
                           .native = sourceOf(ColorSourceKind::PaletteHighlightedText),
                           .terminal = schemeColor(SchemeSlot::BarSelectionText) },
        SettingsColorRow {
            .role = SettingsModified, .native = label(1), .terminal = schemeOnBar(SchemeSlot::Accent) },
        SettingsColorRow { .role = SettingsIconTile, .native = fill(3), .terminal = noColor() },
        SettingsColorRow {
            .role = SettingsIconGlyph, .native = label(2), .terminal = schemeColor(SchemeSlot::BarInk) },
        SettingsColorRow { .role = SwitchOnFill,
                           .native = statusMarkSource(StatusColorRole::Running),
                           .terminal = schemeOnBar(SchemeSlot::Accent) },
        SettingsColorRow {
            .role = SwitchOffFill, .native = label(3, 95), .terminal = schemeOnBar(SchemeSlot::Secondary) },
        SettingsColorRow { .role = SwitchKnob, .native = white, .terminal = schemeColor(SchemeSlot::BarInk) },
        SettingsColorRow {
            .role = ControlFill, .native = fill(4), .terminal = schemeColor(SchemeSlot::Background) },
        SettingsColorRow {
            .role = ControlText, .native = label(1), .terminal = schemeColor(SchemeSlot::PaneInk) },
        SettingsColorRow {
            .role = PreviewFrame, .native = separator(), .terminal = schemeOnBar(SchemeSlot::Secondary) },
    };
    // Every Settings role has a row here.
    CHECK(rows.size() == SettingsColorRoleEnd - SettingsColorRoleBegin);

    for (auto const index: allStyles())
    {
        auto const style = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        for (auto const& row: rows)
        {
            CAPTURE(static_cast<int>(row.role));
            auto const& expected = style == ChromeStyle::Terminal ? row.terminal : row.native;
            CHECK(resolveChromeTokens(style, {}).colors[static_cast<size_t>(row.role)] == expected);
        }
    }
}

namespace
{
/// The native rows' Settings text inks against what they sit on, in both appearances: the kit's label over
/// the kit's surfaces and over the OS window colour. The OS window colour and its text are the platform's
/// own, so a stand-in pair of typical values (a light and a dark window, black and white ink) stands for
/// them; the kit's roles are the ones this row table decides.
struct NativeGround
{
    RGBColor color;
    bool dark = false;
};

/// @return One colour of @p source over @p ground, composited, where it is one a native row reads on a
/// ground; the OS window text is the stand-in ink.
[[nodiscard]] RGBColor nativeInkOver(contour::config::ChromeColorSource const& source, NativeGround ground)
{
    using contour::config::ColorSourceKind;
    auto const ink = ground.dark ? 0xFFFFFF_rgb : 0x000000_rgb;
    auto const wash = [&](contour::config::AlphaPair pair, int percent) {
        auto const alpha = (ground.dark ? pair.dark : pair.light) * percent / 100;
        return mixColor(ground.color, ink, static_cast<float>(alpha) / 100.0f);
    };
    switch (source.kind)
    {
        case ColorSourceKind::Label:
            return wash(contour::config::LabelAlphas.at(static_cast<size_t>(source.level - 1)),
                        source.alphaPercent);
        case ColorSourceKind::Fill:
            return wash(contour::config::FillAlphas.at(static_cast<size_t>(source.level - 1)),
                        source.alphaPercent);
        case ColorSourceKind::PaletteWindowText: return ink;
        case ColorSourceKind::Fixed: return RGBColor(source.rgb);
        default: break;
    }
    FAIL("a source kind the native Settings contrast test does not resolve: "
         << static_cast<int>(source.kind));
    return ground.color;
}
} // namespace

TEST_CASE("chromeTokens.settingsTextReadsOnTheNativeRows", "[chrometokens]")
{
    using namespace contour::config;
    using enum ChromeColorRole;
    // Grounds a Settings text sits on: the kit's surfaces (sidebar, group), and a typical OS window.
    auto const grounds = std::array {
        NativeGround { .color = RGBColor(SurfaceColors[0].light), .dark = false },
        NativeGround { .color = RGBColor(SurfaceColors[1].light), .dark = false },
        NativeGround { .color = 0xECECEC_rgb, .dark = false },
        NativeGround { .color = RGBColor(SurfaceColors[0].dark), .dark = true },
        NativeGround { .color = RGBColor(SurfaceColors[1].dark), .dark = true },
        NativeGround { .color = 0x1E1E1E_rgb, .dark = true },
    };
    auto const inks = std::array { SettingsLabel, SettingsHelp, SettingsModified, SettingsIconGlyph };
    auto const preferences = std::array {
        AccessibilityPreferences {},
        AccessibilityPreferences { .motion = MotionPreference::Full,
                                   .transparency = TransparencyPreference::Allowed,
                                   .contrast = ContrastPreference::Increased },
    };
    for (auto const style: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        for (auto const& preference: preferences)
        {
            auto const row = resolveChromeTokens(style, preference);
            for (auto const& ground: grounds)
            {
                CAPTURE(static_cast<int>(style), ground.dark);
                for (auto const role: inks)
                {
                    CAPTURE(static_cast<int>(role));
                    auto const ink = nativeInkOver(row.colors[static_cast<size_t>(role)], ground);
                    CHECK(contrastRatio(ink, ground.color) >= MinimumTextContrast);
                }
                // A control's text on its own wash over the group.
                auto const& controlFill = row.colors[static_cast<size_t>(ControlFill)];
                auto const control = nativeInkOver(controlFill, ground);
                // The text is a label over the wash, which is the ground it sits on.
                auto const onWash = NativeGround { .color = control, .dark = ground.dark };
                CHECK(contrastRatio(nativeInkOver(row.colors[static_cast<size_t>(ControlText)], onWash),
                                    control)
                      >= MinimumTextContrast);
                // The unlit switch and its knob are marks: each stands off what it sits on at 3:1.
                auto const off = nativeInkOver(row.colors[static_cast<size_t>(SwitchOffFill)], ground);
                auto const knob = nativeInkOver(row.colors[static_cast<size_t>(SwitchKnob)], ground);
                CHECK(contrastRatio(off, ground.color) >= MinimumMarkContrast);
                CHECK(contrastRatio(knob, off) >= MinimumMarkContrast);
            }
            // The selected sidebar row and a lit switch are the OS's own pair and accent: sources the
            // palette decides, so what the row owns is that they are the ones named above.
            CHECK(row.colors[static_cast<size_t>(SettingsSelectionFill)].kind
                  == ColorSourceKind::PaletteHighlight);
            CHECK(row.colors[static_cast<size_t>(SettingsSelectionText)].kind
                  == ColorSourceKind::PaletteHighlightedText);
        }
    }
}

TEST_CASE("chromeTokens.settingsTextReadsOnTheTerminalRow", "[chrometokens]")
{
    // The Terminal row's Settings inks against their grounds, on every bundled scheme (contour-dark and
    // contour-light among them), with and without the increased-contrast preference.
    using namespace contour::config;
    using enum ChromeColorRole;
    struct Pair
    {
        ChromeColorRole ink = SettingsLabel;
        ChromeColorRole ground = SettingsPageFill;
    };
    auto const pairs = std::array {
        Pair { SettingsLabel, SettingsPageFill },
        Pair { SettingsLabel, SettingsGroupFill },
        Pair { SettingsLabel, SettingsSidebarFill },
        Pair { SettingsHelp, SettingsPageFill },
        Pair { SettingsHelp, SettingsGroupFill },
        Pair { SettingsModified, SettingsPageFill },
        Pair { SettingsModified, SettingsGroupFill },
        Pair { SettingsSelectionText, SettingsSelectionFill },
        Pair { SettingsIconGlyph, SettingsSidebarFill },
        Pair { SwitchOnFill, SettingsGroupFill },
        Pair { SwitchOffFill, SettingsGroupFill },
        Pair { ControlText, ControlFill },
    };
    auto const preferences = std::array {
        AccessibilityPreferences {},
        AccessibilityPreferences { .motion = MotionPreference::Full,
                                   .transparency = TransparencyPreference::Allowed,
                                   .contrast = ContrastPreference::Increased },
    };
    for (auto const& preference: preferences)
    {
        auto const row = resolveChromeTokens(ChromeStyle::Terminal, preference);
        for (auto const name: defaultColorPaletteNames())
        {
            INFO("scheme: " << name);
            auto palette = ColorPalette {};
            REQUIRE(defaultColorPalettes(name, palette));
            auto const colors = chromeSchemeColorsOf(palette);
            for (auto const& pair: pairs)
            {
                CAPTURE(static_cast<int>(pair.ink), static_cast<int>(pair.ground));
                auto const& ink = row.colors[static_cast<size_t>(pair.ink)];
                auto const& ground = row.colors[static_cast<size_t>(pair.ground)];
                REQUIRE(ground.kind == ColorSourceKind::Scheme);
                REQUIRE((ink.kind == ColorSourceKind::Scheme || ink.kind == ColorSourceKind::SchemeOnBar));
                auto const inkColor = ink.kind == ColorSourceKind::SchemeOnBar
                                          ? legibleOnBar(colors, ink.slot)
                                          : slotIn(colors, ink.slot);
                CHECK(contrastRatio(inkColor, slotIn(colors, ground.slot)) >= MinimumTextContrast);
            }
        }
    }
}

// }}}

TEST_CASE("chromeTokens.tooltipTokensPerRow", "[chrometokens]")
{
    using namespace contour::config;
    struct Row
    {
        ChromeStyle style;
        int padX;
        int padTop;
        int padBottom;
        int gap;
        int radius;
    };
    for (auto const& expected: { Row { ChromeStyle::MacOS, 8, 3, 3, 14, 6 },
                                 Row { ChromeStyle::Fluent, 9, 5, 7, 14, 8 },
                                 Row { ChromeStyle::ContourLinux, 10, 5, 5, 16, 8 },
                                 Row { ChromeStyle::Terminal, 1, 0, 0, 2, 0 } })
    {
        CAPTURE(static_cast<int>(expected.style));
        auto const& row = ChromeTokenRows[static_cast<size_t>(expected.style)];
        CHECK(row.size.tooltipPadX == expected.padX);
        CHECK(row.size.tooltipPadTop == expected.padTop);
        CHECK(row.size.tooltipPadBottom == expected.padBottom);
        CHECK(row.size.tooltipGap == expected.gap);
        CHECK(row.size.tooltipMaxWidth == (expected.style == ChromeStyle::Terminal ? 48 : 360));
        CHECK(row.radii.tooltip == expected.radius);
        // Every row waits the same before the first tooltip and keeps the next warm for the same span.
        CHECK(row.motion.tooltipDelayMs == 600);
        CHECK(row.motion.tooltipWarmMs == 1500);
    }
    // Under Reduce Motion the delay is a hold, not an animation, and stays.
    auto reduced = ChromeTokenRows[static_cast<size_t>(ChromeStyle::MacOS)];
    reduced.motion.fastMs = 0;
    CHECK(reduced.motion.tooltipDelayMs == 600);
}

TEST_CASE("chromeTokens.tooltipColourRolesPerRow", "[chrometokens]")
{
    using namespace contour::config;
    auto const role = [](ChromeStyle style, ChromeColorRole colorRole) {
        return ChromeTokenRows[static_cast<size_t>(style)].colors[static_cast<size_t>(colorRole)];
    };

    SECTION(
        "macOS: the kit's near-white and elevated dark, label 85/88 %, shortcut 45/50 %, hairline 18/14 %")
    {
        auto const fill = role(ChromeStyle::MacOS, ChromeColorRole::TooltipFill);
        CHECK(fill.kind == ColorSourceKind::Tone);
        CHECK(fill.rgb == 0xF6F6F6);
        CHECK(fill.darkRgb == 0x2C2C2E);
        auto const text = role(ChromeStyle::MacOS, ChromeColorRole::TooltipText);
        CHECK((text.rgb == 0x000000 && text.alphaPercent == 85));
        CHECK((text.darkRgb == 0xFFFFFF && text.darkAlphaPercent == 88));
        auto const shortcut = role(ChromeStyle::MacOS, ChromeColorRole::TooltipTextSecondary);
        CHECK((shortcut.alphaPercent == 45 && shortcut.darkAlphaPercent == 50));
        auto const edge = role(ChromeStyle::MacOS, ChromeColorRole::TooltipEdge);
        CHECK((edge.alphaPercent == 18 && edge.darkAlphaPercent == 14));
    }

    SECTION("Windows 11: #F9F9F9 / #2C2C2C, label 90 % / white, shortcut 55 / 60 %, edge 6 / 8 %")
    {
        auto const fill = role(ChromeStyle::Fluent, ChromeColorRole::TooltipFill);
        CHECK((fill.rgb == 0xF9F9F9 && fill.darkRgb == 0x2C2C2C));
        auto const text = role(ChromeStyle::Fluent, ChromeColorRole::TooltipText);
        CHECK((text.alphaPercent == 90 && text.darkAlphaPercent == 100));
        auto const shortcut = role(ChromeStyle::Fluent, ChromeColorRole::TooltipTextSecondary);
        CHECK((shortcut.alphaPercent == 55 && shortcut.darkAlphaPercent == 60));
        auto const edge = role(ChromeStyle::Fluent, ChromeColorRole::TooltipEdge);
        CHECK((edge.alphaPercent == 6 && edge.darkAlphaPercent == 8));
    }

    SECTION("ContourLinux: white / #38383D, label 87 / 92 %, shortcut 55 %, edge 12 / 10 %")
    {
        auto const fill = role(ChromeStyle::ContourLinux, ChromeColorRole::TooltipFill);
        CHECK((fill.rgb == 0xFFFFFF && fill.darkRgb == 0x38383D));
        auto const text = role(ChromeStyle::ContourLinux, ChromeColorRole::TooltipText);
        CHECK((text.alphaPercent == 87 && text.darkAlphaPercent == 92));
        auto const shortcut = role(ChromeStyle::ContourLinux, ChromeColorRole::TooltipTextSecondary);
        CHECK((shortcut.alphaPercent == 55 && shortcut.darkAlphaPercent == 55));
        auto const edge = role(ChromeStyle::ContourLinux, ChromeColorRole::TooltipEdge);
        CHECK((edge.alphaPercent == 12 && edge.darkAlphaPercent == 10));
    }

    SECTION("Terminal: the bar's cells, in the scheme's colours")
    {
        CHECK(role(ChromeStyle::Terminal, ChromeColorRole::TooltipFill).kind == ColorSourceKind::Scheme);
        CHECK(role(ChromeStyle::Terminal, ChromeColorRole::TooltipText).slot == SchemeSlot::BarInk);
        CHECK(role(ChromeStyle::Terminal, ChromeColorRole::TooltipTextSecondary).kind
              == ColorSourceKind::SchemeOnBar);
    }
}
