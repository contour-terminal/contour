// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

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

/// The chrome colours a row describes; enumerator order is the order of @c ChromeTokenRow::colors.
enum class ChromeColorRole : uint8_t
{
    BannerFill = 0, //!< Background of a banner.
    BannerText,     //!< Text of a banner.
    BannerGlyph,    //!< Leading glyph of a banner.
    ToastFill,      //!< Background of a toast.
    ToastText,      //!< Text of a toast.
    SwatchRing,     //!< Ring around the checked colour swatch of a menu.
};

/// Where a chrome colour comes from.
enum class ColorSourceKind : uint8_t
{
    Fixed = 0,         //!< The source's @c rgb.
    PaletteWindow,     //!< The OS palette's window colour.
    PaletteWindowText, //!< The OS palette's window text colour.
    Status,            //!< A status hue, picked by the source's @c status.
    Scheme,            //!< A slot of the terminal's colour scheme, picked by the source's @c slot.
};

/// The terminal colour-scheme slots the Terminal style paints its chrome with.
enum class SchemeSlot : uint8_t
{
    Background = 0, //!< The scheme's default background.
    Foreground,     //!< The scheme's default foreground.
    Bar,            //!< ANSI colour 0, the bar fill.
    BarText,        //!< Whichever of the default foreground and background reads better on @c Bar.
    Secondary,      //!< ANSI colour 8.
    Selection,      //!< The selection background.
    Accent,         //!< ANSI colour 4.
    Danger,         //!< ANSI colour 1.
    Success,        //!< ANSI colour 2.
    Warning,        //!< ANSI colour 3.
};

/// Number of @c SchemeSlot enumerators, derived from the last one.
inline constexpr size_t SchemeSlotCount = static_cast<size_t>(SchemeSlot::Warning) + 1;

/// Number of @c ChromeColorRole enumerators, derived from the last one.
inline constexpr size_t ChromeColorRoleCount = static_cast<size_t>(ChromeColorRole::SwatchRing) + 1;

/// One chrome colour: a source kind plus the field that kind reads.
struct ChromeColorSource
{
    ColorSourceKind kind = ColorSourceKind::Fixed;    ///< Where the colour comes from.
    uint32_t rgb = 0;                                 ///< @c 0xRRGGBB, for @c Fixed.
    StatusColorRole status = StatusColorRole::Danger; ///< The hue, for @c Status.
    SchemeSlot slot = SchemeSlot::Background;         ///< The slot, for @c Scheme.
    int alphaPercent = 100;                           ///< Opacity of the resulting colour.

    [[nodiscard]] bool operator==(ChromeColorSource const&) const = default;
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

    [[nodiscard]] bool operator==(SizeTokens const&) const = default;
};

/// Gaps and the spacing scale s1..s10.
struct SpaceTokens
{
    int paneGap = 0;              ///< Gap between split panes.
    int splitHit = 0;             ///< Hit target of a split divider.
    int paneInset = 0;            ///< Inset of a pane's content inside its card.
    std::array<int, 10> scale {}; ///< The spacing scale, s1 first.

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

/// Layout choices that differ per style.
struct LayoutTokens
{
    DialogPlacement dialog = DialogPlacement::Centered; ///< Where dialogs appear.
    MenuPresentation menu = MenuPresentation::InScene;  ///< How menus are presented; read by C++ only.

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
    std::array<ChromeColorSource, ChromeColorRoleCount>
        colors {}; ///< Colour sources, indexed by @c ChromeColorRole.
};

/// Duration every transition collapses to under @c MotionPreference::Reduced.
inline constexpr int ReducedMotionCrossFadeMs = 100;

namespace detail
{
    constexpr SpaceTokens nativeSpace(int paneGap, int paneInset) noexcept
    {
        return SpaceTokens { .paneGap = paneGap,
                             .splitHit = 10,
                             .paneInset = paneInset,
                             .scale = { 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 } };
    }

    inline constexpr MotionTokens NativeMotion {
        .fastMs = 100, .standardMs = 160, .emphasizedMs = 220, .travel = 8
    };

    inline constexpr ElevationTokens NativeElevation {
        .levels = { ElevationLevel { .offsetX = 0, .offsetY = 0, .blur = 0, .opacityPercent = 0 },
                    ElevationLevel { .offsetX = 0, .offsetY = 1, .blur = 2, .opacityPercent = 12 },
                    ElevationLevel { .offsetX = 0, .offsetY = 4, .blur = 16, .opacityPercent = 25 },
                    ElevationLevel { .offsetX = 0, .offsetY = 12, .blur = 40, .opacityPercent = 35 } }
    };

    inline constexpr ContrastTokens StandardContrast { .separatorPercent = 10,
                                                       .activeCardRing = 0.0,
                                                       .paneDimPercent = std::nullopt };

    inline constexpr ChromeColorSource StatusWarning { .kind = ColorSourceKind::Status,
                                                       .rgb = 0,
                                                       .status = StatusColorRole::Warning,
                                                       .slot = SchemeSlot::Background,
                                                       .alphaPercent = 100 };

