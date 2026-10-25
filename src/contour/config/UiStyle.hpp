// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <vtbackend/vt/ProgressState.hpp>

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

/// One row of the chrome's token table: the look the chrome is painted with once the configured
/// @c UiStyle has been resolved against the host platform (see @c resolveChromeStyle).
///
/// @c UiStyle is what the user asks for and stays a two-entry config value; this is what the chrome
/// actually is, so a native look can differ per platform without a new config key.
/// Enumerator order is the row order of the token table.
enum class ChromeStyle : uint8_t
{
    MacOS = 0,    //!< Native chrome on macOS.
    Fluent,       //!< Native chrome on Windows.
    ContourLinux, //!< Native chrome on KDE Plasma and every other host.
    Terminal,     //!< Cell-quantized monospace chrome that reads like a TUI.
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

/// The shape, metrics and glyph vocabulary one @c ChromeStyle paints its chrome with.
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

    /// How a traffic-light dot is drawn: empty paints a vector circle, and any other value is the
    /// glyph to paint in the dot's color instead.
    ///
    /// This is what lets the macOS-style window controls appear in a cell-quantized chrome without a
    /// special case anywhere: that style draws its dots the way it draws every other affordance --
    /// as one glyph in one cell -- while a pixel-counting style keeps the round shape the platform
    /// itself draws. The QML branches on this string, so it never learns which style is active.
    std::string_view trafficLightGlyph;

    std::string_view tabSeparator; ///< Drawn between adjacent tabs; empty when the style has none.

    /// The cell a coloured tab leads with, in the tab's colour; empty where the colour tints the tab
    /// instead (the native rows' pill and fill).
    std::string_view tabColorGlyph;
    std::string_view chipOpenGlyph;  ///< Opens a toolbar or host chip; empty where the chip is a shape.
    std::string_view chipCloseGlyph; ///< Closes a toolbar or host chip; empty where the chip is a shape.
    std::string_view hostDotGlyph;   ///< The host chip's state dot; empty where it is a vector circle.

    std::string_view closeGlyph;   ///< The per-tab close affordance.
    std::string_view zoomGlyph;    ///< Marks a tab whose active pane is zoomed (see vtmux::Tab).
    std::string_view newTabGlyph;  ///< The "new tab" button.
    std::string_view menuGlyph;    ///< The "new tab with profile" dropdown button.
    std::string_view submenuGlyph; ///< Marks a menu row that opens a sub-menu.
};

/// The meanings the chrome colours with a status hue rather than with the OS palette.
///
/// Enumerator order is the row order of @c StatusColors. A status hue is a statement about meaning
/// ("this failed"), so a theme-picked colour could render a failure green; the table pins it.
enum class StatusColorRole : uint8_t
{
    Danger = 0, //!< A destructive action or a failure.
    Warning,    //!< Something the user should read before going on.
    Success,    //!< A completed or healthy state.
    Running,    //!< Work in progress. Not a fixed hue: it is the palette's accent.
};

/// Where a status hue comes from.
enum class StatusColorSource : uint8_t
{
    Fixed = 0,     //!< The row's @c light / @c dark pair.
    PaletteAccent, //!< The application palette's highlight colour; the pair is unused.
};

/// One status role's colour in each theme, as @c 0xRRGGBB.
struct StatusColor
{
    StatusColorSource source = StatusColorSource::Fixed; ///< Where the colour comes from.
    uint32_t light = 0;                                  ///< Used on a light chrome.
    uint32_t dark = 0;                                   ///< Used on a dark chrome.
};

/// Number of @c StatusColorRole enumerators, derived from the last one.
inline constexpr size_t StatusColorRoleCount = static_cast<size_t>(StatusColorRole::Running) + 1;

/// The status colours, indexed by @c StatusColorRole; a new role adds its row here.
inline constexpr std::array<StatusColor, StatusColorRoleCount> StatusColors = {
    StatusColor { StatusColorSource::Fixed, 0xD70015, 0xFF6961 }, // Danger
    StatusColor { StatusColorSource::Fixed, 0xC93400, 0xFF9F0A }, // Warning
    StatusColor { StatusColorSource::Fixed, 0x248A3D, 0x32D74B }, // Success
    StatusColor { StatusColorSource::PaletteAccent, 0, 0 },       // Running
};

/// The status role a tab's OSC 9;4 progress indicator is inked in, indexed by @c vtbackend::ProgressState.
///
/// A role rather than a hue, so each chrome row resolves it as it resolves every status ink: running
/// is the accent (never a fixed green), a failure is @c Danger, a pause is @c Warning. Index 0
/// (@c Inactive) paints nothing and is never read.
inline constexpr std::array<StatusColorRole, 5> ProgressStatusRoles {
    StatusColorRole::Running, // Inactive: never painted
    StatusColorRole::Running, // Normal
    StatusColorRole::Danger,  // Error
    StatusColorRole::Running, // Indeterminate: drawn as running, pulsing
    StatusColorRole::Warning, // Paused
};

static_assert(ProgressStatusRoles.size() == static_cast<size_t>(vtbackend::ProgressState::Paused) + 1,
              "ProgressStatusRoles needs one row per vtbackend::ProgressState");

/// Chrome tokens no style row varies, kept together so a restyle changes a number here and not a
/// literal in a QML file.
namespace ChromeTokens
{
    inline constexpr int PaneDimPercent = 60;       ///< Opacity of the black scrim over a dimmed pane.
    inline constexpr int DotGlyphPercent = 60;      ///< Opacity of the glyph drawn on a traffic-light dot.
    inline constexpr int CaptionFontPercent = 85;   ///< Caption text size relative to the chrome font.
    inline constexpr int HeadlineFontPercent = 150; ///< Overlay headline size relative to the chrome font.
} // namespace ChromeTokens

