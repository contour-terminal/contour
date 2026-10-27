// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
};

/// Number of @c TabShape enumerators, derived from the last one.
inline constexpr size_t TabShapeCount = static_cast<size_t>(TabShape::Pill) + 1;

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
};

/// Number of @c SchemeSlot enumerators, derived from the last one.
inline constexpr size_t SchemeSlotCount = static_cast<size_t>(SchemeSlot::BarSelectionText) + 1;

/// Number of @c ChromeColorRole enumerators, derived from the last one.
inline constexpr size_t ChromeColorRoleCount = static_cast<size_t>(ChromeColorRole::FindToggleOnRing) + 1;

/// One past the last pane colour role: the pane roles run from @c PaneGapFill to here.
inline constexpr size_t PaneColorRoleEnd = static_cast<size_t>(ChromeColorRole::PaneHeaderActionHover) + 1;

/// One past the last command palette colour role: the palette roles run from @c PaneColorRoleEnd to here,
/// so a family appended after them does not shift their count.
inline constexpr size_t PaletteColorRoleEnd = static_cast<size_t>(ChromeColorRole::PaletteSeparator) + 1;

/// One past the last find bar colour role: the find bar's roles run from @c PaletteColorRoleEnd to here.
inline constexpr size_t FindColorRoleEnd = static_cast<size_t>(ChromeColorRole::FindToggleOnRing) + 1;

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

    // The tab colour flyout.
    int flyoutSwatch = 0; ///< Edge of a swatch in the tab colour flyout (bigger than a menu's).

    // The panes.
    int bareBand = 0;         ///< Height of a band with neither tabs nor a title (Minimal, one tab).
    int grabber = 0;          ///< Thickness of the split handle's grabber; 0 draws none.
    int grabberLength = 0;    ///< Length of the split handle's grabber.
    int paneHeaderAction = 0; ///< Hit extent of a pane header's action button.
    int paneFooter = 0;       ///< Height of the exited footer.
    int footerButton = 0;     ///< Height of an exited footer's button.

    // The command palette.
    int paletteWidth = 0;       ///< Width of the palette (horizontal units).
    int paletteMaxHeight = 0;   ///< Height the palette grows to at most (vertical units).
    int paletteField = 0;       ///< Height of the query row (vertical units).
    int paletteRow = 0;         ///< Height of a row (vertical units).
    int paletteGroupHeader = 0; ///< Height of a group header (vertical units).
    int paletteFooter = 0;      ///< Height of the footer (vertical units).

    // The find bar.
    int findBar = 0;          ///< Height of the find bar.
    int findBarWidth = 0;     ///< Width of the find bar.
    int findToggleWidth = 0;  ///< Width of a find option toggle.
    int findToggleHeight = 0; ///< Height of a find option toggle.
    int findButtonWidth = 0;  ///< Width of the previous, next and close buttons.
    int findButtonHeight = 0; ///< Height of the previous, next and close buttons.
    int findIcon = 0;         ///< Edge of the magnifier; 0 draws none.

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

    // The panes.
    int paneGapRows =
        0; ///< @c paneGap between stacked panes, in vertical units (a cell's height on Terminal).
    int paneHeaderGap = 0; ///< Gap between a pane header's items.
    int footerLeading = 0; ///< Leading padding of the exited footer.

    int paletteInset = 0; ///< The palette rows' horizontal inset and the list's side padding.

    // The find bar.
    int findBarInset = 0;     ///< The find bar's inset from the pane's right edge.
    int findBarInsetRows = 0; ///< Its inset from the pane's top edge, in vertical units (a row on Terminal).

    [[nodiscard]] bool operator==(SpaceTokens const&) const = default;
};

/// Corner radii.
struct RadiusTokens
{
    int control = 0;  ///< Buttons and fields.
    int popover = 0;  ///< Popovers.
    int menu = 0;     ///< Menus.
    int paneCard = 0; ///< Pane cards.
    int toast = 0;    ///< Toasts.

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

