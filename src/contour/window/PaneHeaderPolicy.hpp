// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeDensity.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>

namespace contour::window
{

/// What, besides the density, makes headers appear.
enum class HeaderReveal : uint8_t
{
    None = 0,     //!< Nothing is revealing them.
    HeldModifier, //!< The reveal modifier has been held long enough.
};

/// Whether the pointer is over the pane.
enum class PointerPresence : uint8_t
{
    Away = 0,
    Hovering,
};

/// A pane's focus state.
enum class PaneFocus : uint8_t
{
    Unfocused = 0,
    Focused,
    KeyboardFocused, //!< Focus reached the header by keyboard.
};

/// What the header text shows.
enum class PaneHeaderContent : uint8_t
{
    Hidden = 0, //!< No header.
    Name,       //!< The pane's name.
    NameAndCwd, //!< The name and the working directory.
};

/// Whether the header's action buttons show.
enum class HeaderActions : uint8_t
{
    Hidden = 0,
    Shown,
};

/// Whether the header takes room from the pane or floats over it.
enum class HeaderPlacement : uint8_t
{
    Inline = 0,
    Overlay,
};

/// How loudly the header reads.
enum class HeaderEmphasis : uint8_t
{
    Secondary = 0,
    Primary,
};

/// The narrowest pane that shows the working directory beside the name.
inline constexpr double CwdMinPaneWidth = 360.0;

/// Everything the header decision depends on.
struct PaneHeaderInputs
{
    config::ChromeDensity density = config::ChromeDensity::Comfortable;
    HeaderReveal reveal = HeaderReveal::None;
    PointerPresence pointer = PointerPresence::Away;
    PaneFocus focus = PaneFocus::Unfocused;
    config::PaneActionsReveal actionsReveal = config::PaneActionsReveal::OnHover;
    int visiblePanes = 1;            ///< Panes visible in the tab.
    double width = 0;                ///< The pane's width.
    double height = 0;               ///< The pane's height.
    double headerHeight = 0;         ///< The height an inline header would take.
    double minimumContentHeight = 0; ///< The least height the terminal content needs.
};

/// The decided header.
struct PaneHeader
{
    PaneHeaderContent content = PaneHeaderContent::Hidden;
    HeaderActions actions = HeaderActions::Hidden;
    HeaderPlacement placement = HeaderPlacement::Inline;
    HeaderEmphasis emphasis = HeaderEmphasis::Secondary;

