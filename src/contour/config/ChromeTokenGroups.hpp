// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
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
    Fill,        //!< The kit's @c fill.N wash.
    Label,       //!< The kit's @c label.N text.
    Surface,     //!< The kit's opaque @c surface.N: 1 elevated, 2 secondary.
    SchemeOnBar, //!< The source's scheme @c slot where it reads at 4.5:1 on @c Bar, else @c BarText.
};

/// The terminal colour-scheme slots the Terminal style paints its chrome with.
enum class SchemeSlot : uint8_t
{
    Background = 0, //!< The scheme's default background.
    Foreground,     //!< The scheme's default foreground.
    Bar,            //!< ANSI colour 0, the bar fill.
    BarText,        //!< Whichever of the default foreground and background reads better on @c Bar.
    Secondary,      //!< ANSI colour 8.
    /// The chrome's selection: the scheme's (@c schemeSelectionOf) where an end of the scheme reads on it
    /// at 4.5:1, else the first legible pair of @c SelectionFallbacks.
    Selection,
    SelectionText, //!< The text on @c Selection, which reaches 4.5:1 wherever any candidate pair does.
    Accent,        //!< ANSI colour 4.
    Danger,        //!< ANSI colour 1.
    Success,       //!< ANSI colour 2.
    Warning,       //!< ANSI colour 3.
};

/// Number of @c SchemeSlot enumerators, derived from the last one.
inline constexpr size_t SchemeSlotCount = static_cast<size_t>(SchemeSlot::Warning) + 1;

/// Number of @c ChromeColorRole enumerators, derived from the last one.
inline constexpr size_t ChromeColorRoleCount = static_cast<size_t>(ChromeColorRole::ChipTextStrong) + 1;

/// One chrome colour: a source kind plus the field that kind reads.
struct ChromeColorSource
{
    ColorSourceKind kind = ColorSourceKind::Fixed;    ///< Where the colour comes from.
    uint32_t rgb = 0;                                 ///< @c 0xRRGGBB, for @c Fixed.
    StatusColorRole status = StatusColorRole::Danger; ///< The hue, for @c Status.
    SchemeSlot slot = SchemeSlot::Background;         ///< The slot, for @c Scheme and @c SchemeOnBar.
    int alphaPercent = 100;                           ///< Opacity of the resulting colour.
    int level = 0; ///< The 1-based step of a @c Fill, @c Label or @c Surface source; 0 for other kinds.

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

    [[nodiscard]] bool operator==(SizeTokens const&) const = default;
};

/// Gaps and the spacing scale s1..s10.
struct SpaceTokens
{
    int paneGap = 0;              ///< Gap between split panes.
    int splitHit = 0;             ///< Hit target of a split divider.
    int paneInset = 0;            ///< Inset of a pane's content inside its card.
    std::array<int, 10> scale {}; ///< The spacing scale, s1 first.

    // The title band.
    int dragReserve = 0;        ///< Empty band that always stays draggable.
    int revealEdge = 0;         ///< Height of the full-screen edge that reveals the band.
    int toolbarChipPadding = 0; ///< Horizontal padding inside a toolbar chip.

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

    [[nodiscard]] bool operator==(LayoutTokens const&) const = default;
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
};

