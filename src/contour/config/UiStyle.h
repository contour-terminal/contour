// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace contour::config
{

/// How the application's own chrome -- tab strip, title bar, menus and popups -- is painted.
///
/// This selects a *form*, not a color: both styles take their colors from the OS palette and follow
/// the dark/light @c theme setting. What differs is shape and metrics -- @c Terminal quantizes the
/// chrome to whole character cells of a monospace font and squares off its corners for box drawing,
/// so the window reads as one continuous TUI rather than as a GUI wrapped around a terminal.
///
/// @note The Qt Quick Controls half of the style is chosen once, before the first control is created
///       (see ContourGuiApp), so a change takes effect on the next start rather than live.
enum class UiStyle : uint8_t
{
    Native = 0, //!< Platform-native GUI chrome (default, historical behavior).
    Terminal,   //!< Cell-quantized monospace chrome that reads like a TUI.
};

/// What one unit of chrome length means, for the extents @c UiStyleTokens counts in units.
///
/// This is what lets one set of numbers describe both styles: @c Pixel leaves an extent free to take
/// any value, while @c Cell makes it a count of character cells and so snaps the chrome to the grid.
enum class LengthUnit : uint8_t
{
    Pixel = 0, //!< One logical pixel; extents are free to take any value.
    Cell,      //!< One character cell of the chrome font; extents snap to whole cells.
};

/// Where a style's chrome font comes from.
///
/// A token rather than a test on the style, so the `ui_font_family` / `ui_font_size` keys apply to
/// any style whose row asks for the terminal's font, present or future.
enum class ChromeFontSource : uint8_t
{
    PlatformUi = 0,  //!< The platform UI font. What a native-looking chrome has always used.
    TerminalProfile, //!< The default profile's regular font, so the chrome matches the grid.
};

/// The shape, metrics and glyph vocabulary one @c UiStyle paints its chrome with.
///
/// Every corner radius, inset and decorative glyph the chrome QML used to carry as a literal lives
/// here instead, so a style is described by a row of data that the QML interprets.
///
/// @note "Adding a style is adding a row" holds for the hand-drawn chrome in @c src/contour/qml --
///       those files never learn which style is active. It does NOT cover the Qt Quick Controls half:
///       a new style also needs its own @c src/contour/styles/<Name>/ module of control files, which
///       is inherent to how Qt resolves a Quick Controls style by name.
///
/// Extents named @c ...Units are counted in @c unit; those named @c ...Pixels are always logical
/// pixels, because a hairline that scaled with the font would stop being a hairline.
struct UiStyleTokens
{
    LengthUnit unit; ///< The unit every @c ...Units member below is counted in.

    /// The Qt Quick Controls style that paints this chrome's controls, menus and popups.
    std::string_view quickControlsStyle;

    /// Where the chrome font comes from. @see resolveChromeFont.
    ChromeFontSource fontSource;

    int cornerRadiusPixels; ///< Corner radius of chrome rectangles. 0 squares them off.
    int borderWidthPixels;  ///< Border and outline thickness.
    int dropCaretPixels;    ///< Width of the tab strip's drag-and-drop insertion caret.

    /// Alpha of the hover wash laid over the highlight color, as a percentage. One number, so how
    /// strongly "hovered" reads is decided here rather than in every control that shows it.
    int hoverWashPercent;

    /// Alpha of the scrim a modal popup dims its window with, as a percentage. Same reasoning as the
    /// hover wash: how firmly "you cannot use that right now" reads is one number, not a literal
    /// repeated in every popup surface the style defines.
    int modalScrimPercent;

    /// Alpha of the scrim a merely dimmed (non-modal) popup uses. Lighter: it says "this is on top of
    /// that", not "that is unreachable".
    int modelessScrimPercent;

    int chromeHeightUnits; ///< Height of the whole title bar / tab strip.
    int tabHeightUnits;    ///< A tab's own natural height.
    int labelPaddingUnits; ///< Inset from a tab's leading edge to its label.
    int labelGapUnits;     ///< Gap between the label and whatever follows it.

    /// Inset from a tab's trailing edge to its close button -- the trailing counterpart of
    /// @c labelPaddingUnits, so the affordance sits in clear space at both ends rather than flush
    /// against whatever the tab ends with.
    ///
    /// It is a token because how much it matters is exactly what differs between styles: a style that
    /// draws a @c tabSeparator ends every tab with a visible rule the close glyph would otherwise
    /// touch, whereas a style whose tabs abut invisibly borrows the gap from the next tab's own
    /// @c labelPaddingUnits and needs none of its own.
    int trailingPaddingUnits;

    int minTabUnits; ///< Narrowest a tab may become.
    int maxTabUnits; ///< Widest a tab may grow.

    /// Width a tab carries beyond the sum of its laid-out parts, so a label at its natural size keeps
    /// breathing room instead of ending flush against the close button. Native's value is what the
    /// historical `+ 56` literal held over the parts it was made of; a cell-counting style wants none,
    /// because there a cell of padding is already a visible gap.
    int tabSlackUnits;

    int controlUnits;       ///< Edge length of a square chrome button, i.e. a tab's close affordance.
    int stripButtonUnits;   ///< Width of a tab-strip button ("+" and the profile dropdown).
    int windowControlUnits; ///< Width of a window button (minimize/maximize/close), which is wider.
    int badgeUnits;         ///< Edge length of the zoom badge.

    /// Point size for the per-tab close button, or 0 to use the chrome font's own size.
    int closePointSize;
    /// Point size for the new-tab button, or 0 to use the chrome font's own size.
    int newTabPointSize;
    /// Point size for the profile-dropdown button, or 0 to use the chrome font's own size.
    int menuPointSize;
    /// Point size for the window buttons, or 0 to use the chrome font's own size.
    int windowControlPointSize;
    /// Pixel size for the zoom badge glyph, or 0 to use the chrome font's own size. Pixels rather
    /// than points because the badge is sized against the cell box it must fit inside, not against
    /// the surrounding text.
    int badgePixelSize;

    std::string_view tabSeparator; ///< Drawn between adjacent tabs; empty when the style has none.
    std::string_view closeGlyph;   ///< The per-tab close affordance.
    std::string_view zoomGlyph;    ///< Marks a tab whose active pane is zoomed (see vtmux::Tab).
    std::string_view newTabGlyph;  ///< The "new tab" button.
    std::string_view menuGlyph;    ///< The "new tab with profile" dropdown button.
    std::string_view submenuGlyph; ///< Marks a menu row that opens a sub-menu.
};

namespace detail
{
    // inline, so every translation unit that includes this header shares one table rather than
    // getting a private copy the returned span would then point into.
    //
    // Rows are indexed by UiStyle, so their order is the enumerator order -- uiStyleTokens() relies
    // on that, and the static_assert below pins it against the token table.
    //
    // NB: zoomGlyph, newTabGlyph, menuGlyph and submenuGlyph happen to be identical in both rows. They live
    // in the table anyway because they are the same KIND of thing as the two that do vary, and a
    // style wanting an ASCII "v" rather than "▾" should not have to edit QML to get it.
    inline constexpr auto UiStyleTokenTable = std::array {
        // Native's numbers are the literals the chrome QML carried before it was made style-driven,
        // so selecting Native reproduces the previous appearance. That is what the geometry
        // assertions in MainWindowQml_test guard.
        //
        // One deliberate exception: stripButtonUnits. The "+" and "▾" used to be as narrow as a
        // tab's close glyph (controlUnits), which made two of the strip's three permanently visible
        // targets the hardest in it to hit. They are padded out here rather than in TabStrip.qml so
        // that every style states the width it wants.
        UiStyleTokens {
            .unit = LengthUnit::Pixel,
            .quickControlsStyle = "Fusion",
            .fontSource = ChromeFontSource::PlatformUi,
            .cornerRadiusPixels = 3,
            .borderWidthPixels = 1,
            .dropCaretPixels = 4,
            .hoverWashPercent = 25,
            // The values Qt's own Basic and Fusion styles dim with, so a scrim looks like a scrim
            // wherever the user has met one before.
            .modalScrimPercent = 50,
            .modelessScrimPercent = 12,
            .chromeHeightUnits = 34,
            .tabHeightUnits = 32,
            .labelPaddingUnits = 10,
            .labelGapUnits = 4,
            // None, which is what a native tab always had: its tabs abut with no rule between them, so
            // the close button is followed by the next tab's own 10px leading inset. Giving it one here
            // would widen every tab in every existing window for no visible gain.
            .trailingPaddingUnits = 0,
            .minTabUnits = 120,
            .maxTabUnits = 240,
            // The remainder of the historical `+ 56`: that literal covered the label's leading inset,
            // the gap after it and the close button (10 + 4 + 22) with 20 pixels left over. Keeping
            // the leftover is what keeps a native tab exactly as wide as it always was.
            .tabSlackUnits = 20,
            .controlUnits = 22,
            .stripButtonUnits = 32,
            .windowControlUnits = 44,
            .badgeUnits = 16,
            .closePointSize = 8,
            .newTabPointSize = 12,
            .menuPointSize = 10,
            .windowControlPointSize = 10,
            .badgePixelSize = 10,
            .tabSeparator = "",
            .closeGlyph = "✕",
            .zoomGlyph = "Z",
            .newTabGlyph = "+",
            .menuGlyph = "▾",
            .submenuGlyph = "▸",
        },
        // One row tall, every extent a whole cell, square corners and a box-drawing separator: the
        // vocabulary a status line would use. The close glyph is the lighter multiplication sign
        // rather than Native's heavy ✕, because at one cell wide beside a label set in the same
        // monospace font the heavy form reads as another letter of the label.
        UiStyleTokens {
            .unit = LengthUnit::Cell,
            .quickControlsStyle = "ContourTui",
            .fontSource = ChromeFontSource::TerminalProfile,
            .cornerRadiusPixels = 0,
            .borderWidthPixels = 1,
            .dropCaretPixels = 1,
            .hoverWashPercent = 25,
            // The values Qt's own Basic and Fusion styles dim with, so a scrim looks like a scrim
            // wherever the user has met one before.
            .modalScrimPercent = 50,
            .modelessScrimPercent = 12,
            .chromeHeightUnits = 1,
            .tabHeightUnits = 1,
            .labelPaddingUnits = 1,
            .labelGapUnits = 1,
            // A cell, matching the leading inset: this style ends every tab with a "│" rule, and with
            // no inset the close glyph sat directly against it -- "×│" reads as one box-drawing
            // ligature rather than as a button beside a border.
            .trailingPaddingUnits = 1,
            .minTabUnits = 10,
            .maxTabUnits = 24,
            // None: a cell of padding at either end (labelPaddingUnits and trailingPaddingUnits) is
            // already the breathing room slack buys natively, and a spare cell per tab is a cell of
            // title the user does not get.
            .tabSlackUnits = 0,
            .controlUnits = 1,
            .stripButtonUnits = 3,
            .windowControlUnits = 3,
            .badgeUnits = 1,
            .closePointSize = 0,
            .newTabPointSize = 0,
            .menuPointSize = 0,
            .windowControlPointSize = 0,
            .badgePixelSize = 0,
            .tabSeparator = "│",
            .closeGlyph = "×",
            .zoomGlyph = "Z",
            .newTabGlyph = "+",
            .menuGlyph = "▾",
            .submenuGlyph = "▸",
        },
    };

    inline constexpr auto UiStyleTable = std::array {
        ConfigEnumInfo<UiStyle> { UiStyle::Native, "native", "Native (GUI)" },
        ConfigEnumInfo<UiStyle> { UiStyle::Terminal, "terminal", "Terminal (TUI)" },
    };

    static_assert(UiStyleTokenTable.size() == UiStyleTable.size(),
                  "Every UiStyle needs exactly one token row; uiStyleTokens() indexes by enumerator.");
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<UiStyle> const> configEnumValues() noexcept
{
    return detail::UiStyleTable;
}

/// The shape, metrics and glyph vocabulary @p style paints with.
///
/// Indexes the table by enumerator, which the static_assert above keeps sound; no bounds check is
/// needed and none is done, so this stays usable from a noexcept context.
[[nodiscard]] constexpr UiStyleTokens const& uiStyleTokens(UiStyle style) noexcept
{
    return detail::UiStyleTokenTable[static_cast<size_t>(style)];
}

} // namespace contour::config