    [[nodiscard]] bool operator==(PaneHeader const&) const = default;
};

/// How a density shows headers.
struct DensityHeaderRule
{
    config::ChromeDensity density;
    HeaderReveal needs; ///< What must reveal the header; None means any.
    HeaderPlacement placement;
};

/// One row per density, in enumerator order.
inline constexpr std::array<DensityHeaderRule, config::ChromeDensityCount> DensityHeaderRules {
    DensityHeaderRule { config::ChromeDensity::Comfortable, HeaderReveal::None, HeaderPlacement::Inline },
    DensityHeaderRule {
        config::ChromeDensity::Minimal, HeaderReveal::HeldModifier, HeaderPlacement::Overlay },
};

/// Decides a pane's header.
/// @param inputs What the decision depends on.
/// @return The header; PaneHeader {} (hidden) for a lone pane, an unmet reveal or too short an inline header.
[[nodiscard]] constexpr PaneHeader resolvePaneHeader(PaneHeaderInputs const& inputs) noexcept
{
    if (inputs.visiblePanes < 2)
        return {};

    auto const& rule = DensityHeaderRules[static_cast<size_t>(inputs.density)];
    if (rule.needs != HeaderReveal::None && rule.needs != inputs.reveal)
        return {};

    if (rule.placement == HeaderPlacement::Inline
        && inputs.height < inputs.headerHeight + inputs.minimumContentHeight)
        return {};

    auto const focused = inputs.focus != PaneFocus::Unfocused;
    auto const actionsShown =
        inputs.pointer == PointerPresence::Hovering || inputs.focus == PaneFocus::KeyboardFocused
        || (inputs.actionsReveal == config::PaneActionsReveal::OnHoverOrFocus && focused);
    return PaneHeader {
        .content = inputs.width >= CwdMinPaneWidth ? PaneHeaderContent::NameAndCwd : PaneHeaderContent::Name,
        .actions = actionsShown ? HeaderActions::Shown : HeaderActions::Hidden,
        .placement = rule.placement,
        .emphasis = focused ? HeaderEmphasis::Primary : HeaderEmphasis::Secondary,
    };
}

/// A button on the pane header.
enum class PaneHeaderAction : uint8_t
{
    SplitRight = 0,
    SplitDown,
    Zoom,
    Close,
};

/// A header button's command and its accessible name.
struct PaneHeaderActionRow
{
    PaneHeaderAction action;
    std::string_view commandName;    ///< The action's name in the command catalog.
    std::string_view accessibleName; ///< What assistive technology announces.
};

/// The header buttons, in enumerator order.
inline constexpr auto PaneHeaderActions = std::array {
    PaneHeaderActionRow { PaneHeaderAction::SplitRight, "SplitVertical", "Split right" },
    PaneHeaderActionRow { PaneHeaderAction::SplitDown, "SplitHorizontal", "Split down" },
    PaneHeaderActionRow { PaneHeaderAction::Zoom, "TogglePaneZoom", "Zoom pane" },
    PaneHeaderActionRow { PaneHeaderAction::Close, "ClosePane", "Close pane" },
};

static_assert(
    []() constexpr {
        auto const densitiesInOrder = std::ranges::all_of(
            std::views::iota(size_t { 0 }, DensityHeaderRules.size()), [](size_t i) constexpr {
                return DensityHeaderRules[i].density == static_cast<config::ChromeDensity>(i);
            });
        auto const actionsInOrder = std::ranges::all_of(
            std::views::iota(size_t { 0 }, PaneHeaderActions.size()), [](size_t i) constexpr {
                return PaneHeaderActions[i].action == static_cast<PaneHeaderAction>(i);
            });
        return densitiesInOrder && actionsInOrder;
    }(),
    "DensityHeaderRules and PaneHeaderActions must be in enumerator order");

/// The name a header shows.
/// @param processName The foreground process name.
/// @param title The pane's title.
/// @return @p processName unless empty, else @p title.
[[nodiscard]] inline std::string paneHeaderName(std::string_view processName, std::string_view title)
{
    return std::string { processName.empty() ? title : processName };
}

/// @param content The content kind.
/// @return Its QML-facing name.
[[nodiscard]] constexpr std::string_view paneHeaderContentName(PaneHeaderContent content) noexcept
{
    switch (content)
    {
        case PaneHeaderContent::Hidden: return "hidden";
        case PaneHeaderContent::Name: return "name";
        case PaneHeaderContent::NameAndCwd: return "nameAndCwd";
    }
    return "hidden";
}

/// @param actions The actions state.
/// @return Its QML-facing name.
[[nodiscard]] constexpr std::string_view headerActionsName(HeaderActions actions) noexcept
{
    switch (actions)
    {
        case HeaderActions::Hidden: return "hidden";
        case HeaderActions::Shown: return "shown";
    }
    return "hidden";
}

/// @param placement The placement.
/// @return Its QML-facing name.
[[nodiscard]] constexpr std::string_view headerPlacementName(HeaderPlacement placement) noexcept
{
    switch (placement)
    {
        case HeaderPlacement::Inline: return "inline";
        case HeaderPlacement::Overlay: return "overlay";
    }
    return "inline";
}

/// @param emphasis The emphasis.
/// @return Its QML-facing name.
[[nodiscard]] constexpr std::string_view headerEmphasisName(HeaderEmphasis emphasis) noexcept
{
    switch (emphasis)
    {
        case HeaderEmphasis::Secondary: return "secondary";
        case HeaderEmphasis::Primary: return "primary";
    }
    return "secondary";
}

} // namespace contour::window
