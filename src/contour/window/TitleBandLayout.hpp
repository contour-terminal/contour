// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the title band and the tabs go: one table matched on the chrome row (its style, tab shape and
/// density), the window frame and mode, the tab-bar gate, the tab count, the host chip's notice and the
/// tab-bar position, followed by one full-screen step. Pure and Qt-free, so the band, its snapshot rows and
/// the tests read the same rows.

#include <contour/config/ChromeDensity.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/window/HostChipState.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// Who draws the window's title area; mirrors the @c show_title_bar setting.
enum class WindowFrame : uint8_t
{
    ClientBand = 0, ///< @c show_title_bar false: the chrome draws its own band.
    NativeFrame,    ///< @c show_title_bar true: the system frame draws the title.
};

/// Number of @c WindowFrame enumerators, derived from the last one.
inline constexpr size_t WindowFrameCount = static_cast<size_t>(WindowFrame::NativeFrame) + 1;

/// Whether the window is in full screen.
enum class WindowMode : uint8_t
{
    Windowed = 0, ///< An ordinary window.
    FullScreen,   ///< Full screen.
};

/// Number of @c WindowMode enumerators, derived from the last one.
inline constexpr size_t WindowModeCount = static_cast<size_t>(WindowMode::FullScreen) + 1;

/// What the tab-bar gate (@c config::isTabBarShown) answers.
enum class TabStripGate : uint8_t
{
    Hidden = 0, ///< The tab bar would be hidden (e.g. one tab under @c Multiple).
    Shown,      ///< The tab bar would be shown.
};

/// Number of @c TabStripGate enumerators, derived from the last one.
inline constexpr size_t TabStripGateCount = static_cast<size_t>(TabStripGate::Shown) + 1;

/// Whether a bar is on screen.
enum class BarPresence : uint8_t
{
    Hidden = 0,   ///< Not shown.
    Shown,        ///< Always shown.
    RevealOnEdge, ///< Overlays the top rows while the pointer is at the screen edge (full screen).
    Pinned, ///< Overlays the top rows, held revealed (full screen, while the connection is interrupted).
};

/// Number of @c BarPresence enumerators, derived from the last one.
inline constexpr size_t BarPresenceCount = static_cast<size_t>(BarPresence::Pinned) + 1;

/// The window edge a bar sits at.
enum class BarEdge : uint8_t
{
    Top = 0, ///< Above the terminal content.
    Bottom,  ///< Below the terminal content.
};

/// Number of @c BarEdge enumerators, derived from the last one.
inline constexpr size_t BarEdgeCount = static_cast<size_t>(BarEdge::Bottom) + 1;

/// What the middle of the band holds.
enum class BarCentre : uint8_t
{
    Empty = 0, ///< Nothing.
    Tabs,      ///< The tabs.
    Title,     ///< The active session's title, centred.
};

/// Number of @c BarCentre enumerators, derived from the last one.
inline constexpr size_t BarCentreCount = static_cast<size_t>(BarCentre::Title) + 1;

/// How many tabs the window holds, as the band decision reads it.
enum class OpenTabs : uint8_t
{
    Several = 0, ///< More than one (or none yet).
    One,         ///< Exactly one.
};

/// Number of @c OpenTabs enumerators, derived from the last one.
inline constexpr size_t OpenTabsCount = static_cast<size_t>(OpenTabs::One) + 1;

/// Everything the band decision reads.
struct TitleBandInputs
{
    config::TabShape shape = config::TabShape::Flat;                    ///< The row's tab shape.
    WindowFrame frame = WindowFrame::ClientBand;                        ///< Who draws the title area.
    WindowMode mode = WindowMode::Windowed;                             ///< Windowed or full screen.
    TabStripGate strip = TabStripGate::Shown;                           ///< The tab-bar gate's answer.
    config::TabBarPosition position = config::TabBarPosition::Top;      ///< The configured tab-bar edge.
    HostNotice notice = HostNotice::None;                               ///< Whether the host chip must show.
    config::ChromeDensity density = config::ChromeDensity::Comfortable; ///< The configured chrome density.
    config::ChromeStyle style = config::ChromeStyle::ContourLinux;      ///< The chrome row.
    OpenTabs tabs = OpenTabs::Several;                                  ///< How many tabs the window holds.
};

