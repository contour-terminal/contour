// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/config/MaterialSupport.hpp>
#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/BlockPill.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/screen/GutterSettings.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace contour::config
{

/// What a translucent or opaque surface is made of.
enum class Material : uint8_t
{
    None = 0, //!< No material: the surface is a plain cell fill (the Terminal style).
    Glass,    //!< Vibrancy / blur-behind.
    Mica,     //!< Wallpaper-tinted backdrop.
    Solid,    //!< An opaque fill.
};

/// Number of @c Material enumerators, derived from the last one.
inline constexpr size_t MaterialCount = static_cast<size_t>(Material::Solid) + 1;

/// A material as drawn where nothing translucent may be: Glass and Mica become Solid, the rest stay.
/// @param material The material.
/// @return Its opaque counterpart.
[[nodiscard]] constexpr Material opaqueMaterialOf(Material material) noexcept
{
    return material == Material::Glass || material == Material::Mica ? Material::Solid : material;
}

/// Which kind of window asks for the operating system's material: it picks the material's variant.
enum class BackdropKind : uint8_t
{
    Tabbed = 0, //!< A tabbed terminal window: Mica Alt on Windows.
    Main,       //!< A single-content window (Settings): Mica on Windows.
};

/// What the operating system draws behind the title band.
enum class BandBackdrop : uint8_t
{
    Client = 0,     //!< Nothing: the band is the client area's own paint.
    NativeMaterial, //!< The platform's title-bar material (macOS vibrancy) shows through the band.
};

/// What the title band paints.
enum class BandFill : uint8_t
{
    Palette = 0, //!< Today's palette window colour.
    Clear,       //!< Nothing, so the backdrop shows as it is.
    Tint,        //!< The terminal background, tinted (see window::bandTintOf).
    Background,  //!< The terminal background itself (Minimal's bare band).
};

/// Number of @c BandFill enumerators, derived from the last one.
inline constexpr size_t BandFillCount = static_cast<size_t>(BandFill::Background) + 1;

/// What the native title bar is grown with.
enum class TitleBarGrowth : uint8_t
{
    None = 0,       //!< The bare title bar.
    CompactToolbar, //!< An empty compact toolbar, which makes the title bar as tall as the band (40 pt).
};

/// How a toast is painted.
enum class ToastStyle : uint8_t
{
    Floating = 0, //!< A raised pill over the content.
    Inverse,      //!< A row of inverted cells.
};

/// Where a dialog appears.
enum class DialogPlacement : uint8_t
{
    Centered = 0, //!< Centred over the window.
    Sheet,        //!< Attached to the window's title bar.
};

/// How a menu is presented.
enum class MenuPresentation : uint8_t
{
    InScene = 0, //!< Drawn by the chrome inside the window's scene.
    Native,      //!< Handed to the operating system's menu presenter, falling back to the scene.
};

/// How the tabs are drawn.
enum class TabShape : uint8_t
{
    Flat = 0, //!< Today's strip: the bar is the tab strip, edge to edge.
    Pill,     //!< A well of pills inside the window's title band.
    /// Tabs with rounded top corners, bottom-aligned in the bar, the active one filled with the content's
    /// colour so that it joins the content area (Windows 11).
    Joined,
};

/// Number of @c TabShape enumerators, derived from the last one.
inline constexpr size_t TabShapeCount = static_cast<size_t>(TabShape::Joined) + 1;

/// How the window's top row is arranged (spec §1.2's "header-bar arrangement").
enum class HeaderBarArrangement : uint8_t
{
    Band = 0, //!< 1c's title band, the window buttons where the OS or the control style puts them.
    Unified,  //!< The Linux header bar: new tab leading, search and menu trailing, then the window buttons
              //!< beyond a separator, on the side the desktop's button layout names.
};

/// Number of @c HeaderBarArrangement enumerators, derived from the last one.
inline constexpr size_t HeaderBarArrangementCount = static_cast<size_t>(HeaderBarArrangement::Unified) + 1;

/// What a tab's close button shows.
enum class TabCloseMark : uint8_t
{
    Glyph = 0,    //!< The row's close glyph (UiStyleTokens::closeGlyph), set as text.
    SymbolicIcon, //!< The freedesktop close icon (TabCloseIcon), recoloured to the label's ink.
};

/// A freedesktop icon: its theme name, and the bundled copy used where the theme lacks it.
struct SymbolicIconRef
{
    std::string_view name {};    ///< The freedesktop icon name, resolved through the theme.
    std::string_view bundled {}; ///< The bundled icon's URL.
};

/// The icon a @c TabCloseMark::SymbolicIcon close button shows: the header bar's close button's.
inline constexpr auto TabCloseIcon =
    SymbolicIconRef { .name = "window-close-symbolic",
                      .bundled = "qrc:/contour/icons/window-close-symbolic.svg" };

/// Where a tab's fill and label colours come from.
enum class TabColorSource : uint8_t
{
    Palette = 0, //!< The OS palette: highlight for the active tab, a user colour as a WT-style fill.
    Roles,       //!< The chrome colour roles (TabActiveFill, TabText, ...), so a scheme's legible pair holds.
};

/// How the active tab's label is set apart from the others, beyond its fill.
enum class ActiveTabWeight : uint8_t
{
    Regular = 0, //!< By its fill alone.
    Bold,        //!< In bold as well: the Terminal row's selection can fall back to the bar's own colour.
};

/// How the chrome animates.
enum class MotionStyle : uint8_t
{
    Full = 0,  //!< Slides, spinners and pulses run.
    CrossFade, //!< Reduce Motion: every transition is a cross-fade, and nothing spins or pulses.
};

/// How the new-tab profile menu is reached.
enum class ProfileMenuAffordance : uint8_t
{
    Caret = 0,      //!< A "▾" button beside the "+".
    SecondaryClick, //!< A secondary click or a long press on the "+" itself.
};

/// The chrome colours a row describes; enumerator order is the order of @c ChromeTokenRow::colors.
enum class ChromeColorRole : uint8_t
{
    BannerFill = 0, //!< Background of a banner.
    BannerText,     //!< Text of a banner.
    BannerGlyph,    //!< Leading glyph of a banner.
    ToastFill,      //!< Background of a toast.
    ToastText,      //!< Text of a toast.
    SwatchRing,     //!< Ring around the checked colour swatch of a menu.

    // The tab well and its pills.
    TabWellFill,        //!< The well the tabs sit in.
    TabActiveFill,      //!< The active tab.
    TabHoverFill,       //!< A hovered tab.
    TabCloseFill,       //!< A hovered tab's close button.
    ControlPressedFill, //!< A pressed band control.
    TabText,            //!< An inactive tab's label.
    TabTextActive,      //!< The active tab's label.
    /// A label one level down: the inactive window's labels, and a tab icon without a colour. Below
    /// 4.5:1 on a light well (about 3.3) by design: WCAG 1.4.3 exempts the text of an inactive
    /// component, and the icon is a non-text mark, so both are held to 3:1 instead.
    TabTextDimmed,
    TabTextTinted, //!< An inactive tab's label on its colour tint: one level up, so it keeps 4.5:1.

    // The band's chips.
    ChipFill,       //!< A toolbar or host chip.
    ChipText,       //!< A chip's text.
    ChipTextStrong, //!< A chip's emphasised text (the host name).

    // The caption buttons (WindowControls.qml).
    CaptionWash,         //!< A hovered or pressed caption button's fill, where its style washes with the kit.
    CaptionGlyphPressed, //!< A pressed caption button's glyph.
    /// The caption glyphs, and the Terminal row's control cells, of an inactive window. Held to 3:1, as
    /// @c TabTextDimmed is: WCAG 1.4.3 exempts an inactive component, and a glyph is a mark.
    CaptionGlyphInactive,

    // The pane cards (resolved against the card's own background, see UiStyleProvider::colorOn).
    PaneGapFill,           //!< The backdrop the cards and the gap between them sit on.
    PaneCardBorder,        //!< A card's one-device-pixel border.
    PaneHeaderFill,        //!< A pane header's fill.
    PaneHeaderText,        //!< The focused pane's header text.
    PaneHeaderTextDimmed,  //!< An unfocused pane's header text, and the working directory.
    PaneSplitLine,         //!< The line a split is drawn with where the gap does not draw it.
    PaneGrabber,           //!< The split handle's grabber, and the Terminal row's engaged split line.
    PaneFooterFill,        //!< The exited footer's fill.
    PaneFooterText,        //!< The exited footer's text.
    PaneHeaderActionHover, //!< A hovered or focused pane header button's fill.

    // The command palette.
    PaletteSurface,       //!< The palette's surface.
    PaletteText,          //!< A row's title and the query.
    PaletteSecondaryText, //!< Group headers, shortcuts, the footer and the placeholder.
    PaletteSelectionFill, //!< The selected row's fill.
    PaletteSelectionText, //!< The selected row's title and accessory.
    PaletteSeparator,     //!< The rules below the query and above the footer.

    // The find bar.
    FindToggleOnFill, //!< A lit option toggle's fill.
    FindToggleOnText, //!< A lit option toggle's glyph.
    FindToggleText,   //!< An unlit option toggle's glyph.
    FindCountText,    //!< The match count beside the field.
    FindToggleOnRing, //!< The ring that sets a lit option toggle off an unlit one; none where its fill does.
    FindErrorText,    //!< The count, and the field's edge, on a miss or a refused pattern.

    // The overlays drawn over a pane's grid.
    ToastTextSecondary,   //!< A toast's second text: the link pill's hint.
    ScrollbarThumb,       //!< The overlay scrollbar's thumb at rest (resolved against the pane, see colorOn).
    ScrollbarThumbActive, //!< The thumb while hovered or dragged.
    ScrollbarTrack,       //!< The overlay scrollbar's track.

    // Keyboard focus in the chrome.
    /// The ring around the item that has keyboard focus: a mark, so it reads at 3:1 on the window; none on
    /// the Terminal row, which fills the focused cells instead (@c FocusFill).
    FocusRing,
    /// The fill of the focused item on the Terminal row (a cell run cannot draw an outset); none on the
    /// native rows, which ring it.
    FocusFill,

    // The Settings window.
    SettingsSidebarFill,   //!< The sidebar's fill.
    SettingsPageFill,      //!< The page the groups sit on.
    SettingsGroupFill,     //!< A group's (or a card's) fill.
    SettingsSeparator,     //!< The rule between rows, and below the page header.
    SettingsLabel,         //!< A row's label.
    SettingsHelp,          //!< A row's help line, and a group's title.
    SettingsSelectionFill, //!< The selected sidebar row's fill.
    SettingsSelectionText, //!< The selected sidebar row's label and glyph.
    SettingsModified,      //!< The "Modified" mark beside an overridden value.
    SettingsIconTile,      //!< The tile behind a sidebar icon.
    SettingsIconGlyph,     //!< A sidebar icon.
    SwitchOnFill,          //!< A switch's track while on.
    SwitchOffFill,         //!< A switch's track while off.
    SwitchKnob,            //!< A switch's knob.
    ControlFill,           //!< A pop-up, button or field's fill.
    ControlText,           //!< A pop-up, button or field's text.
    PreviewFrame,          //!< The edge of the live preview.

    // The window controls.
    WindowButtonFill, //!< A round window button's fill at rest (the header bar's minimize, maximize, close).
    WindowButtonHoverFill,   //!< A round window button's fill while hovered.
    WindowButtonPressedFill, //!< A round window button's fill while pressed.
    WindowButtonGlyph,       //!< A round window button's icon, on each of its three fills.
    HeaderSeparator,         //!< The header bar's line between its items and the window buttons.

    // The tooltips (KitToolTip.qml).
    TooltipFill,          //!< A tooltip's fill.
    TooltipText,          //!< A tooltip's label, and the first line of a multi-line one.
    TooltipTextSecondary, //!< A tooltip's shortcut, and the lines after the first.
    TooltipEdge,          //!< A tooltip's hairline edge; none where the box is drawn in cells.
};

/// Where a chrome colour comes from.
enum class ColorSourceKind : uint8_t
{
    Fixed = 0,         //!< The source's @c rgb.
    PaletteWindow,     //!< The OS palette's window colour.
    PaletteWindowText, //!< The OS palette's window text colour.
    Status,            //!< A status hue, picked by the source's @c status.
    Scheme,            //!< A slot of the terminal's colour scheme, picked by the source's @c slot.
    // The kit's tables (FillAlphas, LabelAlphas, SurfaceColors), by the source's @c level: black or the
    // light surface on a light chrome, white or the dark surface on a dark one.
    Fill,             //!< The kit's @c fill.N wash.
    Label,            //!< The kit's @c label.N text.
    Surface,          //!< The kit's opaque @c surface.N: 1 elevated, 2 secondary.
    SchemeOnBar,      //!< The source's scheme @c slot where it reads at 4.5:1 on @c Bar, else @c BarInk.
    StatusMark,       //!< A status hue's mark ink (@c StatusInkUse::Mark), picked by the source's @c status.
    SchemeShade,      //!< The scheme @c slot moved by @c level HSL lightness points (see shadeLightness).
    Separator,        //!< Black on a light ground, white on a dark one, at @c contrast.separatorPercent.
    PaletteHighlight, //!< The OS palette's selected-row colour (@c Highlight).
    PaletteHighlightedText, //!< The OS palette's text on its selected row (@c HighlightedText).
    /// A status hue's mark ink, picked by the source's @c status, where it reaches 3:1 on the OS palette's
    /// window colour, else the window's own ink (UiStyleProvider::statusMarkOn).
    StatusMarkOnWindow,
    /// The source's @c rgb at @c alphaPercent on a light chrome, its @c darkRgb at @c darkAlphaPercent on a
    /// dark one: a colour the kit's tables do not hold, such as a tooltip's own fill.
    Tone,
    PaletteMid, //!< The OS palette's mid colour: what an in-scene menu's border is drawn in.
};

/// The terminal colour-scheme slots the Terminal style paints its chrome with.
enum class SchemeSlot : uint8_t
{
    Background = 0, //!< The scheme's default background.
    Foreground,     //!< The scheme's default foreground.
    Bar,            //!< ANSI colour 0, the bar fill.
    /// The bar's one ink: vtbackend::legibleInkOn(), the first of vtbackend::InkFallbacks that reads on @c
    /// Bar at 4.5:1. Every text on the bar is inked with it, so one bar never carries two answers to the same
    /// question.
    BarInk,
    Secondary, //!< ANSI colour 8.
    /// The chrome's selection: the scheme's (@c schemeSelectionOf) where an end of the scheme reads on it
    /// at 4.5:1, else the first legible pair of @c SelectionFallbacks.
    Selection,
    SelectionText, //!< The text on @c Selection, which reaches 4.5:1 wherever any candidate pair does.
    Accent,        //!< ANSI colour 4.
    Danger,        //!< ANSI colour 1.
    Success,       //!< ANSI colour 2.
    Warning,       //!< ANSI colour 3.
    /// A selection that also stands off @c Bar at 3:1, for a selected row drawn on the bar (the command
    /// palette): @c Selection where it does, else inverse video on the bar (@c BarInk as the fill).
    BarSelection,
    BarSelectionText, //!< The text on @c BarSelection, at 4.5:1.
    /// The toast's fill: inverse video (the foreground) where the background reads on it at 4.5:1, else the
    /// bar.
    Toast,
    ToastInk, //!< The text on @c Toast, which reads on it wherever either candidate does.
    /// Ink on the pane's own background (@c Background): vtbackend::legibleInkOn(), at least 4.5:1. The last
    /// resort of the mark chains below, which therefore always end in a legible mark.
    PaneInk,
    /// The overlay scrollbar's thumb on the pane: the first of @c ScrollbarThumbFallbacks that stands off
    /// @c Background at 3:1 (a mark, WCAG 1.4.11).
    ScrollbarThumb,
    /// The thumb while hovered or dragged: the first of @c ScrollbarThumbActiveFallbacks that stands off
    /// @c Background at 3:1.
    ScrollbarThumbActive,
    /// The link pill's hint on the toast: vtbackend::legibleInkOn() on @c Toast, at least 4.5:1.
    ToastHint,
};

/// Number of @c SchemeSlot enumerators, derived from the last one.
inline constexpr size_t SchemeSlotCount = static_cast<size_t>(SchemeSlot::ToastHint) + 1;

/// Where the Terminal row's resting scrollbar thumb takes its colour, first that marks against the pane wins:
/// ANSI 8, else the foreground, else the pane's ink, which always does.
inline constexpr std::array ScrollbarThumbFallbacks {
    SchemeSlot::Secondary,
    SchemeSlot::Foreground,
    SchemeSlot::PaneInk,
};

/// Where the Terminal row's engaged scrollbar thumb takes its colour, by the same rule: the accent first.
inline constexpr std::array ScrollbarThumbActiveFallbacks {
    SchemeSlot::Accent,
    SchemeSlot::Foreground,
    SchemeSlot::PaneInk,
};

/// A slot that is a mark against the pane, and the slots it is chosen from (detail::firstMarkOn).
struct SchemeMarkChain
{
    SchemeSlot slot;                        ///< The mark slot.
    std::span<SchemeSlot const> candidates; ///< What it is chosen from, in order; the last always marks.
};

/// The mark slots: chosen against the default profile's background in chromeSchemeColorsOf(), and against a
/// pane's own by markOn().
inline constexpr std::array SchemeMarkChains {
    SchemeMarkChain { .slot = SchemeSlot::ScrollbarThumb, .candidates = ScrollbarThumbFallbacks },
    SchemeMarkChain { .slot = SchemeSlot::ScrollbarThumbActive, .candidates = ScrollbarThumbActiveFallbacks },
};

/// Number of @c ChromeColorRole enumerators, derived from the last one.
inline constexpr size_t ChromeColorRoleCount = static_cast<size_t>(ChromeColorRole::TooltipEdge) + 1;

// Each family of colour roles is counted by its own two ends, End - Begin, never from a neighbour's End: a
// family added between two others then shifts no count.

/// The first pane colour role.
inline constexpr size_t PaneColorRoleBegin = static_cast<size_t>(ChromeColorRole::PaneGapFill);
/// One past the last pane colour role.
inline constexpr size_t PaneColorRoleEnd = static_cast<size_t>(ChromeColorRole::PaneHeaderActionHover) + 1;

/// The first command palette colour role.
inline constexpr size_t PaletteColorRoleBegin = static_cast<size_t>(ChromeColorRole::PaletteSurface);
/// One past the last command palette colour role.
inline constexpr size_t PaletteColorRoleEnd = static_cast<size_t>(ChromeColorRole::PaletteSeparator) + 1;

/// The first find bar colour role.
inline constexpr size_t FindColorRoleBegin = static_cast<size_t>(ChromeColorRole::FindToggleOnFill);
/// One past the last find bar colour role.
inline constexpr size_t FindColorRoleEnd = static_cast<size_t>(ChromeColorRole::FindErrorText) + 1;

/// The first overlay colour role: the link pill's hint, then the overlay scrollbar's.
inline constexpr size_t OverlayColorRoleBegin = static_cast<size_t>(ChromeColorRole::ToastTextSecondary);
/// The first of the overlay scrollbar's roles, which a pane resolves on its own background (colorOn).
inline constexpr size_t ScrollbarColorRoleBegin = static_cast<size_t>(ChromeColorRole::ScrollbarThumb);
/// One past the last overlay colour role.
inline constexpr size_t OverlayColorRoleEnd = static_cast<size_t>(ChromeColorRole::ScrollbarTrack) + 1;

/// The first keyboard focus colour role.
inline constexpr size_t FocusColorRoleBegin = static_cast<size_t>(ChromeColorRole::FocusRing);
/// One past the last keyboard focus colour role.
inline constexpr size_t FocusColorRoleEnd = static_cast<size_t>(ChromeColorRole::FocusFill) + 1;

/// The first Settings window colour role.
inline constexpr size_t SettingsColorRoleBegin = static_cast<size_t>(ChromeColorRole::SettingsSidebarFill);
/// One past the last Settings window colour role.
inline constexpr size_t SettingsColorRoleEnd = static_cast<size_t>(ChromeColorRole::PreviewFrame) + 1;

/// One chrome colour: a source kind plus the field that kind reads.
struct ChromeColorSource
{
    ColorSourceKind kind = ColorSourceKind::Fixed;    ///< Where the colour comes from.
    uint32_t rgb = 0;                                 ///< @c 0xRRGGBB, for @c Fixed.
    StatusColorRole status = StatusColorRole::Danger; ///< The hue, for @c Status.
    SchemeSlot slot = SchemeSlot::Background;         ///< The slot, for @c Scheme and @c SchemeOnBar.
    int alphaPercent = 100;                           ///< Opacity of the resulting colour.
    /// The 1-based step of a @c Fill, @c Label or @c Surface source, the signed lightness shift of a
    /// @c SchemeShade one; 0 for other kinds.
    int level = 0;
    uint32_t darkRgb = 0;       ///< @c 0xRRGGBB on a dark chrome, for @c Tone.
    int darkAlphaPercent = 100; ///< Opacity on a dark chrome, for @c Tone.

    [[nodiscard]] bool operator==(ChromeColorSource const&) const = default;
};

/// One alpha of the kit's tables on each chrome, as a percentage.
struct AlphaPair
{
    int light = 0; ///< Over a light chrome, in black.
    int dark = 0;  ///< Over a dark chrome, in white.

    [[nodiscard]] bool operator==(AlphaPair const&) const = default;
};

/// The kit's @c fill.1-4 washes (kit-tokens), @c fill.1 first.
inline constexpr std::array<AlphaPair, 4> FillAlphas {
    AlphaPair { .light = 20, .dark = 24 },
    AlphaPair { .light = 12, .dark = 16 },
    AlphaPair { .light = 8, .dark = 10 },
    AlphaPair { .light = 5, .dark = 6 },
};

/// The kit's @c label.primary/secondary/tertiary text (kit-tokens), primary first.
inline constexpr std::array<AlphaPair, 3> LabelAlphas {
    AlphaPair { .light = 88, .dark = 92 },
    AlphaPair { .light = 60, .dark = 62 },
    AlphaPair { .light = 45, .dark = 50 },
};

/// One opaque surface of the kit on each chrome, as @c 0xRRGGBB.
struct SurfacePair
{
    uint32_t light = 0; ///< On a light chrome.
    uint32_t dark = 0;  ///< On a dark chrome.

    [[nodiscard]] bool operator==(SurfacePair const&) const = default;
};

/// The kit's @c surface.elevated and @c surface.secondary (kit-tokens), elevated first.
inline constexpr std::array<SurfacePair, 2> SurfaceColors {
    SurfacePair { .light = 0xFFFFFF, .dark = 0x2C2C2E },
    SurfacePair { .light = 0xF2F2F7, .dark = 0x1C1C1E },
};

/// The terminal colour scheme reduced to the slots the chrome reads, indexed by @c SchemeSlot.
struct ChromeSchemeColors
{
    /// One colour per @c SchemeSlot. Not named `slots`: Qt defines that as a keyword macro, and this
    /// header reaches Qt translation units through the chrome provider.
    std::array<vtbackend::RGBColor, SchemeSlotCount> bySlot {};

    [[nodiscard]] bool operator==(ChromeSchemeColors const&) const = default;
};

/// Where a style hosts the find bar.
enum class FindBarHost : uint8_t
{
    Overlay = 0,   //!< Floating over the pane's terminal, at its top right.
    PaneHeaderRow, //!< In the pane header's row, where the pane has one (the Terminal row).
};

/// Component extents. Logical pixels for the native rows, character cells for the Terminal row.
struct SizeTokens
{
    int titleBar = 0;          ///< Title bar height.
    int tabWell = 0;           ///< Height of the tab well below the title bar, 0 when tabs sit in it.
    int tab = 0;               ///< Tab height.
    int control = 0;           ///< Height of a button or field.
    int paneHeader = 0;        ///< Height of a pane header.
    int toast = 0;             ///< Height of a toast.
    int tooltipPadX = 0;       ///< A tooltip's leading and trailing padding (horizontal units).
    int tooltipPadTop = 0;     ///< A tooltip's top padding (vertical units).
    int tooltipPadBottom = 0;  ///< A tooltip's bottom padding (vertical units).
    int tooltipEdge = 0;       ///< A tooltip's edge in whole pixels; 0 draws one device pixel.
    int tooltipGap = 0;        ///< The gap between a tooltip's label and its shortcut (horizontal units).
    int tooltipMaxWidth = 0;   ///< The widest a tooltip's text grows before it wraps (horizontal units).
    int dialogWidth = 0;       ///< Width of a dialog.
    int swatchWidth = 0;       ///< Width of a menu's colour swatch.
    int swatchHeight = 0;      ///< Height of a menu's colour swatch.
    int settingsWidth = 0;     ///< Default width of the Settings window.
    int settingsHeight = 0;    ///< Default height of the Settings window.
    int settingsMinWidth = 0;  ///< Smallest width the Settings window may be resized to.
    int settingsMinHeight = 0; ///< Smallest height the Settings window may be resized to.

    // The title band and its tabs.
    int tabIcon = 0;       ///< Edge of a tab's icon square.
    int tabClose = 0;      ///< Edge of a tab's close button.
    int badge = 0;         ///< Diameter of a badge disc or ring.
    int badgeDot = 0;      ///< Diameter of the unseen-output dot.
    int progressBar = 0;   ///< Height of a tab's progress bar.
    int hostChip = 0;      ///< Height of the host chip.
    int hostDot = 0;       ///< Diameter of the host chip's state dot.
    int toolbarButton = 0; ///< Width of a band toolbar button.
    int toolbarChip = 0;   ///< Height of a band toolbar chip.
    /// Height of the "+" and profile buttons in a @c TabShape::Joined strip; 0 on the rows that do not join.
    int stripButton = 0;
    /// Width of the profile button in a @c TabShape::Joined strip; 0 on the rows that do not join.
    int profileButton = 0;

    // The tab colour flyout.
    int flyoutSwatch = 0; ///< Edge of a swatch in the tab colour flyout (bigger than a menu's).

    // The panes.
    int bareBand = 0;         ///< Height of a band with neither tabs nor a title (Minimal, one tab).
    int grabber = 0;          ///< Thickness of the split handle's grabber; 0 draws none.
    int grabberLength = 0;    ///< Length of the split handle's grabber.
    int paneHeaderAction = 0; ///< Hit extent of a pane header's action button.
    int paneFooter = 0;       ///< Height of the exited footer.
    int footerButton = 0;     ///< Height of an exited footer's button.
    /// Width of the shell-integration card; 0 where the pane's width less two cells is used (Terminal).
    int noticeCard = 0;

    // The command palette.
    int paletteWidth = 0;       ///< Width of the palette (horizontal units).
    int paletteMaxHeight = 0;   ///< Height the palette grows to at most (vertical units).
    int paletteField = 0;       ///< Height of the query row (vertical units).
    int paletteRow = 0;         ///< Height of a row (vertical units).
    int paletteGroupHeader = 0; ///< Height of a group header (vertical units).
    int paletteFooter = 0;      ///< Height of the footer (vertical units).

    // The Connect picker.
    int connectWidth = 0;  ///< Default width of the picker's window (horizontal units).
    int connectHeight = 0; ///< Default height of the picker's window (vertical units).
    int connectRow = 0;    ///< Height of a two-line target row (vertical units).

    // The find bar.
    int findBar = 0;          ///< Height of the find bar.
    int findBarWidth = 0;     ///< Width of the find bar.
    int findToggleWidth = 0;  ///< Width of a find option toggle.
    int findToggleHeight = 0; ///< Height of a find option toggle.
    int findButtonWidth = 0;  ///< Width of the previous, next and close buttons.
    int findButtonHeight = 0; ///< Height of the previous, next and close buttons.
    int findIcon = 0;         ///< Edge of the magnifier; 0 draws none.

    // The overlays.
    int linkPill = 0;  ///< Height of the link pill (vertical units).
    int scrollbar = 0; ///< Width of the overlay scrollbar's track (horizontal units).

    // The secure-input lock.
    int secureInputGlyph = 0; ///< Edge of the lock's icon; 0 where the Terminal row draws it as cell text.

    // Keyboard focus.
    int focusRing = 0; ///< Outset of the keyboard focus ring (horizontal units); 0 draws none.

    // The command blocks.
    int rail = 0;              ///< Width of the block rail's bar (horizontal units).
    int railMark = 0;          ///< Edge of the rail's mark: the ✗ disc, the ▸ and ✓ boxes (horizontal units).
    int blockActions = 0;      ///< Edge of the square ⋯ button (horizontal units).
    int foldChip = 0;          ///< Drawn height of the fold chip, clamped to the row (vertical units).
    int foldChipHit = 0;       ///< Smallest hit width and height of the fold chip.
    int scrollTick = 0;        ///< Height of a scrollbar tick (vertical units).
    int scrollTickOutline = 0; ///< Stroke of a hollow scrollbar tick (horizontal units).

    // The header bar and its window buttons.
    int windowButton = 0;      ///< Edge of a round window button; 0 where the row draws none.
    int windowButtonGlyph = 0; ///< Edge of a window button's glyph icon.
    int headerIcon = 0;        ///< Edge of a header-bar button's icon.
    int controlsSeparator = 0; ///< Height of the separator before the window buttons; 0 draws none.

    [[nodiscard]] bool operator==(SizeTokens const&) const = default;
};

/// Gaps and the spacing scale s1..s10.
struct SpaceTokens
{
    int paneGap = 0;              ///< Gap between split panes.
    int splitHit = 0;             ///< Hit target of a split divider.
    int paneInset = 0;            ///< Inset of the pane cards from the content edge.
    std::array<int, 10> scale {}; ///< The spacing scale, s1 first.

    // The title band.
    int dragReserve = 0;        ///< Empty band that always stays draggable.
    int revealEdge = 0;         ///< Height of the full-screen edge that reveals the band.
    int toolbarChipPadding = 0; ///< Horizontal padding inside a toolbar chip.
    /// Inset from the band's leading edge to a @c TabShape::Joined strip's first tab; 0 on the rows that do
    /// not join.
    int tabStripLeading = 0;

    // The panes.
    int paneGapRows =
        0; ///< @c paneGap between stacked panes, in vertical units (a cell's height on Terminal).
    int paneHeaderGap = 0;  ///< Gap between a pane header's items.
    int footerLeading = 0;  ///< Leading padding of the exited footer.
    int noticeCardPadX = 0; ///< Horizontal padding inside the shell-integration card.
    int noticeCardPadY = 0; ///< Vertical padding inside the shell-integration card.

    int paletteInset = 0; ///< The palette rows' horizontal inset and the list's side padding.

    // The find bar.
    int findBarInset = 0;     ///< The find bar's inset from the pane's right edge.
    int findBarInsetRows = 0; ///< Its inset from the pane's top edge, in vertical units (a row on Terminal).
    /// What the count keeps from the first toggle beyond the bar's own spacing: one cell on Terminal, whose
    /// spacing is none, so "2 of 5" does not run into "[Aa]"; none where the spacing already parts them.
    int findBarCountGap = 0;

    int scrollbarInset = 0; ///< The overlay scrollbar's gap to the pane's edge, inside its hit area.

    // The command blocks.
    int blockGutter = 0; ///< Width of the strip the rail sits in, after the gutter's cells; 0 has none.
    int railInset = 0;   ///< The rail bar's left edge within the strip.
    int railEnd = 0;     ///< What the bar keeps clear of its first row's top and its last row's bottom.

    [[nodiscard]] bool operator==(SpaceTokens const&) const = default;
};

/// Corner radii.
struct RadiusTokens
{
    int control = 0;    ///< Buttons and fields.
    int popover = 0;    ///< Popovers.
    int menu = 0;       ///< Menus.
    int menuRow = 0;    ///< A menu row's highlight.
    int paneCard = 0;   ///< Pane cards.
    int noticeCard = 0; ///< The shell-integration card.
    int toast = 0;      ///< Toasts.
    int tooltip = 0;    ///< Tooltips.

    // The title band.
    int tab = 0;         ///< A tab pill.
    int tabWell = 0;     ///< The well the pills sit in.
    int tabIcon = 0;     ///< A tab's icon square.
    int tabClose = 0;    ///< A tab's close button.
    int badge = 0;       ///< A badge disc.
    int hostChip = 0;    ///< The host chip.
    int toolbarChip = 0; ///< A toolbar chip.

    int grabber = 0; ///< The split handle's grabber.
    int findBar = 0; ///< The find bar.

    int linkPill = 0;  ///< The link pill.
    int scrollbar = 0; ///< The overlay scrollbar's thumb.

    int rail = 0;         ///< The block rail's bar ends.
    int blockActions = 0; ///< The ⋯ button.
    int chip = 0;         ///< The status and fold chips.

    int window = 0;       ///< The window's top corners where the client draws its frame.
    int windowButton = 0; ///< A round window button.

    int quickTerminal = 0; ///< The quick terminal's bottom corners.

    [[nodiscard]] bool operator==(RadiusTokens const&) const = default;
};

/// Chrome font sizes; 0 means the chrome font's own size.
struct TypeTokens
{
    double caption = 0.0;  ///< Caption text.
    double footnote = 0.0; ///< Footnote text.
    double body = 0.0;     ///< Body text.
    double tab = 0.0;      ///< Tab titles.
    double title = 0.0;    ///< Titles.

    double paneHeader = 0.0;       ///< A pane header's text.
    int paneHeaderWeight = 400;    ///< A pane header's font weight, on the CSS 100-900 scale.
    double paneHeaderAction = 0.0; ///< A pane header button's glyph.

    double paletteInput = 0.0; ///< The command palette's query.
    double shortcut = 0.0;     ///< A command palette row's shortcut.

    [[nodiscard]] bool operator==(TypeTokens const&) const = default;
};

/// Animation durations and travel.
struct MotionTokens
{
    int fastMs = 0;       ///< Fast transitions, in milliseconds.
    int standardMs = 0;   ///< Standard transitions, in milliseconds.
    int emphasizedMs = 0; ///< Emphasised transitions, in milliseconds.
    int travel = 0;       ///< Distance a sliding element travels.

    int spinnerMs = 0; ///< One turn of the Working spinner, in milliseconds.
    int pulseMs = 0;   ///< One half-cycle of the indeterminate progress pulse, in milliseconds.
    MotionStyle style = MotionStyle::Full; ///< Whether things slide, spin and pulse, or cross-fade.

    /// How long the reveal modifier is held alone before Minimal's headers show, in milliseconds. A
    /// hold rather than an animation, so Reduce Motion leaves it be.
    int headerRevealMs = 0;

    /// The scale a popover opens from, in percent; 100 opens at full size (a fade only).
    int popoverScalePercent = 0;

    /// How long the size indicator stays after the last change, in milliseconds. A hold rather than an
    /// animation, so Reduce Motion leaves it be.
    int lingerMs = 0;

    /// How long a pointer rests on a control before its first tooltip shows, in milliseconds. A hold
    /// rather than an animation, so Reduce Motion leaves it be.
    int tooltipDelayMs = 0;

    /// How long after a tooltip was last shown the next one shows with no delay, in milliseconds.
    int tooltipWarmMs = 0;

    [[nodiscard]] bool operator==(MotionTokens const&) const = default;
};

/// One drop shadow.
struct ElevationLevel
{
    int offsetX = 0;        ///< Horizontal offset.
    int offsetY = 0;        ///< Vertical offset.
    int blur = 0;           ///< Blur radius.
    int opacityPercent = 0; ///< Shadow opacity.

    [[nodiscard]] bool operator==(ElevationLevel const&) const = default;
};

/// The shadows e0..e3.
struct ElevationTokens
{
    std::array<ElevationLevel, 4> levels {}; ///< e0 first.

    [[nodiscard]] bool operator==(ElevationTokens const&) const = default;
};

/// Surface materials.
struct MaterialTokens
{
    Material titleBand = Material::None; ///< The title band.
    Material popover = Material::None;   ///< Popovers and menus.

    [[nodiscard]] bool operator==(MaterialTokens const&) const = default;
};

/// Separation strength; the accessibility contrast preference adjusts these.
struct ContrastTokens
{
    int separatorPercent = 0;             ///< Opacity of separators.
    double activeCardRing = 0.0;          ///< Width of the active pane card's ring.
    std::optional<int> paneDimPercent {}; ///< Dimming of inactive panes, when set.

    [[nodiscard]] bool operator==(ContrastTokens const&) const = default;
};

/// Terminal-style specifics.
struct TuiTokens
{
    double inactivePaneOpacity = 0.0;             ///< Opacity of an inactive pane.
    ToastStyle toastStyle = ToastStyle::Floating; ///< How toasts are painted.

    [[nodiscard]] bool operator==(TuiTokens const&) const = default;
};

/// Tints laid over chrome surfaces.
struct TintTokens
{
    int tabPercent = 0; ///< Opacity of a tab colour over its pill; 0 where the colour is a glyph instead.

    [[nodiscard]] bool operator==(TintTokens const&) const = default;
};

/// How the Settings window is laid out; the row's @c layout.settings.
enum class SettingsLayout : uint8_t
{
    InsetGrouped = 0, //!< Rows grouped in one inset card per group, a toolbar above (macOS).
    Cards,            //!< One card per row, a breadcrumb above (Fluent).
    BoxedList,        //!< Boxed lists under a split header bar (ContourLinux).
    Cells,            //!< Rows of cells, a toolbar of cells above (the Terminal style).
};

/// Where a page's title sits.
enum class SettingsHeader : uint8_t
{
    Toolbar = 0,    //!< A centred title in the window's toolbar.
    Breadcrumb,     //!< A large title with the section as a breadcrumb.
    SplitHeaderBar, //!< A header bar over the page, split from the sidebar's own.
};

/// How a page's rows are shaped.
enum class SettingsRowShape : uint8_t
{
    Grouped = 0,   //!< The rows of a group share one card, divided by separators.
    SeparateCards, //!< Each row is a card of its own, with a gap between.
};

/// How a stepper's arrows sit by its value field.
enum class StepperArrangement : uint8_t
{
    Stacked = 0, //!< A column of two arrows beside the field.
    SideBySide,  //!< A minus and a plus either side of the field.
    Inline,      //!< Both arrows inside the field's right end.
};

/// The choices a @c SettingsLayout stands for, so QML and C++ read a trait rather than compare layouts.
struct SettingsLayoutTraits
{
    SettingsHeader header = SettingsHeader::Toolbar;          ///< Where the page title sits.
    SettingsRowShape rows = SettingsRowShape::Grouped;        ///< How the rows are shaped.
    StepperArrangement stepper = StepperArrangement::Stacked; ///< How a stepper's arrows sit.

    [[nodiscard]] bool operator==(SettingsLayoutTraits const&) const = default;
};

/// The traits of each @c SettingsLayout, indexed by it.
inline constexpr std::array SettingsLayoutTraitsTable {
    SettingsLayoutTraits { .header = SettingsHeader::Toolbar,
                           .rows = SettingsRowShape::Grouped,
                           .stepper = StepperArrangement::Stacked }, // InsetGrouped
    SettingsLayoutTraits { .header = SettingsHeader::Breadcrumb,
                           .rows = SettingsRowShape::SeparateCards,
                           .stepper = StepperArrangement::Inline }, // Cards
    SettingsLayoutTraits { .header = SettingsHeader::SplitHeaderBar,
                           .rows = SettingsRowShape::Grouped,
                           .stepper = StepperArrangement::SideBySide }, // BoxedList
    SettingsLayoutTraits { .header = SettingsHeader::Toolbar,
                           .rows = SettingsRowShape::Grouped,
                           .stepper = StepperArrangement::Stacked }, // Cells
};

/// Number of @c SettingsLayout enumerators, derived from the last one.
inline constexpr size_t SettingsLayoutCount = static_cast<size_t>(SettingsLayout::Cells) + 1;

static_assert(SettingsLayoutTraitsTable.size() == SettingsLayoutCount,
              "Every SettingsLayout needs exactly one row of traits.");

/// @param layout A Settings layout.
/// @return The choices it stands for.
[[nodiscard]] constexpr SettingsLayoutTraits settingsLayoutTraits(SettingsLayout layout) noexcept
{
    return SettingsLayoutTraitsTable[static_cast<size_t>(layout)];
}

/// The Settings window's extents, in the units of the row (points; cells on the Terminal row), and its type
/// sizes in points (0: the chrome font's own). The window's own four extents are @c SizeTokens::settings*.
struct SettingsTokens
{
    int sidebarWidth = 0;  ///< The sidebar's width.
    int sidebarRow = 0;    ///< A sidebar row's height.
    int iconTile = 0;      ///< The tile behind a sidebar icon, its edge; 0: no tile.
    int iconGlyph = 0;     ///< A sidebar icon's edge.
    int searchField = 0;   ///< The search field's height.
    int headerBar = 0;     ///< The page header's height.
    int listMaxWidth = 0;  ///< The widest the list of groups grows; 0: it fills the page.
    int groupRadius = 0;   ///< A group's (or a card's) corner radius.
    int row = 0;           ///< A row's height.
    int rowWithHelp = 0;   ///< A row's height when it carries a help line.
    int rowPadding = 0;    ///< A row's horizontal padding.
    int cardGap = 0;       ///< The gap between the rows of one group.
    int groupGap = 0;      ///< The gap between groups.
    int groupTitleGap = 0; ///< The gap from a group's title to its list.
    int control = 0;       ///< A pop-up, button or field's height.
    int switchWidth = 0;   ///< A switch's width.
    int switchHeight = 0;  ///< A switch's height.
    int slider = 0;        ///< A slider's track width.
    int stepperField = 0;  ///< A stepper's value field width.
    int colorWell = 0;     ///< A colour well's edge.
    int previewHeight = 0; ///< The live preview's height.
    int previewWidth = 0;  ///< The live preview's width; 0: it fills the page.
    int previewRadius = 0; ///< The live preview's corner radius.
    /// How much of a row's label and control remain where it cannot be edited, in percent.
    int disabledPercent = 0;
    double labelType = 0.0;      ///< A row's label.
    double helpType = 0.0;       ///< A row's help line.
    double groupTitleType = 0.0; ///< A group's title.
    double pageTitleType = 0.0;  ///< A page's title.

    [[nodiscard]] bool operator==(SettingsTokens const&) const = default;
};

/// Layout choices that differ per style.
struct LayoutTokens
{
    DialogPlacement dialog = DialogPlacement::Centered; ///< Where dialogs appear.
    MenuPresentation menu = MenuPresentation::InScene;  ///< How menus are presented; read by C++ only.
    TabShape tabShape = TabShape::Flat;                 ///< How the tabs are drawn.
    ProfileMenuAffordance profileMenu = ProfileMenuAffordance::Caret; ///< How the profile menu is reached.
    TabColorSource tabColors = TabColorSource::Palette;               ///< Where a tab's colours come from.
    ActiveTabWeight activeTabWeight = ActiveTabWeight::Regular;       ///< How the active label stands out.
    PaneActionsReveal paneActions = PaneActionsReveal::OnHover; ///< When a pane header shows its actions.
    int paletteTopPercent = 0; ///< The command palette's top edge, in percent of the window's height.
    FindBarHost findBar = FindBarHost::Overlay; ///< Where the find bar sits.
    /// How the gutter draws the block column: a pixel strip holding the rail, or the glyph cells.
    vtbackend::BlockColumnForm blockColumn = vtbackend::BlockColumnForm::Glyphs;
    SettingsLayout settings = SettingsLayout::InsetGrouped;      ///< How the Settings window is laid out.
    HeaderBarArrangement headerBar = HeaderBarArrangement::Band; ///< How the window's top row is arranged.
    TabCloseMark tabClose = TabCloseMark::Glyph;                 ///< What a tab's close button shows.

    [[nodiscard]] bool operator==(LayoutTokens const&) const = default;
};

/// One block chip style's pill, in points (the canvas's `blocks` board).
struct PillTokens
{
    double type = 0.0; ///< The label's size.
    int weight = 400;  ///< The label's weight, on the CSS 100-900 scale.
    int padX = 0;      ///< The padding left and right of the label.
    int padY = 0;      ///< The padding above and below it, for a pill fitted to its label.
    int height = 0;    ///< The pill's height, clamped to the row; 0 fits it to the label and padding.
    int radius = 0;    ///< Its corners' radius.
    double ring = 0.0; ///< The ring inside its edge; 0 has none.

    [[nodiscard]] bool operator==(PillTokens const&) const = default;
};

/// The native rows' block chips and rail marks, drawn by the renderer in the UI font. All zero on the
/// Terminal row, whose chips are terminal text.
struct BlockChipTokens
{
    std::array<PillTokens, vtbackend::PillStyleCount> pills {}; ///< By vtbackend::PillStyle.
    double markType = 0.0; ///< The glyph in a rail mark (size.railMark is its disc).
    int markWeight = 400;  ///< Its weight.
    int hoverRing = 0;     ///< The ring around a hovered block, inside its outline; 0 draws none.

    [[nodiscard]] bool operator==(BlockChipTokens const&) const = default;
};

/// How strongly an unfocused pane dims, by the chrome's tone (the kit's @c pane.dim).
struct DimTokens
{
    int darkPercent = 0;  ///< Opacity of the black scrim on a dark chrome.
    int lightPercent = 0; ///< Opacity of the black scrim on a light chrome.

    [[nodiscard]] bool operator==(DimTokens const&) const = default;
};

/// The extents @c ChromeDensity::Minimal replaces, in the units of the groups they replace.
struct MinimalDensityTokens
{
    int paneHeader = 0;     ///< Replaces @c size.paneHeader.
    int paneGap = 0;        ///< Replaces @c space.paneGap.
    int paneGapRows = 0;    ///< Replaces @c space.paneGapRows.
    int splitHit = 0;       ///< Replaces @c space.splitHit.
    int paneInset = 0;      ///< Replaces @c space.paneInset.
    int paneCardRadius = 0; ///< Replaces @c radii.paneCard.

    [[nodiscard]] bool operator==(MinimalDensityTokens const&) const = default;
};

/// One style's complete 1a token set.
struct ChromeTokenRow
{
    SizeTokens size {};           ///< Component extents.
    SpaceTokens space {};         ///< Gaps and the spacing scale.
    RadiusTokens radii {};        ///< Corner radii.
    TypeTokens type {};           ///< Chrome font sizes.
    MotionTokens motion {};       ///< Animation durations and travel.
    ElevationTokens elevation {}; ///< Drop shadows e0..e3.
    MaterialTokens material {};   ///< Surface materials.
    ContrastTokens contrast {};   ///< Separation strength.
    TuiTokens tui {};             ///< Terminal-style specifics.
    LayoutTokens layout {};       ///< Per-style layout choices.
    TintTokens tint {};           ///< Tints over chrome surfaces.
    std::array<ChromeColorSource, ChromeColorRoleCount>
        colors {}; ///< Colour sources, indexed by @c ChromeColorRole.
    /// What each status role is inked in on this row's chrome, indexed by @c StatusColorRole.
    std::array<ChromeColorSource, StatusColorRoleCount> statusInk {};
    DimTokens dim {};                ///< How strongly an unfocused pane dims.
    MinimalDensityTokens minimal {}; ///< The extents the Minimal density replaces.
    BlockChipTokens blockChips {};   ///< The native rows' block chips and rail marks.
    SettingsTokens settings {};      ///< The Settings window's extents and type.
};

/// The WCAG AA ratio chrome text must reach against what it sits on: the terminal's own, one entity, so a
/// scope that sees both namespaces finds no ambiguity.
using vtbackend::MinimumTextContrast;

/// The WCAG 1.4.11 ratio a chrome mark (a dot, a status glyph) must reach against what it sits on: the
/// terminal's own, one entity, as MinimumTextContrast is.
using vtbackend::MinimumMarkContrast;

namespace detail
{
    /// A colour's HSL hue, saturation and lightness, each normalized to [0, 1].
    struct Hsl
    {
        double hue = 0.0;        ///< Hue.
        double saturation = 0.0; ///< Saturation.
        double lightness = 0.0;  ///< Lightness.
    };

    /// @param color An sRGB colour.
    /// @return Its HSL coordinates, the inverse of vtbackend::hslToRgb.
    [[nodiscard]] constexpr Hsl hslOf(vtbackend::RGBColor color) noexcept
    {
        auto const r = color.red / 255.0;
        auto const g = color.green / 255.0;
        auto const b = color.blue / 255.0;
        auto const high = std::max({ r, g, b });
        auto const low = std::min({ r, g, b });
        auto const lightness = (high + low) / 2;
        if (high == low)
            return Hsl { .hue = 0.0, .saturation = 0.0, .lightness = lightness };
        auto const delta = high - low;
        auto const saturation = lightness > 0.5 ? delta / (2 - high - low) : delta / (high + low);
        auto const sector = [&] {
            if (high == r)
                return ((g - b) / delta) + (g < b ? 6 : 0);
            if (high == g)
                return ((b - r) / delta) + 2;
            return ((r - g) / delta) + 4;
        }();
        return Hsl { .hue = sector / 6, .saturation = saturation, .lightness = lightness };
    }
} // namespace detail

/// @p color with its HSL lightness moved by @p points (out of 100) and clamped, hue and saturation kept.
/// @param color  The colour to move.
/// @param points The lightness points to move it by; negative darkens.
/// @return The moved colour, converted back by vtbackend::hslToRgb (which truncates each channel).
[[nodiscard]] constexpr vtbackend::RGBColor shiftLightness(vtbackend::RGBColor color, int points) noexcept
{
    auto const hsl = detail::hslOf(color);
    return vtbackend::hslToRgb(
        hsl.hue, hsl.saturation, std::clamp(hsl.lightness + (points / 100.0), 0.0, 1.0));
}

/// Duration every transition collapses to under @c MotionPreference::Reduced.
inline constexpr int ReducedMotionCrossFadeMs = 100;

/// The selection colour a palette paints, with a cell-colour selection resolved to that end of the scheme.
/// @param palette The terminal colour palette.
/// @return The selection background, as the terminal draws it.
[[nodiscard]] inline vtbackend::RGBColor schemeSelectionOf(vtbackend::ColorPalette const& palette)
{
    if (auto const* rgb = std::get_if<vtbackend::RGBColor>(&palette.selection.background))
        return *rgb;
    if (std::holds_alternative<vtbackend::CellForegroundColor>(palette.selection.background))
        return palette.defaultForeground;
    return palette.defaultBackground;
}

/// Where the chrome's selection comes from, in the order it is tried.
enum class SelectionSource : uint8_t
{
    Scheme = 0, //!< The scheme's own selection, with its better end of the scheme on top.
    Inverse,    //!< Inverse video: the foreground as the fill, the background as the text.
    Bar,        //!< The bar and its ink (vtbackend::legibleInkOn()): always legible.
    BarInverse, //!< The bar's ink as the fill, the bar as the text: legible, and stands off the bar.
};

/// Where the Terminal row's toast takes its colours from, in the order it is tried.
enum class ToastSource : uint8_t
{
    Inverse = 0, //!< Inverse video: the foreground as the fill, the background as the text.
    Bar,         //!< The bar and its ink (vtbackend::legibleInkOn()): always legible.
};

/// The candidates for the Terminal row's toast, first legible one wins: some schemes' two ends do not read
/// on each other at 4.5:1 (one-dark).
inline constexpr std::array ToastFallbacks { ToastSource::Inverse, ToastSource::Bar };

/// The candidates for the chrome's selection, first legible one wins. Most bundled schemes inherit a
/// saturated blue selection on which neither end of the scheme reaches 4.5:1 (one-dark, solarized-dark);
/// the last candidate always reads, so the active tab does on every scheme.
inline constexpr std::array SelectionFallbacks {
    SelectionSource::Scheme,
    SelectionSource::Inverse,
    SelectionSource::Bar,
};

/// The candidates for a selection drawn on the bar, first one that reads and stands off the bar wins: the
/// last one always does, the bar's ink reading on the bar at 4.5:1 both ways round.
inline constexpr std::array BarSelectionFallbacks {
    SelectionSource::Scheme,
    SelectionSource::Inverse,
    SelectionSource::BarInverse,
};

namespace detail
{
    /// A text colour on the fill it is read on.
    struct InkPair
    {
        vtbackend::RGBColor fill {}; ///< The ground.
        vtbackend::RGBColor text {}; ///< The ink on it.
    };

    /// The first candidate whose text reads on its fill at 4.5:1; failing all, the one that reads best.
    /// @param candidates The sources, in the order they are tried; not empty.
    /// @param pairOf     Maps a source to its pair.
    /// @return The chosen pair.
    template <typename Source, size_t N, typename PairOf>
    [[nodiscard]] InkPair firstLegiblePair(std::array<Source, N> const& candidates, PairOf const& pairOf)
    {
        static_assert(N > 0);
        auto const ratioOf = [](InkPair const& pair) {
            return vtbackend::contrastRatio(pair.text, pair.fill);
        };
        auto best = pairOf(candidates.front());
        for (auto const source: candidates)
        {
            auto const candidate = pairOf(source);
            if (ratioOf(candidate) >= MinimumTextContrast)
                return candidate;
            if (ratioOf(candidate) > ratioOf(best))
                best = candidate;
        }
        return best;
    }

    /// The first candidate whose text reads on its fill at 4.5:1 and whose fill stands off @p ground at
    /// 3:1 (a mark, WCAG 1.4.11); failing all, the last.
    /// @param candidates The sources, in the order they are tried; not empty.
    /// @param ground     What the fill is drawn on.
    /// @param pairOf     Maps a source to its pair.
    /// @return The chosen pair.
    template <typename Source, size_t N, typename PairOf>
    [[nodiscard]] InkPair firstMarkedPair(std::array<Source, N> const& candidates,
                                          vtbackend::RGBColor ground,
                                          PairOf const& pairOf)
    {
        static_assert(N > 0);
        for (auto const source: candidates)
        {
            auto const candidate = pairOf(source);
            if (vtbackend::contrastRatio(candidate.text, candidate.fill) >= MinimumTextContrast
                && vtbackend::contrastRatio(candidate.fill, ground) >= MinimumMarkContrast)
                return candidate;
        }
        return pairOf(candidates.back());
    }

    /// The first of @p candidates whose colour stands off @p ground at 3:1 (a mark, WCAG 1.4.11); failing
    /// all, the last.
    /// @param colors     The scheme's slots, those named in @p candidates already resolved.
    /// @param candidates The slots to try, in order; not empty.
    /// @param ground     What the mark is drawn on.
    /// @return The chosen colour.
    [[nodiscard]] inline vtbackend::RGBColor firstMarkOn(ChromeSchemeColors const& colors,
                                                         std::span<SchemeSlot const> candidates,
                                                         vtbackend::RGBColor ground)
    {
        for (auto const slot: candidates)
        {
            auto const color = colors.bySlot[static_cast<size_t>(slot)];
            if (vtbackend::contrastRatio(color, ground) >= MinimumMarkContrast)
                return color;
        }
        return colors.bySlot[static_cast<size_t>(candidates.back())];
    }
} // namespace detail

/// Reduces a terminal colour palette to the slots the Terminal chrome paints with.
/// @param palette The active terminal colour palette.
/// @return One colour per @c SchemeSlot.
[[nodiscard]] inline ChromeSchemeColors chromeSchemeColorsOf(vtbackend::ColorPalette const& palette)
{
    // ANSI colour 0 is black in a light scheme as in a dark one, but a light scheme's foreground is dark
    // too: text on the bar (and on the selection) takes whichever end of the scheme stands off it.
    auto const bar = palette.normalColor(0);
    auto const readableOn = [&palette](vtbackend::RGBColor fill) {
        return vtbackend::contrastRatio(palette.defaultForeground, fill)
                       >= vtbackend::contrastRatio(palette.defaultBackground, fill)
                   ? palette.defaultForeground
                   : palette.defaultBackground;
    };

    // The bar's ink, resolved once: the bar's text, the Terminal row's selection fallback and every
    // SchemeOnBar source read this slot. The status line's roles take the same chain
    // (vtbackend::InkFallbacks) on their own bars.
    auto const barInk = vtbackend::legibleInkOn(palette, bar);

    auto const pairOf = [&](SelectionSource source) {
        switch (source)
        {
            case SelectionSource::Scheme: {
                auto const own = schemeSelectionOf(palette);
                return detail::InkPair { .fill = own, .text = readableOn(own) };
            }
            case SelectionSource::Inverse:
                return detail::InkPair { .fill = palette.defaultForeground,
                                         .text = palette.defaultBackground };
            case SelectionSource::BarInverse: return detail::InkPair { .fill = barInk, .text = bar };
            case SelectionSource::Bar: break;
        }
        return detail::InkPair { .fill = bar, .text = barInk };
    };
    auto const selection = detail::firstLegiblePair(SelectionFallbacks, pairOf);
    auto const barSelection = detail::firstMarkedPair(BarSelectionFallbacks, bar, pairOf);

    auto const toast = detail::firstLegiblePair(ToastFallbacks, [&](ToastSource source) {
        if (source == ToastSource::Inverse)
            return detail::InkPair { .fill = palette.defaultForeground, .text = palette.defaultBackground };
        return detail::InkPair { .fill = bar, .text = barInk };
    });

    auto colors = ChromeSchemeColors {};
    colors.bySlot[static_cast<size_t>(SchemeSlot::Background)] = palette.defaultBackground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Foreground)] = palette.defaultForeground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Bar)] = bar;
    colors.bySlot[static_cast<size_t>(SchemeSlot::BarInk)] = barInk;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Secondary)] = palette.brightColor(0);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Selection)] = selection.fill;
    colors.bySlot[static_cast<size_t>(SchemeSlot::SelectionText)] = selection.text;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Accent)] = palette.normalColor(4);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Danger)] = palette.normalColor(1);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Success)] = palette.normalColor(2);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Warning)] = palette.normalColor(3);
    colors.bySlot[static_cast<size_t>(SchemeSlot::BarSelection)] = barSelection.fill;
    colors.bySlot[static_cast<size_t>(SchemeSlot::BarSelectionText)] = barSelection.text;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Toast)] = toast.fill;
    colors.bySlot[static_cast<size_t>(SchemeSlot::ToastInk)] = toast.text;

    // The overlays drawn on the pane itself: marks that stand off its background, and the hint on the toast.
    auto const background = palette.defaultBackground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::PaneInk)] = vtbackend::legibleInkOn(palette, background);
    for (auto const& chain: SchemeMarkChains)
        colors.bySlot[static_cast<size_t>(chain.slot)] =
            detail::firstMarkOn(colors, chain.candidates, background);
    colors.bySlot[static_cast<size_t>(SchemeSlot::ToastHint)] = vtbackend::legibleInkOn(palette, toast.fill);
    return colors;
}

