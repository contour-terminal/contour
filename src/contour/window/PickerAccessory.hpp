// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a picker row's trailing "open in new window" button looks like on each sidebar look: the switcher's
/// workspace rows wear it on the selected and the hovered row. Pure and Qt-free, read by the provider, the
/// QML and the tests alike, as the sidebar's own controls are (SidebarControls.hpp).
///
/// A look that stands in a bar of themed icons (ContourLinux) names a freedesktop icon and its bundled copy;
/// macOS and Fluent draw the bundled icon; the Terminal row draws a glyph in a cell. The face means nothing
/// else on its row: it is none of the sidebar's own controls.

#include <contour/config/ChromeTokenGroups.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// How a picker row's accessory button is drawn.
struct PickerAccessoryFace
{
    std::string_view iconName {};    ///< The freedesktop icon, resolved through the theme; empty for none.
    std::string_view bundledIcon {}; ///< The icon drawn where the theme lacks @c iconName; empty for a glyph.
    std::string_view glyph {};       ///< The glyph drawn where there is no icon.

    [[nodiscard]] bool operator==(PickerAccessoryFace const&) const = default;
};

/// The accessories of one sidebar look.
struct PickerAccessoryRow
{
    config::SidebarLook look = config::SidebarLook::SourceList; ///< The look the row describes.
    PickerAccessoryFace openInNewWindow {};                     ///< The row's "open in new window" button.
};

namespace detail
{
    inline constexpr auto WindowNewIcon = std::string_view { "qrc:/contour/icons/window-new-symbolic.svg" };
} // namespace detail

/// The accessories of every look, in SidebarLook order.
inline constexpr auto PickerAccessoryRows = std::array {
    PickerAccessoryRow {
        .look = config::SidebarLook::SourceList,
        .openInNewWindow = { .iconName = {}, .bundledIcon = detail::WindowNewIcon, .glyph = {} } },
    PickerAccessoryRow {
        .look = config::SidebarLook::NavigationPane,
        .openInNewWindow = { .iconName = {}, .bundledIcon = detail::WindowNewIcon, .glyph = {} } },
    PickerAccessoryRow { .look = config::SidebarLook::HeaderPane,
                         .openInNewWindow = { .iconName = "window-new-symbolic",
                                              .bundledIcon = detail::WindowNewIcon,
                                              .glyph = {} } },
    PickerAccessoryRow { .look = config::SidebarLook::CellTree,
                         .openInNewWindow = { .iconName = {}, .bundledIcon = {}, .glyph = "↗" } },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, PickerAccessoryRows.size()),
                                  [](std::size_t index) {
                                      return PickerAccessoryRows[index].look
                                             == static_cast<config::SidebarLook>(index);
                                  }),
              "PickerAccessoryRows lists every SidebarLook once, in enumerator order");

/// @param look A sidebar look.
/// @return How its picker rows draw the "open in new window" button.
[[nodiscard]] constexpr PickerAccessoryFace const& pickerAccessoryFace(config::SidebarLook look) noexcept
{
    return PickerAccessoryRows[static_cast<std::size_t>(look)].openInNewWindow;
}

} // namespace contour::window
