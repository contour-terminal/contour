// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a tab's colour paints on its sidebar rows, decided by the configured style (`sidebar.tab_color`) and
/// by the row's state. Pure and Qt-free, so the QML only draws what it is handed and the table is asserted
/// without a window.
///
/// The tint reuses the tab strip's blend (TabColorScheme.hpp): the row fades toward the sidebar's fill as
/// the tab loses prominence, so selection and hover stay told apart on a tinted row, and the text over it
/// turns black or white by the fill's lightness.

#include <contour/config/TabBarMode.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TabColorScheme.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace contour::window
{

/// Whether the pointer is over a sidebar row, and whether it holds it down.
enum class RowPointer : uint8_t
{
    Away = 0, ///< The pointer is elsewhere.
    Hovering, ///< The pointer is over the row.
    Pressing, ///< The pointer holds the row down.
};

/// Number of @c RowPointer enumerators, derived from the last one.
inline constexpr size_t RowPointerCount = static_cast<size_t>(RowPointer::Pressing) + 1;

/// The tab-strip prominence a sidebar row's tab colour is drawn at.
/// @param selection Whether the row is the window's shown tab; a folded workspace holding it counts as
///                  selected.
/// @param pointer   Whether the pointer is over the row.
/// @param activity  Whether the row's window is active.
/// @return As the tab strip decides a coloured tab's (WindowController's tabVisualStateFor): Active for the
///         selected row in any window, so it always keeps the most of the colour; else Pressed while held
///         down, so a press on a tinted row reads apart from a hover; else Hover while hovered; else Inactive
///         in the active window and InactiveWindowUnfocused in another.
[[nodiscard]] constexpr TabVisualState sidebarTabVisualState(RowSelection selection,
                                                             RowPointer pointer,
                                                             WindowActivity activity) noexcept
{
    switch (selection)
    {
        case RowSelection::Selected:
        case RowSelection::ContainsSelected: return TabVisualState::Active;
        case RowSelection::None: break;
    }
    switch (pointer)
    {
        case RowPointer::Pressing: return TabVisualState::Pressed;
        case RowPointer::Hovering: return TabVisualState::Hover;
        case RowPointer::Away: break;
    }
    return activity == WindowActivity::Active ? TabVisualState::Inactive
                                              : TabVisualState::InactiveWindowUnfocused;
}

/// What a colour lays under a sidebar row's text and marks: the fill a tab's or a workspace's tint gives the
/// row, and the ink everything written on it takes.
struct SidebarRowTint
{
    std::optional<vtbackend::RGBColor> fill; ///< The row's fill; none where nothing tints the row.
    std::optional<vtbackend::RGBColor> ink;  ///< The ink of what is written on the fill.

    [[nodiscard]] bool operator==(SidebarRowTint const&) const = default;
};

/// What a tab's colour paints on its sidebar row; an empty member paints nothing of that kind.
struct SidebarTabColorPaint
{
    std::optional<vtbackend::RGBColor> fill; ///< The row's fill, replacing its selection and hover fill.
    std::optional<vtbackend::RGBColor> bar;  ///< A bar at the row's leading edge.
    std::optional<vtbackend::RGBColor> ink;  ///< The name's colour.

    /// @return The tint this paint lays under the row's marks; none without a fill (`bar`, `name`).
    [[nodiscard]] SidebarRowTint tint() const
    {
        return fill ? SidebarRowTint { .fill = fill, .ink = ink } : SidebarRowTint {};
    }

    [[nodiscard]] bool operator==(SidebarTabColorPaint const&) const = default;
};

/// Decides what a tab's colour paints on one sidebar row.
/// @param style         The configured style.
/// @param tabColor      The tab's colour; none paints nothing, in every style.
/// @param rowBackground The sidebar's fill, the tint fades toward.
/// @param state         The row's prominence (sidebarTabVisualState).
/// @return Tint: the faded fill and a black or white ink for it. Bar: the colour as the bar. Name: the
///         colour as the ink.
[[nodiscard]] inline SidebarTabColorPaint sidebarTabColorPaint(config::SidebarTabColorStyle style,
                                                               std::optional<vtbackend::RGBColor> tabColor,
                                                               vtbackend::RGBColor rowBackground,
                                                               TabVisualState state) noexcept
{
    if (!tabColor)
        return {};
    auto paint = SidebarTabColorPaint {};
    switch (style)
    {
        case config::SidebarTabColorStyle::Tint:
            paint.fill = tabBackgroundColor(*tabColor, rowBackground, state);
            paint.ink = contrastingTextColor(*paint.fill);
            break;
        case config::SidebarTabColorStyle::Bar: paint.bar = *tabColor; break;
        case config::SidebarTabColorStyle::Name: paint.ink = *tabColor; break;
    }
    return paint;
}

/// The colour a status mark (a badge, a glyph, a progress meter) is drawn in on a sidebar row.
///
/// A mark keeps its own, semantic colour wherever it reads on what it sits on: on an untinted row, and on a
/// tint it stands off by WCAG 1.4.11's 3:1 for non-text marks (vtbackend::MinimumMarkContrast, the floor the
/// chrome holds every other mark to). Short of that -- an orange mark on an orange tab's tint -- it takes the
/// tint's own ink, the black or white the row's text already reads in.
/// @param mark The mark's semantic colour.
/// @param tint What the tab's or the workspace's colour lays under the row (their paint's tint()).
/// @return @p mark, or the tint's ink where @p mark would not stand off the tint.
[[nodiscard]] inline vtbackend::RGBColor sidebarMarkInk(vtbackend::RGBColor mark,
                                                        SidebarRowTint const& tint) noexcept
{
    if (!tint.fill || !tint.ink
        || vtbackend::contrastRatio(mark, *tint.fill) >= vtbackend::MinimumMarkContrast)
        return mark;
    return *tint.ink;
}

} // namespace contour::window
