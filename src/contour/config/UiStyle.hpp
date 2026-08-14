// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.hpp>

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

    /// Blur radius of the drop shadow under a popup or menu. 0 draws none.
    ///
    /// Drawn rather than asked of the compositor, unlike the window's own shadow: every menu in the
    /// application is an in-scene popup (@c popupType: @c Popup.Item, forced because a native
    /// platform menu cannot host the sub-menus built at runtime), and an in-scene popup has no OS
    /// surface for any compositor to cast a shadow around.
    int shadowBlurPixels;

    /// How far the popup's shadow is displaced downward, so it reads as lying over the window.
    int shadowOffsetUnits;

    /// Alpha of the shadow color, as a percentage. Same reasoning as the scrims above: how heavy a
    /// popup's shadow reads is one number rather than a literal in every popup.
    int shadowOpacityPercent;

    /// Clear space a popup keeps between itself and the window edge.
    ///
    /// An in-scene popup is clipped by the window it lives in, so without a gutter the shadow of a
    /// popup opened near an edge is simply not drawn. Counted in @c unit rather than pixels,
    /// because it has to clear @c shadowOffsetUnits, which is.
    int shadowMarginUnits;

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

    /// Diameter of one traffic-light dot, for a @c WindowControlStyle that draws its window controls
    /// as dots rather than as buttons. @see WindowControlStyle.hpp.
    ///
    /// The extents of the window controls live in THIS table rather than in the window-control style's
    /// own row for the same reason every other chrome extent does: how long a thing is, is what the
    /// two styles disagree about, and stating it here is what lets a cell-counting chrome keep every
    /// window control on the grid. The window-control row states only what a style IS -- which side,
    /// which order, which colors -- so the two tables stay orthogonal instead of multiplying out.
    int trafficLightDotUnits;

    /// Gap between adjacent traffic-light dots.
    ///
    /// Part of the control's own width rather than empty space between controls, so that it is
    /// clickable: a dot is the smallest target in the whole chrome, and one exactly its own size is
    /// a hard thing to hit. The dot sits at the control's leading edge with the gap trailing it,
    /// which is also what keeps it on the grid in a cell-counting chrome -- centring a one-cell dot
    /// inside a wider box would land it half a cell off.
    int trafficLightGapUnits;

    /// Inset from the bar's outer edge to the first (or last) window control, so the controls sit in
    /// clear space rather than flush against the window corner. The horizontal counterpart of what
    /// @c labelPaddingUnits does for a tab.
    int windowControlInsetUnits;

    /// Clear space between the window-control group and whatever the bar puts beside it -- the tab
    /// strip where the group is at the leading edge, the drag region where it is at the trailing
    /// one. The inward counterpart of @c windowControlInsetUnits.
    ///
    /// Inert, unlike @c trafficLightGapUnits: what separates two lights is part of a control's own
    /// clickable width because a dot is the smallest target in the chrome, but the space beside the
    /// GROUP has no control to belong to, and a clickable strip of maximize in what reads as bare
    /// title bar is a bug rather than a generous hit target.
    int windowControlGutterUnits;

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

    /// The colors a tab's OSC 9;4 progress bar is painted in, as 0xRRGGBB, indexed by
    /// vtbackend::ProgressState. Index 0 (Inactive) paints nothing and is never read.
    ///
    /// A row here rather than a literal in the provider, so a style that wants its own palette says
    /// so in the table like every other style value. They are stated rather than derived from the OS
    /// palette because they say what an operation is DOING -- a theme-picked hue could render a
    /// failure green. Both rows agree today; the point is that they need not.
    std::array<uint32_t, 5> progressColors;

    /// How a traffic-light dot is drawn: empty paints a vector circle, and any other value is the
    /// glyph to paint in the dot's color instead.
    ///
    /// This is what lets the macOS-style window controls appear in a cell-quantized chrome without a
    /// special case anywhere: that style draws its dots the way it draws every other affordance --
    /// as one glyph in one cell -- while a pixel-counting style keeps the round shape the platform
    /// itself draws. The QML branches on this string, so it never learns which style is active.
    std::string_view trafficLightGlyph;

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
            // A soft, clearly displaced shadow -- what a desktop menu casts everywhere else. The
            // margin is generous enough that the blur is not cut off against the window edge.
            .shadowBlurPixels = 24,
            .shadowOffsetUnits = 4,
            .shadowOpacityPercent = 35,
            .shadowMarginUnits = 12,
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
            // The dimensions macOS itself uses for its traffic lights: a 12px dot on a 20px pitch,
            // inset from the window's leading edge. They are stated here rather than in the macOS
            // window-control row because they are lengths, and lengths are this table's business.
            .trafficLightDotUnits = 12,
            .trafficLightGapUnits = 8,
            .windowControlInsetUnits = 10,
            // Roughly what macOS leaves between its traffic lights and the first toolbar item. It is
            // larger than the inset on purpose: the outer end of the group faces the window's edge,
            // which is empty, while this end faces the tab strip -- a run of labelled, clickable
            // boxes that the lights would otherwise read as the first of.
            .windowControlGutterUnits = 16,
            .closePointSize = 8,
            .newTabPointSize = 12,
            .menuPointSize = 10,
            .windowControlPointSize = 10,
            .badgePixelSize = 10,
            .progressColors = {
                0x000000,  // Inactive: never painted
                0x3D9A50,  // Normal: running
                0xD13438,  // Error: failed
                0x3D9A50,  // Indeterminate: busy, drawn as running but pulsing
                0xE8A317,  // Paused: paused, or warning
            },
            // Empty: at 12 logical pixels a dot is smaller than any text this chrome sets, so it is
            // drawn as the shape it is rather than as a glyph scaled down to fit.
            .trafficLightGlyph = "",
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
            // No blur, one cell of offset: the hard-edged drop shadow a text-mode UI casts, which is
            // what this style is quoting. A soft 24px halo would be the one thing in the chrome not
            // aligned to the character grid.
            .shadowBlurPixels = 0,
            .shadowOffsetUnits = 1,
            .shadowOpacityPercent = 60,
            .shadowMarginUnits = 1,
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
            // One cell per dot, and one between them: "● ● ●" reads as the six-cell run a status
            // line would draw, and every dot stays on the grid. The gap is not decoration -- a cell
            // is around ten pixels, so three abutting one-cell dots are three targets too small and
            // too close together to hit, which is what a bare "●●●" turned out to be.
            .trafficLightDotUnits = 1,
            .trafficLightGapUnits = 1,
            .windowControlInsetUnits = 1,
            // One cell, like every other gap this style states: the point of the gutter is that the
            // lights are not the first tab, and one blank cell says that on a character grid as
            // clearly as sixteen pixels do on a pixel one.
            .windowControlGutterUnits = 1,
            .closePointSize = 0,
            .newTabPointSize = 0,
            .menuPointSize = 0,
            .windowControlPointSize = 0,
            .badgePixelSize = 0,
            .progressColors = {
                0x000000,  // Inactive: never painted
                0x3D9A50,  // Normal: running
                0xD13438,  // Error: failed
                0x3D9A50,  // Indeterminate: busy, drawn as running but pulsing
                0xE8A317,  // Paused: paused, or warning
            },
            // A dot is a character here, like everything else this style paints: one cell of "●" in
            // the traffic light's color, set in the same monospace font as the tabs beside it.
            .trafficLightGlyph = "●",
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