    [[nodiscard]] bool operator==(LayoutTokens const&) const = default;
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

namespace detail
{
    /// @param paneGap       The gap between split panes, and their inset from the content edge.
    /// @param paneHeaderGap The gap between a pane header's items.
    /// @return A native row's spacing.
    constexpr SpaceTokens nativeSpace(int paneGap, int paneHeaderGap) noexcept
    {
        return SpaceTokens { .paneGap = paneGap,
                             .splitHit = 10,
                             .paneInset = paneGap,
                             .scale = { 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 },
                             .dragReserve = 40,
                             .revealEdge = 2,
                             .toolbarChipPadding = 9,
                             .paneGapRows = paneGap,
                             .paneHeaderGap = paneHeaderGap,
                             .footerLeading = 14,
                             .paletteInset = 8,
                             .findBarInset = 8,
                             .findBarInsetRows = 8 };
    }

    inline constexpr MotionTokens NativeMotion { .fastMs = 100,
                                                 .standardMs = 160,
                                                 .emphasizedMs = 220,
                                                 .travel = 8,
                                                 .spinnerMs = 1200,
                                                 .pulseMs = 700,
                                                 .style = MotionStyle::Full,
                                                 .headerRevealMs = 500,
                                                 .popoverScalePercent = 97 };

    inline constexpr ElevationTokens NativeElevation {
        .levels = { ElevationLevel { .offsetX = 0, .offsetY = 0, .blur = 0, .opacityPercent = 0 },
                    ElevationLevel { .offsetX = 0, .offsetY = 1, .blur = 2, .opacityPercent = 12 },
                    ElevationLevel { .offsetX = 0, .offsetY = 4, .blur = 16, .opacityPercent = 25 },
                    ElevationLevel { .offsetX = 0, .offsetY = 12, .blur = 40, .opacityPercent = 35 } }
    };

    inline constexpr ContrastTokens StandardContrast { .separatorPercent = 10,
                                                       .activeCardRing = 0.0,
                                                       .paneDimPercent = std::nullopt };

    /// @param slot The scheme slot.
    /// @return A @c Scheme source at full opacity.
    constexpr ChromeColorSource schemeColor(SchemeSlot slot) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Scheme,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param level The kit's @c fill step, 1 to 4.
    /// @return A @c Fill source.
    constexpr ChromeColorSource fill(int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Fill,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param level The kit's @c label step, 1 (primary) to 3 (tertiary).
    /// @return A @c Label source.
    constexpr ChromeColorSource label(int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Label,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param level The kit's surface: 1 elevated, 2 secondary.
    /// @return A @c Surface source.
    constexpr ChromeColorSource surface(int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Surface,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @param slot The scheme hue to ink with where it reads on the bar.
    /// @return A @c SchemeOnBar source.
    constexpr ChromeColorSource schemeOnBar(SchemeSlot slot) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::SchemeOnBar,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param slot  The scheme slot to shade.
    /// @param level The HSL lightness points to move it by; negative darkens.
    /// @return A @c SchemeShade source.
    constexpr ChromeColorSource schemeShade(SchemeSlot slot, int level) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::SchemeShade,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100,
                                   .level = level };
    }

    /// @return A source that paints nothing.
    constexpr ChromeColorSource noColor() noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Fixed,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 0,
                                   .level = 0 };
    }

    /// @return A @c Separator source: the kit's separator ink, by the ground's tone.
    constexpr ChromeColorSource separator() noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Separator,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param role The status hue.
    /// @return A @c StatusMark source at full opacity.
    constexpr ChromeColorSource statusMarkSource(StatusColorRole role) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::StatusMark,
                                   .rgb = 0,
                                   .status = role,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    /// @param kind A source kind that reads no field, e.g. @c PaletteHighlight.
    /// @return That kind at full opacity.
    constexpr ChromeColorSource sourceOf(ColorSourceKind kind) noexcept
    {
        return ChromeColorSource { .kind = kind,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = SchemeSlot::Background,
                                   .alphaPercent = 100,
                                   .level = 0 };
    }

