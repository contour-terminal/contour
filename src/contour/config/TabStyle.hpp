// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The `tab_style:` section: what the user may change about how the tabs are drawn, on top of the chrome
/// row's own look. Pure and Qt-free: the spellings, the parsers and the step that lays the section over a
/// row's defaults all live here, so the loader, the writer, the Settings window and the tests read one table.

#include <contour/config/ConfigEnumInfo.hpp>
#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace contour::config
{

/// The line that marks the active tab, in the accent colour.
enum class TabIndicator : uint8_t
{
    None = 0,  //!< No line: the active tab is told apart by its fill alone.
    Underline, //!< A line along the active tab's bottom edge.
    TopLine,   //!< A line along the active tab's top edge.
};

namespace detail
{
    /// The indicator spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto TabIndicatorTable = std::array {
        ConfigEnumInfo<TabIndicator> { TabIndicator::None, "none", "None" },
        ConfigEnumInfo<TabIndicator> { TabIndicator::Underline, "underline", "Underline" },
        ConfigEnumInfo<TabIndicator> { TabIndicator::TopLine, "top-line", "Top line" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabIndicator> const> configEnumValues() noexcept
{
    return detail::TabIndicatorTable;
}

/// How a tab shows a colour of its own (a profile's `tab_color`, or one picked for the tab).
///
/// Zero is `Fill`, what a coloured tab has always looked like.
enum class TabColorMode : uint8_t
{
    Fill = 0, //!< The colour is the tab's fill, faded while the tab is inactive; it replaces the styled fill.
    Tint,     //!< A wash of the colour over the tab as it is otherwise styled.
    Indicator, //!< The tab keeps its styled fill, and the colour is its indicator line.
};

namespace detail
{
    /// The colour-mode spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto TabColorModeTable = std::array {
        ConfigEnumInfo<TabColorMode> { TabColorMode::Fill, "fill", "Fill" },
        ConfigEnumInfo<TabColorMode> { TabColorMode::Tint, "tint", "Tint" },
        ConfigEnumInfo<TabColorMode> { TabColorMode::Indicator, "indicator", "Indicator line" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabColorMode> const> configEnumValues() noexcept
{
    return detail::TabColorModeTable;
}

/// Where a tab's fill comes from.
enum class TabFillKind : uint8_t
{
    Auto = 0, //!< The chrome row's own fill for that tab.
    Accent,   //!< The accent colour.
    Subtle,   //!< The row's hover wash: a faint fill that keeps the bar's tone.
    Custom,   //!< The colour the user wrote.
};

/// Number of @c TabFillKind enumerators, derived from the last one.
inline constexpr size_t TabFillKindCount = static_cast<size_t>(TabFillKind::Custom) + 1;

/// One fill keyword: a kind that is written as a word rather than as a colour.
struct TabFillKeyword
{
    TabFillKind kind {};       ///< The kind.
    std::string_view token {}; ///< Its configuration spelling.
};

/// The fill keywords, in enumerator order. @c Custom has no row: it is written as its colour.
inline constexpr auto TabFillKeywords = std::array {
    TabFillKeyword { .kind = TabFillKind::Auto, .token = "auto" },
    TabFillKeyword { .kind = TabFillKind::Accent, .token = "accent" },
    TabFillKeyword { .kind = TabFillKind::Subtle, .token = "subtle" },
};

/// A tab's fill: a keyword, or a colour of the user's.
struct TabFill
{
    TabFillKind kind = TabFillKind::Auto; ///< Where the fill comes from.
    vtbackend::RGBColor color {};         ///< The colour, read only for @c TabFillKind::Custom.

    [[nodiscard]] bool operator==(TabFill const&) const = default;
};

/// Reads a fill as contour.yml spells it.
/// @param text One of the @c TabFillKeywords tokens (ignoring case), or a colour as `#RRGGBB`.
/// @return The fill, or @c std::nullopt when @p text is neither.
[[nodiscard]] inline std::optional<TabFill> parseTabFill(std::string_view text)
{
    auto const keyword = std::ranges::find_if(TabFillKeywords, [text](TabFillKeyword const& row) {
        return detail::equalsIgnoreCase(row.token, text);
    });
    if (keyword != TabFillKeywords.end())
        return TabFill { .kind = keyword->kind, .color = {} };
    // Seven characters exactly: parseColor() also takes the X11 forms, which a YAML colour is never
    // written in.
    if (text.size() != 7 || text.front() != '#')
        return std::nullopt;
    if (auto const color = vtbackend::parseColor(text))
        return TabFill { .kind = TabFillKind::Custom, .color = *color };
    return std::nullopt;
}

/// Writes a fill as parseTabFill() reads it.
/// @param fill The fill.
/// @return Its keyword, or its colour as `#RRGGBB`.
[[nodiscard]] inline std::string tabFillText(TabFill const& fill)
{
    auto const keyword = std::ranges::find(TabFillKeywords, fill.kind, &TabFillKeyword::kind);
    if (keyword != TabFillKeywords.end())
        return std::string { keyword->token };
    return vtbackend::formatColor(fill.color);
}

/// The indicator line's colour when the section names none: the accent.
inline constexpr auto DefaultIndicatorColor = TabFill { .kind = TabFillKind::Accent, .color = {} };

/// Reads `tab_style.indicator_color`: a fill's grammar, without the two keywords that mean "no colour of
/// its own" -- a line has no row colour to keep and no wash to be.
/// @param text `accent` (ignoring case), or a colour as `#RRGGBB`.
/// @return The colour, or @c std::nullopt when @p text is neither.
[[nodiscard]] inline std::optional<TabFill> parseIndicatorColor(std::string_view text)
{
    auto const color = parseTabFill(text);
    if (!color || color->kind == TabFillKind::Auto || color->kind == TabFillKind::Subtle)
        return std::nullopt;
    return color;
}

/// How each tab is drawn inside the strip the chrome row lays out (`tab_style.shape`).
///
/// A look, not a layout: the row still decides where the strip sits and how tall it is, so a look is
/// available on every native row. Zero is `Auto`, the row's own tab.
enum class TabLook : uint8_t
{
    Auto = 0,  //!< The row's own tab: joined to the content on Windows, a pill on macOS and Linux.
    Pill,      //!< A rounded tab that floats on the bar, the active one filled.
    Flat,      //!< A square tab, edge to edge, the active one filled.
    Underline, //!< No fill at all: the active tab is the one with the line under it.
    Outlined,  //!< A rounded tab that floats on the bar, the active one drawn as an outline.
};

/// Number of @c TabLook enumerators, derived from the last one.
inline constexpr size_t TabLookCount = static_cast<size_t>(TabLook::Outlined) + 1;

namespace detail
{
    /// The look spellings. The tokens are written into the configuration, so once shipped they are fixed.
    inline constexpr auto TabLookTable = std::array {
        ConfigEnumInfo<TabLook> { TabLook::Auto, "auto", "Automatic" },
        ConfigEnumInfo<TabLook> { TabLook::Pill, "pill", "Pill" },
        ConfigEnumInfo<TabLook> { TabLook::Flat, "flat", "Flat" },
        ConfigEnumInfo<TabLook> { TabLook::Underline, "underline", "Underline" },
        ConfigEnumInfo<TabLook> { TabLook::Outlined, "outlined", "Outlined" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabLook> const> configEnumValues() noexcept
{
    return detail::TabLookTable;
}

/// What the tab's corners are.
enum class TabCorners : uint8_t
{
    Row = 0, //!< The row's own: its radius, squared off where the row joins the tab to the content.
    Round,   //!< The row's radius on all four corners.
    Square,  //!< No radius.
};

/// How the tab sits in the strip.
enum class TabSeat : uint8_t
{
    Row = 0,  //!< As the row seats it.
    Floating, //!< Lifted off the strip's floor, so the bar shows under it.
};

/// What sets the active tab apart.
enum class TabActiveMark : uint8_t
{
    Row = 0,   //!< The row's own fill for it.
    Wash,      //!< The row's active-tab fill over the bar.
    Outline,   //!< A hairline around it, and no fill.
    Underline, //!< The indicator line under it, and no fill.
};

/// What the bar's foot is, where it meets the content.
enum class TabBarEdge : uint8_t
{
    Row = 0, //!< As the row draws it: nothing, the active tab or the bar's fill says where the bar ends.
    Ruled,   //!< A hairline along the foot: no tab joins the content, so the line marks the edge.
};

/// What one @c TabLook draws: the tab reads these and never the look.
struct TabLookTraits
{
    TabCorners corners = TabCorners::Row;      ///< The tab's corners.
    TabSeat seat = TabSeat::Row;               ///< How it sits in the strip.
    TabActiveMark active = TabActiveMark::Row; ///< What sets the active tab apart.
    TabBarEdge edge = TabBarEdge::Row;         ///< What the bar's foot is.

    [[nodiscard]] bool operator==(TabLookTraits const&) const = default;
};

/// The looks' traits, indexed by @c TabLook; a new look adds its row here.
inline constexpr std::array<TabLookTraits, TabLookCount> TabLookTraitRows {
    TabLookTraits { .corners = TabCorners::Row,
                    .seat = TabSeat::Row,
                    .active = TabActiveMark::Row,
                    .edge = TabBarEdge::Row },
    TabLookTraits { .corners = TabCorners::Round,
                    .seat = TabSeat::Floating,
                    .active = TabActiveMark::Wash,
                    .edge = TabBarEdge::Row },
    TabLookTraits { .corners = TabCorners::Square,
                    .seat = TabSeat::Row,
                    .active = TabActiveMark::Wash,
                    .edge = TabBarEdge::Ruled },
    TabLookTraits { .corners = TabCorners::Square,
                    .seat = TabSeat::Row,
                    .active = TabActiveMark::Underline,
                    .edge = TabBarEdge::Ruled },
    TabLookTraits { .corners = TabCorners::Round,
                    .seat = TabSeat::Floating,
                    .active = TabActiveMark::Outline,
                    .edge = TabBarEdge::Row },
};

/// @param look A look.
/// @return What it draws.
[[nodiscard]] constexpr TabLookTraits const& tabLookTraits(TabLook look) noexcept
{
    return TabLookTraitRows[static_cast<size_t>(look)];
}

/// What the tab bar is made of (`tab_style.material`): a wish, which the chrome row and the operating
/// system may not be able to grant (config::bandMaterialFor).
///
/// Named for what the user sees, not for one platform's material: each chrome row grants a wish with its
/// own (@c BandMaterialGrants), so one configuration reads the same on every system. Zero is `Auto`, the
/// row's own material.
enum class TabBarMaterial : uint8_t
{
    Auto = 0, //!< The row's own.
    Solid,    //!< Opaque: nothing shows through the bar.
    Frosted,  //!< Mostly opaque, softly coloured by what is behind the window.
    Glass,    //!< Clearly see-through: a blurred view of what is behind the window.
};

namespace detail
{
    /// The material spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto TabBarMaterialTable = std::array {
        ConfigEnumInfo<TabBarMaterial> { TabBarMaterial::Auto, "auto", "Automatic" },
        ConfigEnumInfo<TabBarMaterial> { TabBarMaterial::Solid, "solid", "Solid" },
        ConfigEnumInfo<TabBarMaterial> { TabBarMaterial::Frosted, "frosted", "Frosted" },
        ConfigEnumInfo<TabBarMaterial> { TabBarMaterial::Glass, "glass", "Glass" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabBarMaterial> const> configEnumValues() noexcept
{
    return detail::TabBarMaterialTable;
}

/// When a tab shows its close button (`tab_style.close_button`). Zero is `Auto`, the chrome row's own.
enum class TabCloseButton : uint8_t
{
    Auto = 0, //!< The row's own: on every tab of a flat strip, on the active or hovered tab elsewhere.
    Hover,    //!< Only on the tab under the pointer.
    Active,   //!< On the active tab, and on the tab under the pointer.
    Always,   //!< On every tab.
    Never,    //!< On no tab: a tab closes with a middle click, its menu or the keyboard.
};

namespace detail
{
    /// The close-button spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto TabCloseButtonTable = std::array {
        ConfigEnumInfo<TabCloseButton> { TabCloseButton::Auto, "auto", "Automatic" },
        ConfigEnumInfo<TabCloseButton> { TabCloseButton::Hover, "hover", "On hover" },
        ConfigEnumInfo<TabCloseButton> { TabCloseButton::Active, "active", "Active tab and hover" },
        ConfigEnumInfo<TabCloseButton> { TabCloseButton::Always, "always", "Always" },
        ConfigEnumInfo<TabCloseButton> { TabCloseButton::Never, "never", "Never" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabCloseButton> const> configEnumValues() noexcept
{
    return detail::TabCloseButtonTable;
}

/// When a tab's close button is visible, once `tab_style.close_button` is resolved against the row.
enum class TabCloseVisibility : uint8_t
{
    Always = 0,      //!< On every tab.
    ActiveOrHovered, //!< On the active tab and the tab under the pointer.
    Hovered,         //!< On the tab under the pointer alone.
    Never,           //!< On no tab, and no room is kept for it.
};

/// What each setting resolves to, indexed by @c TabCloseButton; @c Auto's row is unused (the row decides).
inline constexpr std::array<TabCloseVisibility, 5> TabCloseVisibilities {
    TabCloseVisibility::Always,          // Auto: replaced by the row's own
    TabCloseVisibility::Hovered,         // Hover
    TabCloseVisibility::ActiveOrHovered, // Active
    TabCloseVisibility::Always,          // Always
    TabCloseVisibility::Never,           // Never
};

/// @param setting   The configured `tab_style.close_button`.
/// @param rowOwn    When the row shows a close button by itself.
/// @return When the close button is visible.
[[nodiscard]] constexpr TabCloseVisibility closeVisibilityOf(TabCloseButton setting,
                                                             TabCloseVisibility rowOwn) noexcept
{
    return setting == TabCloseButton::Auto ? rowOwn : TabCloseVisibilities[static_cast<size_t>(setting)];
}

/// How wide the tabs are (`tab_style.tab_width`). Zero is `Auto`, the chrome row's own.
enum class TabWidth : uint8_t
{
    Auto = 0, //!< The row's own: equal pills on macOS and Linux, each tab as wide as its title elsewhere.
    Equal,    //!< Every tab as wide as the others, shrinking together as tabs are added.
    Fit,      //!< Each tab as wide as its title needs, within the minimum and maximum.
};

namespace detail
{
    /// The tab-width spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto TabWidthTable = std::array {
        ConfigEnumInfo<TabWidth> { TabWidth::Auto, "auto", "Automatic" },
        ConfigEnumInfo<TabWidth> { TabWidth::Equal, "equal", "Equal" },
        ConfigEnumInfo<TabWidth> { TabWidth::Fit, "fit", "Fit the title" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabWidth> const> configEnumValues() noexcept
{
    return detail::TabWidthTable;
}

/// How the row lays its tabs out by width, once `tab_style.tab_width` is resolved against it.
enum class TabWidthLayout : uint8_t
{
    Fit = 0, //!< Each tab its natural width, clamped to the minimum and maximum.
    Equal,   //!< One width for every tab: the share of the strip, clamped to the minimum and maximum.
};

/// The widest `tab_style.min_tab_width` and `max_tab_width` take, in logical pixels.
inline constexpr int MaxTabWidthSetting = 1000;

/// Whether the window shows a new-tab button (`tab_style.new_tab_button`). Zero shows it, as always.
enum class NewTabButton : uint8_t
{
    Shown = 0, //!< Where the row puts it: after the tabs, in the toolbar or in the header bar.
    Hidden,    //!< Nowhere: a tab opens from the keyboard, the menus or the command palette.
};

namespace detail
{
    /// The new-tab-button spellings. The tokens are written into the configuration, so once shipped they are
    /// fixed.
    inline constexpr auto NewTabButtonTable = std::array {
        ConfigEnumInfo<NewTabButton> { NewTabButton::Shown, "shown", "Shown" },
        ConfigEnumInfo<NewTabButton> { NewTabButton::Hidden, "hidden", "Hidden" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<NewTabButton> const> configEnumValues() noexcept
{
    return detail::NewTabButtonTable;
}

/// The widest gap `tab_style.spacing` takes, in logical pixels.
inline constexpr int MaxTabSpacing = 32;

/// How much of a coloured `tab_style.bar_fill` is laid over a bar whose material shows through it, as a
/// percentage: enough that the bar reads as that colour, little enough that the material still shows.
inline constexpr int BarFillOverMaterialPercent = 70;

/// Whether @p fill is a colour of its own rather than the row's or a wash of the row's tone: what puts a
/// new ground under whatever is drawn on it.
/// @param fill A fill of the section.
/// @return Whether it is the accent or a colour of the user's.
[[nodiscard]] constexpr bool bringsItsOwnTone(TabFill const& fill) noexcept
{
    return fill.kind == TabFillKind::Accent || fill.kind == TabFillKind::Custom;
}

/// The `tab_style:` section. Every member defaults to the chrome row's own look.
///
/// GLOBAL rather than per-profile: a window draws one tab strip whichever profiles its tabs run.
struct TabStyleConfig
{
    TabLook shape = TabLook::Auto; ///< How each tab is drawn (`tab_style.shape`).
    /// The gap between two tabs, in logical pixels from 0 to @c MaxTabSpacing (`tab_style.spacing`).
    int spacing = 0;
    /// What the tab bar is made of (`tab_style.material`). Not a token of the tabs: it changes the chrome
    /// row itself, in config::resolveChromeTokens.
    TabBarMaterial material = TabBarMaterial::Auto;
    TabFill activeFill {};   ///< The active tab's fill (`tab_style.active_fill`).
    TabFill inactiveFill {}; ///< Every other tab's fill (`tab_style.inactive_fill`).
    /// The bar's own fill (`tab_style.bar_fill`): the row's, a wash, the accent or a colour of the user's.
    TabFill barFill {};
    /// The accent line on the active tab (`tab_style.active_indicator`).
    TabIndicator activeIndicator = TabIndicator::None;
    /// The indicator line's colour (`tab_style.indicator_color`): the accent, or a colour of the user's.
    TabFill indicatorColor = DefaultIndicatorColor;
    /// How a tab shows a colour of its own (`tab_style.tab_color_mode`).
    TabColorMode tabColorMode = TabColorMode::Fill;
    /// When a tab shows its close button (`tab_style.close_button`).
    TabCloseButton closeButton = TabCloseButton::Auto;
    NewTabButton newTabButton = NewTabButton::Shown; ///< Whether there is a new-tab button.
    TabWidth tabWidth = TabWidth::Auto;              ///< How wide the tabs are (`tab_style.tab_width`).
    /// The narrowest and widest a tab gets, in logical pixels; 0 keeps the row's own
    /// (`tab_style.min_tab_width`, `max_tab_width`).
    int minTabWidth = 0;
    int maxTabWidth = 0;

    [[nodiscard]] bool operator==(TabStyleConfig const&) const = default;
};

/// Where the line between two joined tabs is drawn, and how strongly: a defaults edition's choice
/// (EditionDefaults::tabSeparators), so a configuration that names no edition keeps the look it had.
enum class TabSeparators : uint8_t
{
    /// After every tab at rest but the strip's last, at the chrome's separator strength
    /// (ContrastTokens::separatorPercent): the classic look.
    Classic = 0,
    /// Only where two tabs at rest meet, never beside the active tab, which its fill already sets apart; at
    /// the row's own tab-separator strength (ContrastTokens::tabSeparatorPercent).
    BetweenResting,
};

/// What of a chrome row the section is laid over.
struct TabStyleRow
{
    /// The row's opacity for the line between two tabs under TabSeparators::BetweenResting, as resolved
    /// against the accessibility preferences (@c ContrastTokens::tabSeparatorPercent).
    int separatorPercent = 0;
    /// The chrome's separator opacity, which the line takes under TabSeparators::Classic
    /// (@c ContrastTokens::separatorPercent).
    int classicSeparatorPercent = 0;
    TabSeparators separators = TabSeparators::Classic; ///< Where the line goes: the edition's choice.
    /// What the row counts its chrome in. A row that counts in cells draws its tabs as cells, so it takes
    /// neither a look nor a gap.
    LengthUnit unit = LengthUnit::Pixel;
    /// When the row shows a close button by itself: on every tab of a flat strip, on the active or hovered
    /// tab of the pills and the joined tabs.
    TabCloseVisibility closeVisibility = TabCloseVisibility::Always;
    /// How the row lays its tabs out by width by itself: equal pills in a well, each tab its title's width
    /// elsewhere.
    TabWidthLayout widths = TabWidthLayout::Fit;
    int minTabWidth = 0; ///< The row's own narrowest tab, in logical pixels.
    int maxTabWidth = 0; ///< The row's own widest tab, in logical pixels.
};

/// How the tabs are drawn once the user's `tab_style:` is laid over the chrome row.
struct TabStyleTokens
{
    TabLookTraits look {};                             ///< What each tab draws.
    int spacing = 0;                                   ///< The gap between two tabs, in logical pixels.
    int separatorPercent = 0;                          ///< Opacity of the line between two tabs.
    TabSeparators separators = TabSeparators::Classic; ///< Where that line is drawn.
    TabFill activeFill {};                             ///< The active tab's fill; @c Auto keeps the row's.
    TabFill inactiveFill {};                           ///< Every other tab's fill; @c Auto keeps the row's.
    TabFill barFill {};                                ///< The bar's own fill; @c Auto keeps the row's.
    TabIndicator indicator = TabIndicator::None;       ///< The line on the active tab.
    TabFill indicatorColor = DefaultIndicatorColor;    ///< That line's colour: the accent, or the user's.
    TabColorMode tabColorMode = TabColorMode::Fill;    ///< How a tab shows a colour of its own.
    TabCloseVisibility closeVisibility = TabCloseVisibility::Always; ///< When the close button shows.
    NewTabButton newTabButton = NewTabButton::Shown;                 ///< Whether there is a new-tab button.
    TabWidthLayout widths = TabWidthLayout::Fit;                     ///< How the tabs are laid out by width.
    int minTabWidth = 0; ///< The narrowest tab, in logical pixels.
    int maxTabWidth = 0; ///< The widest tab, in logical pixels; never below the narrowest.

    [[nodiscard]] bool operator==(TabStyleTokens const&) const = default;
};

/// Lays the user's `tab_style:` over a chrome row's tab look.
///
/// A gap replaces the line between two tabs: tabs that do not touch need nothing drawn between them.
/// How a row counted in pixels lays its tabs out by width, once `tab_style.tab_width` is laid over it.
/// @param wish   The configured `tab_style.tab_width`.
/// @param rowOwn The row's own layout.
/// @return The layout @p wish names, else the row's own. A row whose own layout is Equal keeps it: its
///         pills sit in a well, which is laid out around equal pills.
[[nodiscard]] constexpr TabWidthLayout tabWidthLayoutOf(TabWidth wish, TabWidthLayout rowOwn) noexcept
{
    if (rowOwn == TabWidthLayout::Equal)
        return rowOwn;
    switch (wish)
    {
        case TabWidth::Auto: return rowOwn;
        case TabWidth::Equal: return TabWidthLayout::Equal;
        case TabWidth::Fit: return TabWidthLayout::Fit;
    }
    return rowOwn;
}

/// @param row   What of the row the section is laid over.
/// @param style The configured section.
/// @return What the tabs are drawn with: the row's value wherever the section leaves one to it.
[[nodiscard]] constexpr TabStyleTokens resolveTabStyle(TabStyleRow const& row,
                                                       TabStyleConfig const& style) noexcept
{
    auto const pixels = row.unit == LengthUnit::Pixel;
    auto const spacing = pixels ? std::clamp(style.spacing, 0, MaxTabSpacing) : 0;
    // A width of the user's replaces the row's where it is set; a row counted in cells keeps its own, as its
    // tabs are cells.
    auto const widthOr = [pixels](int setting, int rowOwn) {
        return pixels && setting > 0 ? std::min(setting, MaxTabWidthSetting) : rowOwn;
    };
    auto const minTab = widthOr(style.minTabWidth, row.minTabWidth);
    auto const maxTab = widthOr(style.maxTabWidth, row.maxTabWidth);
    return TabStyleTokens {
        .look = tabLookTraits(pixels ? style.shape : TabLook::Auto),
        .spacing = spacing,
        .separatorPercent = spacing > 0 ? 0
                                        : std::clamp(row.separators == TabSeparators::BetweenResting
                                                         ? row.separatorPercent
                                                         : row.classicSeparatorPercent,
                                                     0,
                                                     100),
        .separators = row.separators,
        .activeFill = style.activeFill,
        .inactiveFill = style.inactiveFill,
        .barFill = style.barFill,
        .indicator = style.activeIndicator,
        .indicatorColor = style.indicatorColor,
        .tabColorMode = style.tabColorMode,
        .closeVisibility = closeVisibilityOf(style.closeButton, row.closeVisibility),
        .newTabButton = style.newTabButton,
        .widths = pixels ? tabWidthLayoutOf(style.tabWidth, row.widths) : row.widths,
        .minTabWidth = minTab,
        .maxTabWidth = std::max(minTab, maxTab),
    };
}

} // namespace contour::config