/// The colour a @c SchemeOnBar source inks with on the Terminal row's bar.
///
/// A hue that reads at 4.5:1 on @c Bar is itself; one that does not (blue on contour-light's dark bar)
/// gives way to @c BarInk, so the glyph it inks keeps its meaning by shape rather than vanishing.
/// @param colors The scheme's chrome slots.
/// @param hue    The slot to ink with.
/// @return The hue, or @c BarInk.
[[nodiscard]] inline vtbackend::RGBColor legibleOnBar(ChromeSchemeColors const& colors,
                                                      SchemeSlot hue) noexcept
{
    auto const color = colors.bySlot[static_cast<size_t>(hue)];
    auto const bar = colors.bySlot[static_cast<size_t>(SchemeSlot::Bar)];
    if (vtbackend::contrastRatio(color, bar) >= MinimumTextContrast)
        return color;
    return colors.bySlot[static_cast<size_t>(SchemeSlot::BarInk)];
}

/// The colour of @p slot drawn on @p ground: a pane whose profile's background is not the default profile's.
///
/// A mark slot (SchemeMarkChains) is chosen again from its chain against @p ground, so a light-profile pane
/// under a dark default keeps a thumb that stands off it at 3:1; the chain's last resort, the pane's ink,
/// reads as the scheme's (@c PaneInk) where it marks on @p ground, else as black or white, whichever stands
/// off it more. Any other slot is the scheme's own.
/// @param colors The scheme's chrome slots.
/// @param slot   The slot.
/// @param ground The pane's background.
/// @return The colour.
[[nodiscard]] inline vtbackend::RGBColor markOn(ChromeSchemeColors const& colors,
                                                SchemeSlot slot,
                                                vtbackend::RGBColor ground) noexcept
{
    auto const marks = [&](vtbackend::RGBColor color) {
        return vtbackend::contrastRatio(color, ground) >= MinimumMarkContrast;
    };
    auto const chain = std::ranges::find(SchemeMarkChains, slot, &SchemeMarkChain::slot);
    auto const own = colors.bySlot[static_cast<size_t>(slot)];
    if (chain == SchemeMarkChains.end() || marks(own))
        return own;
    for (auto const candidate: chain->candidates)
        if (auto const color = colors.bySlot[static_cast<size_t>(candidate)]; marks(color))
            return color;
    constexpr auto Black = vtbackend::RGBColor { 0x00, 0x00, 0x00 };
    constexpr auto White = vtbackend::RGBColor { 0xFF, 0xFF, 0xFF };
    return vtbackend::contrastRatio(Black, ground) >= vtbackend::contrastRatio(White, ground) ? Black : White;
}

/// @p color moved by @p points HSL lightness points (out of 100), keeping its hue and saturation.
///
/// Where the move would leave the 0-100 range -- darkening a black, lightening a white -- it goes the
/// other way instead, so the shade always stands off the colour it was taken from (the gap between two
/// cards on a black scheme is a lighter line, not an invisible one).
/// @param color  The colour to shade.
/// @param points The lightness points to move it by; negative darkens.
/// @return The shaded colour.
[[nodiscard]] constexpr vtbackend::RGBColor shadeLightness(vtbackend::RGBColor color, int points) noexcept
{
    auto const target = detail::hslOf(color).lightness + (points / 100.0);
    return shiftLightness(color, target < 0.0 || target > 1.0 ? -points : points);
}

} // namespace contour::config
