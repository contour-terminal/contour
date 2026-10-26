// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the title band and the tabs go: one table matched on the tab shape, the window frame, the tab-bar
/// gate, the host chip's notice and the tab-bar position, followed by one full-screen step. Pure and Qt-free,
/// so the band, its snapshot rows and the tests read the same rows.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/window/HostChipState.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
};

/// Number of @c BarPresence enumerators, derived from the last one.
inline constexpr size_t BarPresenceCount = static_cast<size_t>(BarPresence::RevealOnEdge) + 1;

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

/// Everything the band decision reads.
struct TitleBandInputs
{
    config::TabShape shape = config::TabShape::Flat;               ///< The row's tab shape.
    WindowFrame frame = WindowFrame::ClientBand;                   ///< Who draws the title area.
    WindowMode mode = WindowMode::Windowed;                        ///< Windowed or full screen.
    TabStripGate strip = TabStripGate::Shown;                      ///< The tab-bar gate's answer.
    config::TabBarPosition position = config::TabBarPosition::Top; ///< The configured tab-bar edge.
    HostNotice notice = HostNotice::None;                          ///< Whether the host chip must show.
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
    std::optional<config::TabShape> shape; ///< The tab shape, or any.
    std::optional<WindowFrame> frame;      ///< The frame, or any.
    std::optional<TabStripGate> strip;     ///< The tab-bar gate, or any.
    std::optional<HostNotice> notice;      ///< The host chip's notice, or any.
    config::TabBarPosition position;       ///< The configured tab-bar edge.
    TitleBand answer;                      ///< The windowed answer.
};

/// The band rules (R-e), first match wins. The Pill rows on a client band come first; the rest is the
/// strip, shown per the gate at its edge, as today.
inline constexpr auto TitleBandRules = std::array {
    // Pill on a client band: the band carries the traffic lights, so it always shows (D1); its centre holds
    // the tabs when the gate shows them, else the title.
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    // Pill with the tabs at the bottom: the band stays on top with the title, the tabs get a bar of their
    // own at the bottom (D6).
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Shown } },
    TitleBandRule { .shape = config::TabShape::Pill,
                    .frame = WindowFrame::ClientBand,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    // Flat on a client band, and every shape under a native frame: the band is the strip. A remote window
    // whose connection is not up shows it whatever the gate says: it is the host chip's home (R-d), and the
    // strip appearing is itself what tells the user the window is remote.
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = HostNotice::Interrupted,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = HostNotice::Interrupted,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .strip = TabStripGate::Hidden,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Top,
                    .answer = { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule { .shape = std::nullopt,
                    .frame = std::nullopt,
                    .strip = TabStripGate::Shown,
                    .notice = std::nullopt,
                    .position = config::TabBarPosition::Bottom,
                    .answer = { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    TitleBandRule {
        .shape = std::nullopt,
        .frame = std::nullopt,
        .strip = TabStripGate::Hidden,
        .notice = std::nullopt,
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

/// Finds the first band rule matching @p inputs; the mode is not a column.
/// @param inputs What the decision reads.
/// @return The matching row, or nullptr when none matches.
[[nodiscard]] constexpr TitleBandRule const* findTitleBandRule(TitleBandInputs const& inputs) noexcept
{
    for (auto const& rule: TitleBandRules)
        if (detail::columnMatches(rule.shape, inputs.shape) && detail::columnMatches(rule.frame, inputs.frame)
            && detail::columnMatches(rule.strip, inputs.strip)
            && detail::columnMatches(rule.notice, inputs.notice) && rule.position == inputs.position)
            return &rule;
    return nullptr;
}

namespace detail
{
    /// Calls @p visit with every windowed input the band decision can be asked about.
    template <typename Visit>
    constexpr void forEachTitleBandInput(Visit visit)
    {
        for (auto const shape: std::views::iota(size_t { 0 }, config::TabShapeCount))
            for (auto const frame: std::views::iota(size_t { 0 }, WindowFrameCount))
                for (auto const strip: std::views::iota(size_t { 0 }, TabStripGateCount))
                    for (auto const position: std::views::iota(size_t { 0 }, config::TabBarPositionCount))
                        for (auto const notice: std::views::iota(size_t { 0 }, HostNoticeCount))
                            visit(TitleBandInputs { .shape = static_cast<config::TabShape>(shape),
                                                    .frame = static_cast<WindowFrame>(frame),
                                                    .mode = WindowMode::Windowed,
                                                    .strip = static_cast<TabStripGate>(strip),
                                                    .position = static_cast<config::TabBarPosition>(position),
                                                    .notice = static_cast<HostNotice>(notice) });
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
///         (D3), while a bottom band and the tab bar are unchanged.
[[nodiscard]] constexpr TitleBand resolveTitleBand(TitleBandInputs const& inputs) noexcept
{
    auto const* const rule = findTitleBandRule(inputs);
    if (rule == nullptr)
        return {};
    auto band = rule->answer;
    if (inputs.mode == WindowMode::FullScreen && band.band == BarPresence::Shown
        && band.bandEdge == BarEdge::Top)
        band.band = BarPresence::RevealOnEdge;
    return band;
}

/// The name QML reads a bar presence by.
/// @param presence The presence.
/// @return "hidden", "shown" or "reveal".
[[nodiscard]] constexpr std::string_view barPresenceName(BarPresence presence) noexcept
{
    switch (presence)
    {
        case BarPresence::Hidden: return "hidden";
        case BarPresence::Shown: return "shown";
        case BarPresence::RevealOnEdge: return "reveal";
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

} // namespace contour::window