/// The WCAG AA ratio chrome text must reach against what it sits on.
inline constexpr double MinimumTextContrast = 4.5;

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
    constexpr SpaceTokens nativeSpace(int paneGap, int paneInset) noexcept
    {
        return SpaceTokens { .paneGap = paneGap,
                             .splitHit = 10,
                             .paneInset = paneInset,
                             .scale = { 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 },
                             .dragReserve = 40,
                             .revealEdge = 2,
                             .toolbarChipPadding = 9 };
    }

    inline constexpr MotionTokens NativeMotion { .fastMs = 100,
                                                 .standardMs = 160,
                                                 .emphasizedMs = 220,
                                                 .travel = 8,
                                                 .spinnerMs = 1200,
                                                 .pulseMs = 700,
                                                 .style = MotionStyle::Full };

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

    /// @param role The status hue.
    /// @return A @c Status source at full opacity.
    constexpr ChromeColorSource statusSource(StatusColorRole role) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Status,
                                   .rgb = 0,
                                   .status = role,
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
    };

    /// The native rows' status inks: the status hues themselves, indexed by @c StatusColorRole.
    inline constexpr std::array<ChromeColorSource, StatusColorRoleCount> NativeStatusInk {
        statusSource(StatusColorRole::Danger),
        statusSource(StatusColorRole::Warning),
        statusSource(StatusColorRole::Success),
        statusSource(StatusColorRole::Running),
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
                  .flyoutSwatch = 30 },
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
                   .toolbarChip = 11 },
        .type = { .caption = 10, .footnote = 11, .body = 13, .tab = 12, .title = 15 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Glass, .popover = Material::Glass },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Sheet,
                    .menu = MenuPresentation::Native,
                    .tabShape = TabShape::Pill,
                    .profileMenu = ProfileMenuAffordance::SecondaryClick,
                    .tabColors = TabColorSource::Roles,
                    .activeTabWeight = ActiveTabWeight::Regular },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
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
                  .flyoutSwatch = 30 },
        .space = nativeSpace(4, 4),
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
                   .toolbarChip = 11 },
        .type = { .caption = 12, .footnote = 12, .body = 14, .tab = 12, .title = 20 },
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
                    .activeTabWeight = ActiveTabWeight::Regular },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
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
                  .flyoutSwatch = 30 },
        .space = nativeSpace(6, 6),
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
                   .toolbarChip = 11 },
        .type = { .caption = 12, .footnote = 12.5, .body = 14, .tab = 13.5, .title = 15 },
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
                    .activeTabWeight = ActiveTabWeight::Regular },
        .tint = { .tabPercent = 20 },
        .colors = NativeColors,
        .statusInk = NativeStatusInk,
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
                  .flyoutSwatch = 2 },
        .space = { .paneGap = 1,
                   .splitHit = 1,
                   .paneInset = 0,
                   .scale = { 0, 0, 0, 1, 1, 1, 1, 1, 2, 3 },
                   .dragReserve = 4,
                   .revealEdge = 1,
                   .toolbarChipPadding = 1 },
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
                   .toolbarChip = 0 },
        .type = { .caption = 0, .footnote = 0, .body = 0, .tab = 0, .title = 0 },
        .motion = { .fastMs = 100,
                    .standardMs = 160,
                    .emphasizedMs = 220,
                    .travel = 0,
                    .spinnerMs = 1200,
                    .pulseMs = 700,
                    .style = MotionStyle::Full },
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
                    .activeTabWeight = ActiveTabWeight::Bold },
        // No tint: a tab's colour is its leading "▌" cell (UiStyleTokens::tabColorGlyph).
        .tint = { .tabPercent = 0 },
        .colors = { schemeColor(SchemeSlot::Bar),
                    schemeColor(SchemeSlot::BarText),
                    schemeColor(SchemeSlot::Warning),
                    schemeColor(SchemeSlot::Foreground),
                    schemeColor(SchemeSlot::Background),
                    schemeColor(SchemeSlot::Foreground),
                    schemeColor(SchemeSlot::Bar),           // TabWellFill
                    schemeColor(SchemeSlot::Selection),     // TabActiveFill
                    schemeColor(SchemeSlot::Bar),           // TabHoverFill
                    schemeColor(SchemeSlot::Bar),           // TabCloseFill
                    schemeColor(SchemeSlot::Selection),     // ControlPressedFill
                    schemeColor(SchemeSlot::BarText),       // TabText
                    schemeColor(SchemeSlot::SelectionText), // TabTextActive
                    // Not Secondary: ANSI 8 on a light scheme's dark bar fails 4.5:1, so it does not dim.
                    schemeColor(SchemeSlot::BarText),   // TabTextDimmed
                    schemeColor(SchemeSlot::BarText),   // TabTextTinted (the row has no tint)
                    schemeColor(SchemeSlot::Bar),       // ChipFill
                    schemeColor(SchemeSlot::BarText),   // ChipText
                    schemeColor(SchemeSlot::BarText) }, // ChipTextStrong
        // The scheme's hues, each falling back to the bar's text where it would not read on the bar.
        .statusInk = { schemeOnBar(SchemeSlot::Danger),
                       schemeOnBar(SchemeSlot::Warning),
                       schemeOnBar(SchemeSlot::Success),
                       schemeOnBar(SchemeSlot::Accent) },
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