    inline constexpr ChromeColorSource StatusWarning { .kind = ColorSourceKind::Status,
                                                       .rgb = 0,
                                                       .status = StatusColorRole::Warning,
                                                       .slot = SchemeSlot::Background,
                                                       .alphaPercent = 100,
                                                       .level = 0 };

    /// The native rows' colours: the 1a roles, then the kit's tables for the band (kit-components).
    inline constexpr std::array<ChromeColorSource, ChromeColorRoleCount> NativeColors {
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindow,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        StatusWarning,
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0x28282C,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 92,
                            .level = 0 },
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0xFFFFFF,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 },
        fill(4),    // TabWellFill
        surface(1), // TabActiveFill
        fill(3),    // TabHoverFill
        fill(3),    // TabCloseFill
        fill(2),    // ControlPressedFill
        label(2),   // TabText
        label(1),   // TabTextActive
        label(3),   // TabTextDimmed
        label(1),   // TabTextTinted
        fill(4),    // ChipFill
        label(2),   // ChipText
        label(1),   // ChipTextStrong
        // The gap is the chrome scheme's background 4 L points darker (mac-dark: #101216 under #1C1E24).
        schemeShade(SchemeSlot::Background, -4), // PaneGapFill
        separator(),                             // PaneCardBorder
        noColor(),                               // PaneHeaderFill: the card shows through.
        label(1),                                // PaneHeaderText
        label(2),                                // PaneHeaderTextDimmed
        separator(),                             // PaneSplitLine
        // The canvas's 28 % grabber and 8 % footer are not kit steps; the nearest ones are.
        fill(1),  // PaneGrabber
        fill(3),  // PaneFooterFill
        label(1), // PaneFooterText
        // The kit's strongest fill, by the pane's own tone: the band's tabHoverFill follows the chrome's.
        fill(1), // PaneHeaderActionHover
        // The palette is the kit's elevated surface; its selection is the OS's own selected-row pair
        // (white on the accent reads 4.0:1 where macOS's selected-content colour reads 5.3:1).
        surface(1),                                        // PaletteSurface
        label(1),                                          // PaletteText
        label(2),                                          // PaletteSecondaryText
        sourceOf(ColorSourceKind::PaletteHighlight),       // PaletteSelectionFill
        sourceOf(ColorSourceKind::PaletteHighlightedText), // PaletteSelectionText
        separator(),                                       // PaletteSeparator
        // A lit toggle is accent-tinted, its 22 % wash a mark (1c's text/mark split). Its glyph is the
        // primary label, not the accent: the accent is the user's, and a dark one on its own wash on a dark
        // chrome falls to 1.3:1.
        ChromeColorSource { .kind = ColorSourceKind::StatusMark,
                            .rgb = 0,
                            .status = StatusColorRole::Running,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 22,
                            .level = 0 }, // FindToggleOnFill
        label(1),                         // FindToggleOnText
        label(2),                         // FindToggleText
        label(2),                         // FindCountText
        // The wash alone can fall under 3:1 against an unlit toggle, so a lit one is also ringed: the accent,
        // or the chrome's ink where the accent would not reach 3:1 on the window (B-I4).
        ChromeColorSource { .kind = ColorSourceKind::StatusMarkOnWindow,
                            .rgb = 0,
                            .status = StatusColorRole::Running,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100,
                            .level = 0 }, // FindToggleOnRing
    };

    /// Every row's unfocused-pane dim (kit-tokens' @c pane.dim).
    inline constexpr DimTokens PaneDim { .darkPercent = 15, .lightPercent = 10 };

    /// The native rows' Minimal extents: no header, a hairline gap, a narrower hit area, no inset or radius.
    inline constexpr MinimalDensityTokens NativeMinimal {
        .paneHeader = 0, .paneGap = 1, .paneGapRows = 1, .splitHit = 8, .paneInset = 0, .paneCardRadius = 0
    };

    /// The native rows' status inks, indexed by @c StatusColorRole: the status hues' mark inks, since every
    /// ink read from here is a mark (a badge, the host chip's dot, a progress bar).
    inline constexpr std::array<ChromeColorSource, StatusColorRoleCount> NativeStatusInk {
        statusMarkSource(StatusColorRole::Danger),
        statusMarkSource(StatusColorRole::Warning),
        statusMarkSource(StatusColorRole::Success),
        statusMarkSource(StatusColorRole::Running),
    };

    inline constexpr ChromeTokenRow MacRow {
        .size = { .titleBar = 38,
                  .tabWell = 28,
                  .tab = 24,
                  .control = 28,
                  .paneHeader = 22,
                  .toast = 36,
                  .dialogWidth = 280,
                  .swatchWidth = 16,
                  .swatchHeight = 16,
                  .settingsWidth = 960,
                  .settingsHeight = 820,
                  .settingsMinWidth = 720,
                  .settingsMinHeight = 480,
                  .tabIcon = 14,
                  .tabClose = 16,
                  .badge = 14,
                  .badgeDot = 6,
                  .progressBar = 2,
                  .hostChip = 24,
                  .hostDot = 7,
                  .toolbarButton = 28,
                  .toolbarChip = 22,
                  .flyoutSwatch = 30,
                  .bareBand = 32,
                  .grabber = 2,
                  .grabberLength = 32,
                  .paneHeaderAction = 22,
                  .paneFooter = 34,
                  .footerButton = 24,
                  .paletteWidth = 640,
                  .paletteMaxHeight = 420,
                  .paletteField = 44,
                  .paletteRow = 32,
                  .paletteGroupHeader = 24,
                  .paletteFooter = 28,
                  .findBar = 32,
                  .findBarWidth = 380,
                  .findToggleWidth = 24,
                  .findToggleHeight = 22,
                  .findButtonWidth = 22,
                  .findButtonHeight = 22,
                  .findIcon = 14 },
        .space = nativeSpace(6, 6),
        .radii = { .control = 6,
                   .popover = 14,
                   .menu = 10,
                   .paneCard = 10,
                   .toast = 18,
                   .tab = 12,
                   .tabWell = 14,
                   .tabIcon = 3,
                   .tabClose = 8,
                   .badge = 7,
                   .hostChip = 12,
                   .toolbarChip = 11,
                   .grabber = 1,
                   .findBar = 8 },
        .type = { .caption = 10,
                  .footnote = 11,
                  .body = 13,
                  .tab = 12,
                  .title = 15,
                  .paneHeader = 11,
                  .paneHeaderWeight = 500,
                  .paneHeaderAction = 15,
                  .paletteInput = 17,
                  .shortcut = 12 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        // The band is Solid, not Glass (ruling S6): under AppKit's title-bar material the NSTitlebarView
        // takes the band's clicks, so the band is the client's paint over the compact toolbar. Glass behind
        // it returns with a within-window NSVisualEffectView of our own, behind the Qt content.
        .material = { .titleBand = Material::Solid, .popover = Material::Glass },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Sheet,
                    .menu = MenuPresentation::Native,
                    .tabShape = TabShape::Pill,
                    .profileMenu = ProfileMenuAffordance::SecondaryClick,
                    .tabColors = TabColorSource::Roles,
                    .activeTabWeight = ActiveTabWeight::Regular,
                    .paneActions = PaneActionsReveal::OnHover,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::Overlay },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
        .dim = PaneDim,
        .minimal = NativeMinimal,
    };

    inline constexpr ChromeTokenRow FluentRow {
        .size = { .titleBar = 40,
                  .tabWell = 0,
                  .tab = 34,
                  .control = 32,
                  .paneHeader = 28,
                  .toast = 36,
                  .dialogWidth = 280,
                  .swatchWidth = 16,
                  .swatchHeight = 16,
                  .settingsWidth = 1110,
                  .settingsHeight = 920,
                  .settingsMinWidth = 720,
                  .settingsMinHeight = 480,
                  .tabIcon = 14,
                  .tabClose = 16,
                  .badge = 14,
                  .badgeDot = 6,
                  .progressBar = 2,
                  .hostChip = 24,
                  .hostDot = 7,
                  .toolbarButton = 32,
                  .toolbarChip = 22,
                  .flyoutSwatch = 30,
                  .bareBand = 28,
                  .grabber = 2,
                  .grabberLength = 32,
                  .paneHeaderAction = 22,
                  .paneFooter = 34,
                  .footerButton = 24,
                  .paletteWidth = 640,
                  .paletteMaxHeight = 420,
                  .paletteField = 44,
                  .paletteRow = 36,
                  .paletteGroupHeader = 24,
                  .paletteFooter = 28,
                  .findBar = 32,
                  .findBarWidth = 380,
                  .findToggleWidth = 24,
                  .findToggleHeight = 22,
                  .findButtonWidth = 22,
                  .findButtonHeight = 22,
                  .findIcon = 14 },
        .space = nativeSpace(4, 8),
        .radii = { .control = 4,
                   .popover = 8,
                   .menu = 8,
                   .paneCard = 4,
                   .toast = 18,
                   .tab = 6,
                   .tabWell = 0,
                   .tabIcon = 3,
                   .tabClose = 8,
                   .badge = 7,
                   .hostChip = 12,
                   .toolbarChip = 11,
                   .grabber = 1,
                   .findBar = 8 },
        .type = { .caption = 12,
                  .footnote = 12,
                  .body = 14,
                  .tab = 12,
                  .title = 20,
                  .paneHeader = 12,
                  .paneHeaderWeight = 600,
                  .paneHeaderAction = 15,
                  .paletteInput = 16,
                  .shortcut = 12 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Mica, .popover = Material::Solid },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Centered,
                    .menu = MenuPresentation::InScene,
                    .tabShape = TabShape::Flat,
                    .profileMenu = ProfileMenuAffordance::Caret,
                    .tabColors = TabColorSource::Palette,
                    .activeTabWeight = ActiveTabWeight::Regular,
                    .paneActions = PaneActionsReveal::OnHover,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::Overlay },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
        .dim = PaneDim,
        .minimal = NativeMinimal,
    };

    inline constexpr ChromeTokenRow LinuxRow {
        .size = { .titleBar = 46,
                  .tabWell = 0,
                  .tab = 34,
                  .control = 34,
                  .paneHeader = 28,
                  .toast = 36,
                  .dialogWidth = 280,
                  .swatchWidth = 16,
                  .swatchHeight = 16,
                  .settingsWidth = 1100,
                  .settingsHeight = 920,
                  .settingsMinWidth = 720,
                  .settingsMinHeight = 480,
                  .tabIcon = 14,
                  .tabClose = 16,
                  .badge = 14,
                  .badgeDot = 6,
                  .progressBar = 2,
                  .hostChip = 24,
                  .hostDot = 7,
                  .toolbarButton = 34,
                  .toolbarChip = 22,
                  .flyoutSwatch = 30,
                  .bareBand = 28,
                  .grabber = 2,
                  .grabberLength = 32,
                  .paneHeaderAction = 22,
                  .paneFooter = 34,
                  .footerButton = 24,
                  .paletteWidth = 640,
                  .paletteMaxHeight = 420,
                  .paletteField = 44,
                  .paletteRow = 38,
                  .paletteGroupHeader = 24,
                  .paletteFooter = 28,
                  .findBar = 32,
                  .findBarWidth = 380,
                  .findToggleWidth = 24,
                  .findToggleHeight = 22,
                  .findButtonWidth = 22,
                  .findButtonHeight = 22,
                  .findIcon = 14 },
        .space = nativeSpace(6, 8),
        .radii = { .control = 8,
                   .popover = 12,
                   .menu = 10,
                   .paneCard = 6,
                   .toast = 18,
                   .tab = 8,
                   .tabWell = 0,
                   .tabIcon = 3,
                   .tabClose = 8,
                   .badge = 7,
                   .hostChip = 12,
                   .toolbarChip = 11,
                   .grabber = 1,
                   .findBar = 8 },
        .type = { .caption = 12,
                  .footnote = 12.5,
                  .body = 14,
                  .tab = 13.5,
                  .title = 15,
                  .paneHeader = 12.5,
                  .paneHeaderWeight = 600,
                  .paneHeaderAction = 15,
                  .paletteInput = 16,
                  .shortcut = 12.5 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Solid, .popover = Material::Solid },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Centered,
                    .menu = MenuPresentation::InScene,
                    .tabShape = TabShape::Flat,
                    .profileMenu = ProfileMenuAffordance::Caret,
                    .tabColors = TabColorSource::Palette,
                    .activeTabWeight = ActiveTabWeight::Regular,
                    .paneActions = PaneActionsReveal::OnHover,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::Overlay },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
        .dim = PaneDim,
        .minimal = NativeMinimal,
    };

    inline constexpr ElevationLevel TerminalShadow {
        .offsetX = 1, .offsetY = 1, .blur = 0, .opacityPercent = 60
    };

    inline constexpr ChromeTokenRow TerminalRow {
        .size = { .titleBar = 1,
                  .tabWell = 1,
                  .tab = 1,
                  .control = 2,
                  .paneHeader = 1,
                  .toast = 1,
                  .dialogWidth = 32,
                  .swatchWidth = 2,
                  .swatchHeight = 1,
                  .settingsWidth = 120,
                  .settingsHeight = 40,
                  .settingsMinWidth = 90,
                  .settingsMinHeight = 24,
                  .tabIcon = 1,
                  .tabClose = 1,
                  .badge = 1,
                  .badgeDot = 1,
                  .progressBar = 1,
                  .hostChip = 1,
                  .hostDot = 1,
                  .toolbarButton = 2,
                  .toolbarChip = 1,
                  .flyoutSwatch = 2,
                  .bareBand = 1,
                  .grabber = 0,
                  .grabberLength = 0,
                  .paneHeaderAction = 1,
                  .paneFooter = 1,
                  .footerButton = 1,
                  .paletteWidth = 48,
                  .paletteMaxHeight = 20,
                  .paletteField = 1,
                  .paletteRow = 1,
                  .paletteGroupHeader = 1,
                  .paletteFooter = 1,
                  .findBar = 1,
                  .findBarWidth = 48,
                  .findToggleWidth = 4,
                  .findToggleHeight = 1,
                  .findButtonWidth = 2,
                  .findButtonHeight = 1,
                  .findIcon = 0 },
        .space = { .paneGap = 1,
                   .splitHit = 1,
                   .paneInset = 0,
                   .scale = { 0, 0, 0, 1, 1, 1, 1, 1, 2, 3 },
                   .dragReserve = 4,
                   .revealEdge = 1,
                   .toolbarChipPadding = 1,
                   .paneGapRows = 1,
                   .paneHeaderGap = 1,
                   .footerLeading = 1,
                   .paletteInset = 1,
                   .findBarInset = 1,
                   .findBarInsetRows = 1 },
        .radii = { .control = 0,
                   .popover = 0,
                   .menu = 0,
                   .paneCard = 0,
                   .toast = 0,
                   .tab = 0,
                   .tabWell = 0,
                   .tabIcon = 0,
                   .tabClose = 0,
                   .badge = 0,
                   .hostChip = 0,
                   .toolbarChip = 0,
                   .grabber = 0,
                   .findBar = 0 },
        .type = { .caption = 0,
                  .footnote = 0,
                  .body = 0,
                  .tab = 0,
                  .title = 0,
                  .paneHeader = 0,
                  .paneHeaderWeight = 400,
                  .paneHeaderAction = 0,
                  .paletteInput = 0,
                  .shortcut = 0 },
        .motion = { .fastMs = 100,
                    .standardMs = 160,
                    .emphasizedMs = 220,
                    .travel = 0,
                    .spinnerMs = 1200,
                    .pulseMs = 700,
                    .style = MotionStyle::Full,
                    .headerRevealMs = 500,
                    .popoverScalePercent = 100 },
        .elevation = { .levels = { ElevationLevel {
                                       .offsetX = 0, .offsetY = 0, .blur = 0, .opacityPercent = 0 },
                                   TerminalShadow,
                                   TerminalShadow,
                                   TerminalShadow } },
        .material = { .titleBand = Material::None, .popover = Material::None },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 0.62, .toastStyle = ToastStyle::Inverse },
        .layout = { .dialog = DialogPlacement::Centered,
                    .menu = MenuPresentation::InScene,
                    .tabShape = TabShape::Flat,
                    .profileMenu = ProfileMenuAffordance::Caret,
                    .tabColors = TabColorSource::Roles,
                    .activeTabWeight = ActiveTabWeight::Bold,
                    .paneActions = PaneActionsReveal::OnHoverOrFocus,
                    .paletteTopPercent = 18,
                    .findBar = FindBarHost::PaneHeaderRow },
        // No tint: a tab's colour is its leading "▌" cell (UiStyleTokens::tabColorGlyph).
        .tint = { .tabPercent = 0 },
        .colors = { schemeColor(SchemeSlot::Bar),
                    schemeColor(SchemeSlot::BarInk),
                    schemeColor(SchemeSlot::Warning),
                    schemeColor(SchemeSlot::Foreground),
                    schemeColor(SchemeSlot::Background),
                    schemeColor(SchemeSlot::Foreground),
                    schemeColor(SchemeSlot::Bar),           // TabWellFill
                    schemeColor(SchemeSlot::Selection),     // TabActiveFill
                    schemeColor(SchemeSlot::Bar),           // TabHoverFill
                    schemeColor(SchemeSlot::Bar),           // TabCloseFill
                    schemeColor(SchemeSlot::Selection),     // ControlPressedFill
                    schemeColor(SchemeSlot::BarInk),        // TabText
                    schemeColor(SchemeSlot::SelectionText), // TabTextActive
                    // Not Secondary: ANSI 8 on a light scheme's dark bar fails 4.5:1, so it does not dim.
                    schemeColor(SchemeSlot::BarInk), // TabTextDimmed
                    schemeColor(SchemeSlot::BarInk), // TabTextTinted (the row has no tint)
                    schemeColor(SchemeSlot::Bar),    // ChipFill
                    schemeColor(SchemeSlot::BarInk), // ChipText
                    schemeColor(SchemeSlot::BarInk), // ChipTextStrong
                    // A pane is its own cells: the header is a bar row, the split a line in ANSI 8.
                    schemeColor(SchemeSlot::Background), // PaneGapFill
                    schemeColor(SchemeSlot::Background), // PaneCardBorder
                    schemeColor(SchemeSlot::Bar),        // PaneHeaderFill
                    // On the bar: its one ink, and the dimmed text ANSI 8 where that reads there (else the
                    // ink).
                    schemeColor(SchemeSlot::BarInk),    // PaneHeaderText
                    schemeOnBar(SchemeSlot::Secondary), // PaneHeaderTextDimmed
                    schemeColor(SchemeSlot::Secondary), // PaneSplitLine
                    schemeColor(SchemeSlot::Accent),    // PaneGrabber
                    schemeColor(SchemeSlot::Bar),       // PaneFooterFill
                    schemeColor(SchemeSlot::BarInk),    // PaneFooterText
                    // A cell row has no hover: the button is its glyph.
                    detail::noColor(), // PaneHeaderActionHover
                    // The palette is a bar-coloured block: the bar's ink, ANSI 8 where it reads there.
                    schemeColor(SchemeSlot::Bar),       // PaletteSurface
                    schemeColor(SchemeSlot::BarInk),    // PaletteText
                    schemeOnBar(SchemeSlot::Secondary), // PaletteSecondaryText
                    // A selected row on the bar-coloured surface must also stand off it (BarSelection).
                    schemeColor(SchemeSlot::BarSelection),     // PaletteSelectionFill
                    schemeColor(SchemeSlot::BarSelectionText), // PaletteSelectionText
                    schemeColor(SchemeSlot::Secondary),        // PaletteSeparator
                    // A lit toggle is its cells in inverse video -- the bar's ink as the fill, the bar as the
                    // glyph -- which stands off an unlit one on the bar at the ink's own 4.5:1; the scheme's
                    // selection on the bar can fall to 1.9:1. An unlit one and the count, the bar's inks.
                    schemeColor(SchemeSlot::BarInk),    // FindToggleOnFill
                    schemeColor(SchemeSlot::Bar),       // FindToggleOnText
                    schemeOnBar(SchemeSlot::Secondary), // FindToggleText
                    schemeColor(SchemeSlot::BarInk),    // FindCountText
                    // Inverse video already stands off the bar at 4.5:1: no ring.
                    detail::noColor() }, // FindToggleOnRing
        // The scheme's hues, each falling back to the bar's ink where it would not read on the bar.
        .statusInk = { schemeOnBar(SchemeSlot::Danger),
                       schemeOnBar(SchemeSlot::Warning),
                       schemeOnBar(SchemeSlot::Success),
                       schemeOnBar(SchemeSlot::Accent) },
        .dim = PaneDim,
        .minimal = { .paneHeader = 0,
                     .paneGap = 1,
                     .paneGapRows = 1,
                     .splitHit = 1,
                     .paneInset = 0,
                     .paneCardRadius = 0 },
    };
} // namespace detail

