// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a sidebar row wears the selection, decided by the look and by where the keyboard is: which fill,
/// whether the focus ring and the accent pill are drawn. Pure and Qt-free, so the QML only draws what it is
/// handed and the table is asserted without a window.
///
/// The rulings, per platform:
///  - macOS (source list): the accent fill while the sidebar has the keyboard, the grey unemphasized fill
///    otherwise.
///  - Linux (libadwaita): a neutral fill, focused or not; keyboard focus adds the accent ring.
///  - Fluent (NavigationView): a subtle grey fill and a rounded accent pill at the leading edge, focused or
///  not.
///  - Terminal: inverse video while the keyboard is in the tree, the quiet run of cells otherwise.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/window/SidebarTree.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// Where the keyboard is, as far as one sidebar row is concerned.
enum class SidebarFocus : uint8_t
{
    Elsewhere = 0, ///< In the terminal (or another surface): the tree has no keyboard focus.
    InTree,        ///< In the tree, on another row.
    OnRow,         ///< In the tree, on this very row.
};

/// The fill a row's selection is drawn with.
enum class SelectionFill : uint8_t
{
    None = 0,   ///< The row is not selected.
    Quiet,      ///< The neutral fill (`SidebarSelectionFillInactive`, and its ink).
    Emphasized, ///< The look's emphasized fill (`SidebarSelectionFill`, and its ink).
};

/// Whether a selected row's fill changes with the keyboard focus.
enum class SelectionEmphasis : uint8_t
{
    Never = 0,    ///< The fill is the same focused or not.
    WhileFocused, ///< Emphasized while the tree has the keyboard, quiet otherwise.
};

/// Whether the selected row carries an accent pill at its leading edge.
enum class AccentPill : uint8_t
{
    Hidden = 0,
    Shown,
};

/// Whether the keyboard-focused row is ringed.
enum class FocusRing : uint8_t
{
    Hidden = 0,
    Shown,
};

/// How one sidebar look wears the selection.
struct SidebarSelectionRule
{
    config::SidebarLook look = config::SidebarLook::SourceList; ///< The look the row describes.
    SelectionEmphasis emphasis = SelectionEmphasis::Never;      ///< Whether keyboard focus changes the fill.
    AccentPill pill = AccentPill::Hidden;                       ///< Whether the selected row has the pill.
};

/// The rules, in SidebarLook order.
inline constexpr auto SidebarSelectionRules = std::array {
    SidebarSelectionRule { .look = config::SidebarLook::SourceList,
                           .emphasis = SelectionEmphasis::WhileFocused,
                           .pill = AccentPill::Hidden },
    SidebarSelectionRule { .look = config::SidebarLook::NavigationPane,
                           .emphasis = SelectionEmphasis::Never,
                           .pill = AccentPill::Shown },
    SidebarSelectionRule { .look = config::SidebarLook::HeaderPane,
                           .emphasis = SelectionEmphasis::Never,
                           .pill = AccentPill::Hidden },
    SidebarSelectionRule { .look = config::SidebarLook::CellTree,
                           .emphasis = SelectionEmphasis::WhileFocused,
                           .pill = AccentPill::Hidden },
};

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, SidebarSelectionRules.size()),
                                  [](size_t index) {
                                      return SidebarSelectionRules[index].look
                                             == static_cast<config::SidebarLook>(index);
                                  }),
              "SidebarSelectionRules lists every SidebarLook once, in enumerator order");

/// What one row draws for its selection.
struct SidebarSelectionPaint
{
    SelectionFill fill = SelectionFill::None; ///< The fill.
    FocusRing ring = FocusRing::Hidden;       ///< The keyboard focus ring.
    AccentPill pill = AccentPill::Hidden;     ///< The accent pill.

    [[nodiscard]] bool operator==(SidebarSelectionPaint const&) const = default;
};

/// Decides how a row wears the selection.
/// @param look      The sidebar look.
/// @param selection Whether the row is what the window shows. A folded workspace standing in for the shown
/// tab
///                  is never more than the quiet fill, and has no pill.
/// @param focus     Where the keyboard is.
/// @return What to draw.
[[nodiscard]] constexpr SidebarSelectionPaint sidebarSelectionPaint(config::SidebarLook look,
                                                                    RowSelection selection,
                                                                    SidebarFocus focus) noexcept
{
    auto const& rule = SidebarSelectionRules[static_cast<size_t>(look)];
    auto paint = SidebarSelectionPaint {};
    paint.ring = focus == SidebarFocus::OnRow ? FocusRing::Shown : FocusRing::Hidden;
    switch (selection)
    {
        case RowSelection::None: break;
        case RowSelection::ContainsSelected: paint.fill = SelectionFill::Quiet; break;
        case RowSelection::Selected:
            paint.fill = rule.emphasis == SelectionEmphasis::WhileFocused && focus != SidebarFocus::Elsewhere
                             ? SelectionFill::Emphasized
                             : SelectionFill::Quiet;
            paint.pill = rule.pill;
            break;
    }
    return paint;
}

/// @param fill A fill.
/// @return The name QML reads it by: "none", "quiet" or "emphasized".
[[nodiscard]] constexpr std::string_view selectionFillName(SelectionFill fill) noexcept
{
    switch (fill)
    {
        case SelectionFill::None: return "none";
        case SelectionFill::Quiet: return "quiet";
        case SelectionFill::Emphasized: return "emphasized";
    }
    return "none";
}

} // namespace contour::window