/// Resolves a style's token row against the user's accessibility preferences.
/// @param row The chrome style whose row to take.
/// @param preferences The accessibility choices to apply on top of it.
/// @return The adjusted row.
[[nodiscard]] constexpr ChromeTokenRow resolveChromeTokens(ChromeStyle row,
                                                           AccessibilityPreferences preferences) noexcept
{
    auto tokens = ChromeTokenRows[static_cast<size_t>(row)];

    if (preferences.motion == MotionPreference::Reduced)
    {
        tokens.motion.fastMs = ReducedMotionCrossFadeMs;
        tokens.motion.standardMs = ReducedMotionCrossFadeMs;
        tokens.motion.emphasizedMs = ReducedMotionCrossFadeMs;
        tokens.motion.travel = 0;
        tokens.motion.style = MotionStyle::CrossFade;
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
    Bar,        //!< The bar and its text.
    BarBright,  //!< The bar with ANSI 15 (bright white) on it, for a scheme whose own ends are dim.
    BarMono,    //!< The bar with black or white on it, whichever reads better: always at least 4.58:1.
};

/// The candidates for the chrome's selection, first legible one wins. Most bundled schemes inherit a
/// saturated blue selection on which neither end of the scheme reaches 4.5:1 (one-dark, solarized-dark);
/// the last candidate always reads, so the active tab does on every scheme.
inline constexpr std::array SelectionFallbacks {
    SelectionSource::Scheme,    SelectionSource::Inverse, SelectionSource::Bar,
    SelectionSource::BarBright, SelectionSource::BarMono,
};

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

    auto colors = ChromeSchemeColors {};
    colors.bySlot[static_cast<size_t>(SchemeSlot::Background)] = palette.defaultBackground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Foreground)] = palette.defaultForeground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Bar)] = bar;
    colors.bySlot[static_cast<size_t>(SchemeSlot::BarText)] = readableOn(bar);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Secondary)] = palette.brightColor(0);
    // The first candidate whose text reads at 4.5:1; failing all, the one that reads best.
    struct Pair
    {
        vtbackend::RGBColor fill;
        vtbackend::RGBColor text;
    };
    auto const pairOf = [&](SelectionSource source) -> Pair {
        switch (source)
        {
            case SelectionSource::Scheme: {
                auto const own = schemeSelectionOf(palette);
                return { .fill = own, .text = readableOn(own) };
            }
            case SelectionSource::Inverse:
                return { .fill = palette.defaultForeground, .text = palette.defaultBackground };
            case SelectionSource::Bar: return { .fill = bar, .text = readableOn(bar) };
            case SelectionSource::BarBright: return { .fill = bar, .text = palette.brightColor(7) };
            case SelectionSource::BarMono: break;
        }
        auto const black = vtbackend::RGBColor { 0, 0, 0 };
        auto const white = vtbackend::RGBColor { 255, 255, 255 };
        return { .fill = bar,
                 .text = vtbackend::contrastRatio(black, bar) >= vtbackend::contrastRatio(white, bar)
                             ? black
                             : white };
    };
    auto const ratioOf = [](Pair const& pair) {
        return vtbackend::contrastRatio(pair.text, pair.fill);
    };
    auto selection = pairOf(SelectionFallbacks.front());
    for (auto const source: SelectionFallbacks)
    {
        auto const candidate = pairOf(source);
        if (ratioOf(candidate) >= MinimumTextContrast)
        {
            selection = candidate;
            break;
        }
        if (ratioOf(candidate) > ratioOf(selection))
            selection = candidate;
    }
    colors.bySlot[static_cast<size_t>(SchemeSlot::Selection)] = selection.fill;
    colors.bySlot[static_cast<size_t>(SchemeSlot::SelectionText)] = selection.text;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Accent)] = palette.normalColor(4);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Danger)] = palette.normalColor(1);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Success)] = palette.normalColor(2);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Warning)] = palette.normalColor(3);
    return colors;
}

/// The colour a scheme hue is inked in on the Terminal row's bar.
///
/// A hue that reads at 4.5:1 on @c Bar is itself; one that does not (blue on contour-light's dark bar)
/// gives way to @c BarText, so the glyph it inks keeps its meaning by shape rather than vanishing.
/// @param colors The scheme's chrome slots.
/// @param hue    The slot to ink with.
/// @return The hue, or @c BarText.
[[nodiscard]] inline vtbackend::RGBColor legibleOnBar(ChromeSchemeColors const& colors,
                                                      SchemeSlot hue) noexcept
{
    auto const color = colors.bySlot[static_cast<size_t>(hue)];
    auto const bar = colors.bySlot[static_cast<size_t>(SchemeSlot::Bar)];
    if (vtbackend::contrastRatio(color, bar) >= MinimumTextContrast)
        return color;
    return colors.bySlot[static_cast<size_t>(SchemeSlot::BarText)];
}

} // namespace contour::config