/// The chrome row's side of the band decision: what a window's inputs cannot say for themselves.
struct TitleBandRow
{
    config::ChromeStyle style = config::ChromeStyle::ContourLinux;      ///< The chrome row.
    config::TabShape shape = config::TabShape::Flat;                    ///< The row's tab shape.
    config::ChromeDensity density = config::ChromeDensity::Comfortable; ///< The configured chrome density.
};

/// Where the band and the tabs go.
struct TitleBand
{
    /// The bar holding the window's own affordances. On @c Flat and under a native frame it is the strip.
    BarPresence band = BarPresence::Hidden;
    BarEdge bandEdge = BarEdge::Top;          ///< The edge the band sits at.
    BarCentre centre = BarCentre::Empty;      ///< What the band's middle holds.
    BarPresence tabBar = BarPresence::Hidden; ///< A second bar holding only the tabs.

    bool operator==(TitleBand const&) const = default;
};

/// One row of the band table. An empty optional column matches any value.
struct TitleBandRule
{
    std::optional<config::TabShape> shape;        ///< The tab shape, or any.
    std::optional<WindowFrame> frame;             ///< The frame, or any.
    std::optional<WindowMode> mode;               ///< Windowed or full screen, or any.
    std::optional<TabStripGate> strip;            ///< The tab-bar gate, or any.
    std::optional<HostNotice> notice;             ///< The host chip's notice, or any.
    std::optional<config::ChromeDensity> density; ///< The chrome density, or any.
    std::optional<config::ChromeStyle> style;     ///< The chrome row, or any.
    std::optional<OpenTabs> tabs;                 ///< The tab count, or any.
    config::TabBarPosition position;              ///< The configured tab-bar edge.
    TitleBand answer; ///< The answer, before the full-screen step (resolveTitleBand).
};

