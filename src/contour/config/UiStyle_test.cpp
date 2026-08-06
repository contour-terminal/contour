// SPDX-License-Identifier: Apache-2.0
#include <contour/config/UiStyle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using contour::config::ChromeFontSource;
using contour::config::configEnumValues;
using contour::config::LengthUnit;
using contour::config::UiStyle;
using contour::config::uiStyleTokens;

TEST_CASE("UiStyle: every style has a token row", "[uistyle]")
{
    // uiStyleTokens() indexes the token table by enumerator, so a style without a row would read
    // past the end. The static_assert in the header pins the sizes; this pins that each row is
    // actually filled in rather than default-constructed.
    for (auto const& info: configEnumValues<UiStyle>())
    {
        auto const& tokens = uiStyleTokens(info.value);
        CHECK(tokens.chromeHeightUnits > 0);
        CHECK(tokens.tabHeightUnits > 0);
        CHECK(tokens.controlUnits > 0);
        CHECK(tokens.badgeUnits > 0);
        // The strip's "+" and "▾" are permanent targets rather than a glyph tucked inside a tab, so
        // every style pads them beyond the square control box. A style that forgot to would ship two
        // of the strip's hardest-to-hit buttons.
        CHECK(tokens.stripButtonUnits > tokens.controlUnits);
        CHECK(tokens.borderWidthPixels > 0);
        // A zero scrim is a modal popup with no dimming at all, which is the state the style files
        // are in when they forget to declare one -- so a row must not be able to say it in data.
        CHECK(tokens.modalScrimPercent > 0);
        CHECK(tokens.modalScrimPercent > tokens.modelessScrimPercent);
        CHECK(tokens.minTabUnits > 0);
        CHECK(tokens.maxTabUnits >= tokens.minTabUnits);
        CHECK(!tokens.closeGlyph.empty());
        CHECK(!tokens.zoomGlyph.empty());
        CHECK(!tokens.newTabGlyph.empty());
        CHECK(!tokens.menuGlyph.empty());
        // A sub-menu row that draws no marker is indistinguishable from one that runs a command.
        CHECK(!tokens.submenuGlyph.empty());
    }
}

TEST_CASE("UiStyle: a native tab is as wide as its historical literal made it", "[uistyle]")
{
    // TabItem.qml's naturalWidth used to be `label.implicitWidth + 56`. It is now the sum of the
    // tokens below, which is only a refactoring for as long as they still come to 56 -- so this is
    // the assertion that catches a token edit silently narrowing (or widening) every tab in the
    // strip. Neither of the QML tests can: one only checks the width lands inside the 120..240 band,
    // the other pins the token values without adding them up.
    //
    // Every term naturalWidth sums appears here, including the ones Native zeroes: a token that is 0
    // today is exactly the one a later edit can give a value to without anything else noticing.
    auto const& native = uiStyleTokens(UiStyle::Native);

    CHECK(native.labelPaddingUnits + native.labelGapUnits + native.controlUnits + native.trailingPaddingUnits
              + native.tabSlackUnits
          == 56);
    // Natively the strip has no separator, which is the remaining term of that sum.
    CHECK(native.tabSeparator.empty());
}

TEST_CASE("UiStyle: Native reproduces the chrome's historical metrics", "[uistyle]")
{
    // These are the literals TitleBar.qml, TabItem.qml and TabStrip.qml carried before the chrome was
    // made style-driven. They are the regression gate for "an existing configuration keeps the
    // appearance it had": the QML now reads them from here, so a change to this row is a change to
    // every user's window, and MainWindowQml_test's chromeHeight == 34 assertion depends on it.
    auto const& native = uiStyleTokens(UiStyle::Native);

    CHECK(native.unit == LengthUnit::Pixel);
    CHECK(native.chromeHeightUnits == 34);
    CHECK(native.tabHeightUnits == 32);
    CHECK(native.labelPaddingUnits == 10);
    CHECK(native.labelGapUnits == 4);
    CHECK(native.minTabUnits == 120);
    CHECK(native.maxTabUnits == 240);
    CHECK(native.controlUnits == 22);
    CHECK(native.badgeUnits == 16);
    // The part of the historical `+ 56` its named parts do not account for. @see the tab-width test.
    CHECK(native.tabSlackUnits == 20);
    // Nothing is drawn at a native tab's trailing edge, so the close button needs no inset from it --
    // the next tab's own labelPaddingUnits is the gap the eye sees.
    CHECK(native.trailingPaddingUnits == 0);
    // The one metric that is NOT the historical literal: the strip buttons used to be controlUnits
    // wide too, which made them needlessly hard to hit. @see the note on the token row.
    CHECK(native.stripButtonUnits == 32);
    CHECK(native.cornerRadiusPixels == 3);
    CHECK(native.dropCaretPixels == 4);
    CHECK(native.newTabPointSize == 12);
    CHECK(native.menuPointSize == 10);
    CHECK(native.windowControlUnits == 44);
    CHECK(native.quickControlsStyle == "Fusion");
    CHECK(native.fontSource == ChromeFontSource::PlatformUi);

    // Native draws no separator between tabs; they sit flush, as TabStrip's spacing: 0 always had it.
    CHECK(native.tabSeparator.empty());
}

TEST_CASE("UiStyle: Terminal counts its chrome in whole cells", "[uistyle]")
{
    auto const& terminal = uiStyleTokens(UiStyle::Terminal);

    // The unit is what makes the same numbers describe both styles -- Cell is what snaps the chrome
    // to the grid, and is what the QML reads to decide its quantum.
    CHECK(terminal.unit == LengthUnit::Cell);

    // One row tall, and every control exactly one cell: that is the whole point of the style, so it
    // is worth pinning rather than leaving to the table being read correctly.
    CHECK(terminal.chromeHeightUnits == 1);
    CHECK(terminal.tabHeightUnits == 1);
    CHECK(terminal.controlUnits == 1);
    CHECK(terminal.badgeUnits == 1);

    // No slack: a cell of padding at either end is already the breathing room slack buys natively, and
    // a spare cell per tab is a cell of title the user does not get.
    CHECK(terminal.tabSlackUnits == 0);

    // Those two ends, which is what makes the line above true. The trailing one is not decoration:
    // this style ends every tab with a rule, and without an inset the close glyph abutted it.
    CHECK(terminal.labelPaddingUnits == 1);
    CHECK(terminal.trailingPaddingUnits == 1);
    CHECK(!terminal.tabSeparator.empty());

    // Except the strip buttons, which are the glyph plus a cell of padding to either side -- still a
    // whole number of cells, so the strip stays on the grid.
    CHECK(terminal.stripButtonUnits == 3);

    // Square corners and a hairline border: box drawing, not GUI chrome. The drop caret narrows to a
    // hairline with them -- natively it is a deliberately fat 4px bar.
    CHECK(terminal.cornerRadiusPixels == 0);
    CHECK(terminal.borderWidthPixels == 1);
    CHECK(terminal.dropCaretPixels == 1);

    // A separator is what makes adjacent tabs readable without a fill difference.
    CHECK(!terminal.tabSeparator.empty());

    // 0 means "use the chrome font's own size", which is what keeps the buttons on the grid; a
    // hardcoded point size would put them off it.
    CHECK(terminal.newTabPointSize == 0);
    CHECK(terminal.menuPointSize == 0);

    // The style names the Quick Controls style that paints its controls, and where its font comes
    // from, so neither is a branch on the enumerator somewhere else in the tree.
    CHECK(terminal.quickControlsStyle == "ContourTui");
    CHECK(terminal.fontSource == ChromeFontSource::TerminalProfile);
}