/// The 1a token rows, indexed by @c ChromeStyle.
inline constexpr std::array<ChromeTokenRow, detail::ChromeStyleCount> ChromeTokenRows {
    detail::MacRow,      // ChromeStyle::MacOS
    detail::FluentRow,   // ChromeStyle::Fluent
    detail::LinuxRow,    // ChromeStyle::ContourLinux
    detail::TerminalRow, // ChromeStyle::Terminal
};

static_assert(ChromeTokenRows.size() == detail::ChromeStyleCount,
              "Every ChromeStyle needs exactly one 1a token row.");

/// Resolves a style's token row against the chrome density and the user's accessibility preferences.
/// @param row The chrome style whose row to take.
/// @param preferences The accessibility choices to apply on top of it.
/// @param density The density whose extents to take: @c Minimal copies the row's @c minimal sub-row over
///                the pane extents, before the accessibility steps.
/// @return The adjusted row.
[[nodiscard]] constexpr ChromeTokenRow resolveChromeTokens(
    ChromeStyle row,
    AccessibilityPreferences preferences,
    ChromeDensity density = ChromeDensity::Comfortable) noexcept
{
    auto tokens = ChromeTokenRows[static_cast<size_t>(row)];

    if (density == ChromeDensity::Minimal)
    {
        tokens.size.paneHeader = tokens.minimal.paneHeader;
        tokens.space.paneGap = tokens.minimal.paneGap;
        tokens.space.paneGapRows = tokens.minimal.paneGapRows;
        tokens.space.splitHit = tokens.minimal.splitHit;
        tokens.space.paneInset = tokens.minimal.paneInset;
        tokens.radii.paneCard = tokens.minimal.paneCardRadius;
    }

    if (preferences.motion == MotionPreference::Reduced)
    {
        tokens.motion.fastMs = ReducedMotionCrossFadeMs;
        tokens.motion.standardMs = ReducedMotionCrossFadeMs;
        tokens.motion.emphasizedMs = ReducedMotionCrossFadeMs;
        tokens.motion.travel = 0;
        tokens.motion.style = MotionStyle::CrossFade;
        tokens.motion.popoverScalePercent = 100;
    }

    if (preferences.transparency == TransparencyPreference::Reduced)
    {
        for (auto* material: { &tokens.material.titleBand, &tokens.material.popover })
            if (*material == Material::Glass || *material == Material::Mica)
                *material = Material::Solid;
    }

    if (preferences.contrast == ContrastPreference::Increased)
    {
        tokens.contrast.separatorPercent *= 2;
        tokens.contrast.activeCardRing = 1.5;
        tokens.contrast.paneDimPercent = 5;
    }

    return tokens;
}

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