/// The band rules (R-e), first match wins. The Pill rows on a client band come first; the rest is the
/// strip, shown per the gate at its edge, as today.
inline constexpr auto TitleBandRules = std::array {
    // Minimal's bare band in full screen: there are no traffic lights to carry, so an empty band would only
    // slide a strip of nothing over the top rows -- it stays hidden. A remote window whose connection is
    // interrupted falls through: its band stays pinned, the host chip saying why the screen stopped.
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .mode = WindowMode::FullScreen,
                    .strip = TabStripGate::Hidden,
                    .notice = HostNotice::None,
                    .density = config::ChromeDensity::Minimal,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Hidden, BarEdge::Top, BarCentre::Empty, BarPresence::Hidden } },
    // Minimal with the tab strip hidden (one tab under Multiple): a bare band, its middle empty -- the
    // traffic lights only, so the window reads bar-less without covering a cell (R-f). With the gate
    // shown, Minimal falls through to the Comfortable rows below.
    //
    // Accepted (Task 7 review, M2): opening a second tab, or closing it, jumps the band 32 -> 40 pt and back
    // (the compact toolbar comes and goes, ruling P3), the content reflowing by 8 pt, and AppKit moves the
    // traffic lights (78 <-> 84 pt). A second tab is a deliberate act, and both moves are AppKit's own.
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .density = config::ChromeDensity::Minimal,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Empty, BarPresence::Hidden } },
    // Minimal on the Terminal row hides its bar row while there is one tab, whatever the gate says
    // (tui.dc.html's Density note); Comfortable keeps it. A remote window whose connection is not up still
    // shows it, for the host chip (below).
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .mode = std::nullopt,
                    .strip = std::nullopt,
                    .notice = HostNotice::None,
                    .density = config::ChromeDensity::Minimal,
                    .style = config::ChromeStyle::Terminal,
                    .tabs = OpenTabs::One,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule {
        .shape = std::nullopt,
        .frame = std::nullopt,
        .mode = std::nullopt,
        .strip = std::nullopt,
        .notice = HostNotice::None,
        .density = config::ChromeDensity::Minimal,
        .style = config::ChromeStyle::Terminal,
        .tabs = OpenTabs::One,
        .position = config::TabBarPosition::Bottom,
        .answer = { BarPresence::Hidden, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    // Pill on a client band: the band carries the traffic lights, so it always shows (D1); its centre holds
    // the tabs when the gate shows them, else the title.
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    // Pill with the tabs at the bottom: the band stays on top with the title, the tabs get a bar of their
    // own at the bottom (D6).
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Shown } },
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    // Flat on a client band, and every shape under a native frame: the band is the strip. A remote window
    // whose connection is not up shows it whatever the gate says: it is the host chip's home (R-d), and the
    // strip appearing is itself what tells the user the window is remote.
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = HostNotice::Interrupted,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = HostNotice::Interrupted,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .mode = std::nullopt,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .density = std::nullopt,
                    .style = std::nullopt,
                    .tabs = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule {
        .shape = std::nullopt,
        .frame = std::nullopt,
        .mode = std::nullopt,
        .strip = TabStripGate::Hidden,
        .notice = std::nullopt,
        .density = std::nullopt,
        .style = std::nullopt,
        .tabs = std::nullopt,
        .position = config::TabBarPosition::Bottom,
        .answer = { BarPresence::Hidden, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
};

namespace detail
{
    /// Whether an optional rule column matches a value: empty matches anything.
    template <typename T>
    [[nodiscard]] constexpr bool columnMatches(std::optional<T> const& column, T value) noexcept
    {
        return !column.has_value() || *column == value;
    }
} // namespace detail

/// Finds the first band rule matching @p inputs.
/// @param inputs What the decision reads.
/// @return The matching row, or nullptr when none matches.
[[nodiscard]] constexpr TitleBandRule const* findTitleBandRule(TitleBandInputs const& inputs) noexcept
{
    for (auto const& rule: TitleBandRules)
        if (detail::columnMatches(rule.shape, inputs.shape) && detail::columnMatches(rule.frame, inputs.frame)
            && detail::columnMatches(rule.mode, inputs.mode)
            && detail::columnMatches(rule.strip, inputs.strip)
            && detail::columnMatches(rule.notice, inputs.notice)
            && detail::columnMatches(rule.density, inputs.density)
            && detail::columnMatches(rule.style, inputs.style)
            && detail::columnMatches(rule.tabs, inputs.tabs) && rule.position == inputs.position)
            return &rule;
    return nullptr;
}

namespace detail
{
    /// Calls @p visit with every input the band decision can be asked about.
    template <typename Visit>
    constexpr void forEachTitleBandInput(Visit visit)
    {
        constexpr auto Extents = std::array { config::TabShapeCount,
                                              WindowFrameCount,
                                              WindowModeCount,
                                              TabStripGateCount,
                                              config::TabBarPositionCount,
                                              HostNoticeCount,
                                              config::ChromeDensityCount,
                                              config::detail::ChromeStyleCount,
                                              OpenTabsCount };
        constexpr auto Total =
            std::accumulate(Extents.begin(), Extents.end(), size_t { 1 }, std::multiplies {});
        for (auto const index: std::views::iota(size_t { 0 }, Total))
        {
            // Mixed-radix digits of the index, one per column, in the order the braces below read them.
            auto rest = index;
            auto digit = size_t { 0 };
            auto const next = [&] {
                auto const value = rest % Extents[digit];
                rest /= Extents[digit++];
                return value;
            };
            visit(TitleBandInputs { .shape = static_cast<config::TabShape>(next()),
                                    .frame = static_cast<WindowFrame>(next()),
                                    .mode = static_cast<WindowMode>(next()),
                                    .strip = static_cast<TabStripGate>(next()),
                                    .position = static_cast<config::TabBarPosition>(next()),
                                    .notice = static_cast<HostNotice>(next()),
                                    .density = static_cast<config::ChromeDensity>(next()),
                                    .style = static_cast<config::ChromeStyle>(next()),
                                    .tabs = static_cast<OpenTabs>(next()) });
        }
    }

    /// @return Whether every input matches a rule.
    [[nodiscard]] constexpr bool titleBandRulesCoverEveryInput() noexcept
    {
        auto covered = true;
        forEachTitleBandInput([&](TitleBandInputs const& inputs) {
            covered = covered && findTitleBandRule(inputs) != nullptr;
        });
        return covered;
    }

    /// @return Whether every rule is the first match for some input: no row is shadowed by the ones above it.
    [[nodiscard]] constexpr bool titleBandRulesAreEachReachable() noexcept
    {
        auto reached = std::array<bool, TitleBandRules.size()> {};
        forEachTitleBandInput([&](TitleBandInputs const& inputs) {
            if (auto const* const rule = findTitleBandRule(inputs); rule != nullptr)
                reached[static_cast<size_t>(rule - TitleBandRules.data())] = true;
        });
        return std::ranges::all_of(reached, std::identity {});
    }
} // namespace detail

static_assert(detail::titleBandRulesCoverEveryInput(), "TitleBandRules must match every input");
static_assert(detail::titleBandRulesAreEachReachable(),
              "every TitleBandRules row must be the first match for some input");

/// Decides where the band and the tabs go.
/// @param inputs What the decision reads.
/// @return The matching row's answer; in full screen, a band shown at the top reveals on the edge instead
///         (D3) -- or stays pinned over the top rows while the host connection is interrupted, since a
///         frozen screen says nothing about why -- while a bottom band and the tab bar are unchanged.
[[nodiscard]] constexpr TitleBand resolveTitleBand(TitleBandInputs const& inputs) noexcept
{
    auto const* const rule = findTitleBandRule(inputs);
    if (rule == nullptr)
        return {};
    auto band = rule->answer;
    if (inputs.mode == WindowMode::FullScreen && band.band == BarPresence::Shown
        && band.bandEdge == BarEdge::Top)
        band.band =
            inputs.notice == HostNotice::Interrupted ? BarPresence::Pinned : BarPresence::RevealOnEdge;
    return band;
}

/// Decides where the band and the tabs go for a chrome row: @p inputs with the row's style, tab shape and
/// density, whatever @p inputs says of them -- the one place a provider or the app fills them in.
/// @param row    The chrome row's side.
/// @param inputs The window's side: frame, mode, gate, tab count, edge and the host's notice.
/// @return The band.
[[nodiscard]] constexpr TitleBand resolveTitleBand(TitleBandRow const& row, TitleBandInputs inputs) noexcept
{
    inputs.style = row.style;
    inputs.shape = row.shape;
    inputs.density = row.density;
    return resolveTitleBand(inputs);
}

/// The name QML reads a bar presence by.
/// @param presence The presence.
/// @return "hidden", "shown", "reveal" or "pinned".
[[nodiscard]] constexpr std::string_view barPresenceName(BarPresence presence) noexcept
{
    switch (presence)
    {
        case BarPresence::Hidden: return "hidden";
        case BarPresence::Shown: return "shown";
        case BarPresence::RevealOnEdge: return "reveal";
        case BarPresence::Pinned: return "pinned";
    }
    return "hidden";
}

/// The name QML reads a bar edge by.
/// @param edge The edge.
/// @return "top" or "bottom".
[[nodiscard]] constexpr std::string_view barEdgeName(BarEdge edge) noexcept
{
    switch (edge)
    {
        case BarEdge::Top: return "top";
        case BarEdge::Bottom: return "bottom";
    }
    return "top";
}

/// The name QML reads a band centre by.
/// @param centre The centre.
/// @return "empty", "tabs" or "title".
[[nodiscard]] constexpr std::string_view barCentreName(BarCentre centre) noexcept
{
    switch (centre)
    {
        case BarCentre::Empty: return "empty";
        case BarCentre::Tabs: return "tabs";
        case BarCentre::Title: return "title";
    }
    return "empty";
}

/// The band centre named @p name (barCentreName()'s inverse).
/// @param name A centre's name, as QML reads it.
/// @return The centre, or @c std::nullopt for a name no centre has.
[[nodiscard]] constexpr std::optional<BarCentre> barCentreNamed(std::string_view name) noexcept
{
    for (auto const index: std::views::iota(size_t { 0 }, BarCentreCount))
        if (auto const centre = static_cast<BarCentre>(index); barCentreName(centre) == name)
            return centre;
    return std::nullopt;
}

} // namespace contour::window
