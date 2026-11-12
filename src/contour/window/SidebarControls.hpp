// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the workspace sidebar's own buttons look like on each sidebar look: the toggle that hides and shows
/// it, the button that makes a workspace, and the one that opens its filter. Pure and Qt-free, so the
/// provider, the QML and the tests read the same rows.
///
/// A look that stands in a bar of themed icons (the ContourLinux header bar) names a freedesktop icon and the
/// bundled copy drawn where the theme lacks it; a look with its own symbol set (macOS, Fluent) draws the
/// bundled icon alone; the Terminal row draws a glyph in cells. A row never puts a text glyph beside an icon
/// in the same bar, and no icon here means anything else in its row: the toggle is not the main menu's
/// hamburger, and the new workspace is not the new tab's "+".

#include <contour/config/ChromeTokenGroups.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// A button of the sidebar's own.
enum class SidebarControl : uint8_t
{
    Toggle = 0,   ///< Hides and shows the sidebar.
    NewWorkspace, ///< Makes a workspace.
    Filter,       ///< Opens the filter field over the tree.
};

/// Number of @c SidebarControl enumerators, derived from the last one.
inline constexpr size_t SidebarControlCount = static_cast<size_t>(SidebarControl::Filter) + 1;

/// How one control is drawn.
struct SidebarControlFace
{
    std::string_view iconName {};    ///< The freedesktop icon, resolved through the theme; empty for none.
    std::string_view bundledIcon {}; ///< The icon drawn where the theme lacks @c iconName; empty for a glyph.
    std::string_view glyph {};       ///< The glyph drawn where there is no icon.

    [[nodiscard]] bool operator==(SidebarControlFace const&) const = default;
};

/// The faces of one look's controls, indexed by SidebarControl.
struct SidebarControlRow
{
    config::SidebarLook look = config::SidebarLook::SourceList;   ///< The look the row describes.
    std::array<SidebarControlFace, SidebarControlCount> faces {}; ///< Toggle, NewWorkspace, Filter.
};

namespace detail
{
    inline constexpr auto ToggleIcon = std::string_view { "qrc:/contour/icons/sidebar-show-symbolic.svg" };
    inline constexpr auto PaneToggleIcon = std::string_view { "qrc:/contour/icons/pane-toggle-symbolic.svg" };
    inline constexpr auto NewWorkspaceIcon =
        std::string_view { "qrc:/contour/icons/folder-new-symbolic.svg" };
    inline constexpr auto FilterIcon = std::string_view { "qrc:/contour/icons/view-filter-symbolic.svg" };
} // namespace detail

/// The controls of every look, in SidebarLook order.
inline constexpr auto SidebarControlRows = std::array {
    // macOS: SF-Symbol-shaped icons in the title row ("sidebar.left", a funnel); the source list's "+" in its
    // footer is the platform's text button.
    SidebarControlRow { .look = config::SidebarLook::SourceList,
                        .faces = { SidebarControlFace { .bundledIcon = detail::ToggleIcon },
                                   SidebarControlFace { .glyph = "+" },
                                   SidebarControlFace { .bundledIcon = detail::FilterIcon } } },
    // Fluent: the navigation pane's own toggle (GlobalNavigationButton, three lines) and a funnel; the row
    // has no main menu for the lines to be mistaken for. The "+" heads the footer, as a pane footer item.
    SidebarControlRow { .look = config::SidebarLook::NavigationPane,
                        .faces = { SidebarControlFace { .bundledIcon = detail::PaneToggleIcon },
                                   SidebarControlFace { .glyph = "+" },
                                   SidebarControlFace { .bundledIcon = detail::FilterIcon } } },
    // ContourLinux: themed symbolic icons in the header bar, like its other items (HeaderBarItems).
    SidebarControlRow { .look = config::SidebarLook::HeaderPane,
                        .faces = { SidebarControlFace { .iconName = "sidebar-show-symbolic",
                                                        .bundledIcon = detail::ToggleIcon },
                                   SidebarControlFace { .iconName = "folder-new-symbolic",
                                                        .bundledIcon = detail::NewWorkspaceIcon },
                                   SidebarControlFace { .iconName = "view-filter-symbolic",
                                                        .bundledIcon = detail::FilterIcon } } },
    // Terminal: one cell each.
    SidebarControlRow { .look = config::SidebarLook::CellTree,
                        .faces = { SidebarControlFace { .glyph = "☰" },
                                   SidebarControlFace { .glyph = "+" },
                                   SidebarControlFace { .glyph = "/" } } },
};

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, SidebarControlRows.size()),
                                  [](size_t index) {
                                      return SidebarControlRows[index].look
                                             == static_cast<config::SidebarLook>(index);
                                  }),
              "SidebarControlRows lists every SidebarLook once, in enumerator order");
static_assert(std::ranges::all_of(SidebarControlRows,
                                  [](SidebarControlRow const& row) {
                                      return std::ranges::all_of(
                                          row.faces, [](SidebarControlFace const& face) {
                                              return !face.bundledIcon.empty() || !face.glyph.empty();
                                          });
                                  }),
              "every control draws an icon or a glyph");

/// @param look A sidebar look.
/// @param control One of its controls.
/// @return How it is drawn.
[[nodiscard]] constexpr SidebarControlFace const& sidebarControlFace(config::SidebarLook look,
                                                                     SidebarControl control) noexcept
{
    return SidebarControlRows[static_cast<size_t>(look)].faces[static_cast<size_t>(control)];
}

/// The action each control runs, indexed by SidebarControl: the same on every look. Its tooltip names the
/// chord that action is bound to at the time, and a key binding of it does what a click does.
inline constexpr auto SidebarControlCommands =
    std::array<std::string_view, SidebarControlCount> { "ToggleSidebar", "NewWorkspace", "FilterWorkspaces" };

/// @param control A control.
/// @return The name of the action it runs (SidebarControlCommands).
[[nodiscard]] constexpr std::string_view sidebarControlCommand(SidebarControl control) noexcept
{
    return SidebarControlCommands[static_cast<size_t>(control)];
}

/// @param control A control.
/// @return The name QML reads it by: "toggle", "newWorkspace" or "filter".
[[nodiscard]] constexpr std::string_view sidebarControlName(SidebarControl control) noexcept
{
    switch (control)
    {
        case SidebarControl::Toggle: return "toggle";
        case SidebarControl::NewWorkspace: return "newWorkspace";
        case SidebarControl::Filter: return "filter";
    }
    return "toggle";
}

} // namespace contour::window