namespace detail
{
    // inline, so every translation unit that includes this header shares one table rather than
    // getting a private copy the returned span would then point into.
    //
    // Rows are indexed by ChromeStyle, so their order is the enumerator order -- uiStyleTokens()
    // relies on that, and the static_assert below pins it against the enumerator count.
    //
    // NB: zoomGlyph, newTabGlyph, menuGlyph and submenuGlyph happen to be identical in both rows.
    // They live in the table anyway because they are the same KIND of thing as the two that do
    // vary, and a style wanting an ASCII "v" rather than "▾" should not have to edit QML to get it.
    // NativeTokens' numbers are the literals the chrome QML carried before it was made
    // style-driven, so every native ChromeStyle row reproduces the previous appearance. That is
    // what the geometry assertions in MainWindowQml_test guard.
    //
    // One deliberate exception: stripButtonUnits. The "+" and "▾" used to be as narrow as a
    // tab's close glyph (controlUnits), which made two of the strip's three permanently visible
    // targets the hardest in it to hit. They are padded out here rather than in TabStrip.qml so
    // that every style states the width it wants.
    inline constexpr auto NativeTokens = UiStyleTokens {
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
        // Empty: at 12 logical pixels a dot is smaller than any text this chrome sets, so it is
        // drawn as the shape it is rather than as a glyph scaled down to fit.
        .trafficLightGlyph = "",
        .tabSeparator = "",
        .tabColorGlyph = "",
        .chipOpenGlyph = "",
        .chipCloseGlyph = "",
        .hostDotGlyph = "",
        .closeGlyph = "✕",
        .zoomGlyph = "Z",
        .newTabGlyph = "+",
        .menuGlyph = "▾",
        .submenuGlyph = "▸",
    };

    // One row tall, every extent a whole cell, square corners and a box-drawing separator: the
    // vocabulary a status line would use. The close glyph is the lighter multiplication sign
    // rather than Native's heavy ✕, because at one cell wide beside a label set in the same
    // monospace font the heavy form reads as another letter of the label.
    inline constexpr auto TerminalTokens = UiStyleTokens {
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
        // A dot is a character here, like everything else this style paints: one cell of "●" in
        // the traffic light's color, set in the same monospace font as the tabs beside it.
        .trafficLightGlyph = "●",
        .tabSeparator = "│",
        // The tab's colour as its leading cell, and a chip as the bracketed run a status line sets.
        .tabColorGlyph = "▌",
        .chipOpenGlyph = "[",
        .chipCloseGlyph = "]",
        .hostDotGlyph = "●",
        .closeGlyph = "×",
        .zoomGlyph = "Z",
        .newTabGlyph = "+",
        .menuGlyph = "▾",
        .submenuGlyph = "▸",
    };

    // The three native rows start as copies of one another (Phase 0 moves no pixel); a platform's
    // look diverges by changing its own row, never by branching in code.
    inline constexpr auto UiStyleTokenTable = std::array {
        NativeTokens,   // ChromeStyle::MacOS
        NativeTokens,   // ChromeStyle::Fluent
        NativeTokens,   // ChromeStyle::ContourLinux
        TerminalTokens, // ChromeStyle::Terminal
    };

    /// Number of @c ChromeStyle enumerators, derived from the last one; the token table must have
    /// exactly this many rows.
    inline constexpr size_t ChromeStyleCount = static_cast<size_t>(ChromeStyle::Terminal) + 1;

    inline constexpr auto UiStyleTable = std::array {
        ConfigEnumInfo<UiStyle> { UiStyle::Native, "native", "Native (GUI)" },
        ConfigEnumInfo<UiStyle> { UiStyle::Terminal, "terminal", "Terminal (TUI)" },
    };

    static_assert(UiStyleTokenTable.size() == ChromeStyleCount,
                  "Every ChromeStyle needs exactly one token row; uiStyleTokens() indexes by enumerator.");
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<UiStyle> const> configEnumValues() noexcept
{
    return detail::UiStyleTable;
}

/// The chrome row @p style resolves to on @p platform.
///
/// A native style picks the row of the host it runs on; the terminal style looks the same everywhere.
/// @param style    The configured style.
/// @param platform The host the application runs on.
/// @return The token row to paint the chrome with.
[[nodiscard]] constexpr ChromeStyle resolveChromeStyle(UiStyle style, HostPlatform platform) noexcept
{
    if (style == UiStyle::Terminal)
        return ChromeStyle::Terminal;
    switch (platform)
    {
        case HostPlatform::MacOS: return ChromeStyle::MacOS;
        case HostPlatform::Windows: return ChromeStyle::Fluent;
        case HostPlatform::KdePlasma:
        case HostPlatform::Other: break;
    }
    return ChromeStyle::ContourLinux;
}

/// The shape, metrics and glyph vocabulary the chrome row @p row paints with.
///
/// Indexes the table by enumerator, which the static_assert above keeps sound; no bounds check is
/// needed and none is done, so this stays usable from a noexcept context.
[[nodiscard]] constexpr UiStyleTokens const& uiStyleTokens(ChromeStyle row) noexcept
{
    return detail::UiStyleTokenTable[static_cast<size_t>(row)];
}

/// The name QML and the snapshot files know @p row by.
/// @param row The chrome row.
/// @return "macos", "fluent", "linux" or "terminal".
[[nodiscard]] constexpr std::string_view chromeStyleName(ChromeStyle row) noexcept
{
    switch (row)
    {
        case ChromeStyle::MacOS: return "macos";
        case ChromeStyle::Fluent: return "fluent";
        case ChromeStyle::ContourLinux: return "linux";
        case ChromeStyle::Terminal: break;
    }
    return "terminal";
}

} // namespace contour::config