    inline constexpr std::array<ChromeColorSource, ChromeColorRoleCount> NativeColors {
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindow,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100 },
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100 },
        StatusWarning,
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0x28282C,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 92 },
        ChromeColorSource { .kind = ColorSourceKind::Fixed,
                            .rgb = 0xFFFFFF,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100 },
        ChromeColorSource { .kind = ColorSourceKind::PaletteWindowText,
                            .rgb = 0,
                            .status = StatusColorRole::Danger,
                            .slot = SchemeSlot::Background,
                            .alphaPercent = 100 },
    };

    constexpr ChromeColorSource schemeColor(SchemeSlot slot) noexcept
    {
        return ChromeColorSource { .kind = ColorSourceKind::Scheme,
                                   .rgb = 0,
                                   .status = StatusColorRole::Danger,
                                   .slot = slot,
                                   .alphaPercent = 100 };
    }

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
                  .settingsMinHeight = 480 },
        .space = nativeSpace(6, 6),
        .radii = { .control = 6, .popover = 14, .menu = 10, .paneCard = 10, .toast = 18 },
        .type = { .caption = 10, .footnote = 11, .body = 13, .tab = 12, .title = 15 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Glass, .popover = Material::Glass },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Sheet, .menu = MenuPresentation::Native },
        .colors = NativeColors,
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
                  .settingsMinHeight = 480 },
        .space = nativeSpace(4, 4),
        .radii = { .control = 4, .popover = 8, .menu = 8, .paneCard = 4, .toast = 18 },
        .type = { .caption = 12, .footnote = 12, .body = 14, .tab = 12, .title = 20 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Mica, .popover = Material::Solid },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Centered, .menu = MenuPresentation::InScene },
        .colors = NativeColors,
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
                  .settingsMinHeight = 480 },
        .space = nativeSpace(6, 6),
        .radii = { .control = 8, .popover = 12, .menu = 10, .paneCard = 6, .toast = 18 },
        .type = { .caption = 12, .footnote = 12.5, .body = 14, .tab = 13.5, .title = 15 },
        .motion = NativeMotion,
        .elevation = NativeElevation,
        .material = { .titleBand = Material::Solid, .popover = Material::Solid },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 1.0, .toastStyle = ToastStyle::Floating },
        .layout = { .dialog = DialogPlacement::Centered, .menu = MenuPresentation::InScene },
        .colors = NativeColors,
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
                  .settingsMinHeight = 24 },
        .space = { .paneGap = 1, .splitHit = 1, .paneInset = 0, .scale = { 0, 0, 0, 1, 1, 1, 1, 1, 2, 3 } },
        .radii = { .control = 0, .popover = 0, .menu = 0, .paneCard = 0, .toast = 0 },
        .type = { .caption = 0, .footnote = 0, .body = 0, .tab = 0, .title = 0 },
        .motion = { .fastMs = 100, .standardMs = 160, .emphasizedMs = 220, .travel = 0 },
        .elevation = { .levels = { ElevationLevel {
                                       .offsetX = 0, .offsetY = 0, .blur = 0, .opacityPercent = 0 },
                                   TerminalShadow,
                                   TerminalShadow,
                                   TerminalShadow } },
        .material = { .titleBand = Material::None, .popover = Material::None },
        .contrast = StandardContrast,
        .tui = { .inactivePaneOpacity = 0.62, .toastStyle = ToastStyle::Inverse },
        .layout = { .dialog = DialogPlacement::Centered, .menu = MenuPresentation::InScene },
        .colors = { schemeColor(SchemeSlot::Bar),
                    schemeColor(SchemeSlot::BarText),
                    schemeColor(SchemeSlot::Warning),
                    schemeColor(SchemeSlot::Foreground),
                    schemeColor(SchemeSlot::Background),
                    schemeColor(SchemeSlot::Foreground) },
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

/// Reduces a terminal colour palette to the slots the Terminal chrome paints with.
/// @param palette The active terminal colour palette.
/// @return One colour per @c SchemeSlot.
[[nodiscard]] inline ChromeSchemeColors chromeSchemeColorsOf(vtbackend::ColorPalette const& palette)
{
    auto const selection = [&]() -> vtbackend::RGBColor {
        if (auto const* rgb = std::get_if<vtbackend::RGBColor>(&palette.selection.background))
            return *rgb;
        if (std::holds_alternative<vtbackend::CellForegroundColor>(palette.selection.background))
            return palette.defaultForeground;
        return palette.defaultBackground;
    }();

    // ANSI colour 0 is black in a light scheme as in a dark one, but a light scheme's foreground is dark
    // too: text on the bar takes whichever end of the scheme stands off it.
    auto const bar = palette.normalColor(0);
    auto const barText = vtbackend::contrastRatio(palette.defaultForeground, bar)
                                 >= vtbackend::contrastRatio(palette.defaultBackground, bar)
                             ? palette.defaultForeground
                             : palette.defaultBackground;

    auto colors = ChromeSchemeColors {};
    colors.bySlot[static_cast<size_t>(SchemeSlot::Background)] = palette.defaultBackground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Foreground)] = palette.defaultForeground;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Bar)] = bar;
    colors.bySlot[static_cast<size_t>(SchemeSlot::BarText)] = barText;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Secondary)] = palette.brightColor(0);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Selection)] = selection;
    colors.bySlot[static_cast<size_t>(SchemeSlot::Accent)] = palette.normalColor(4);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Danger)] = palette.normalColor(1);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Success)] = palette.normalColor(2);
    colors.bySlot[static_cast<size_t>(SchemeSlot::Warning)] = palette.normalColor(3);
    return colors;
}

} // namespace contour::config
