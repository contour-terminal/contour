// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __APPLE__

    #include <contour/command/MenuTree.hpp>

    #include <cstdint>
    #include <string>
    #include <vector>

namespace contour::platform
{

/// Whether a native menu item carries an image, which only a swatch row does.
enum class SwatchImage : uint8_t
{
    Absent = 0, ///< No image.
    Present,    ///< The swatch colour, drawn as a square.
};

/// One NSMenuItem as built for a MenuNode, read back with no Objective-C type in sight.
struct NativeMenuOutline
{
    /// Whether the item is a separator, has a submenu, or is a command.
    command::ContextMenuEntryKind kind = command::ContextMenuEntryKind::Command;
    std::string title {};                                                      ///< The item's title.
    command::RowAvailability availability = command::RowAvailability::Enabled; ///< The item's `enabled`.
    /// The item's `state`: Checked when on, otherwise None, since an NSMenuItem cannot tell an unticked
    /// check column from no check column.
    command::RowCheck check = command::RowCheck::None;
    SwatchImage image = SwatchImage::Absent; ///< Whether the item has an image.
    long tag = -1;                           ///< The item's `tag`: the action index, or -1 for non-commands.
    std::vector<NativeMenuOutline> children {}; ///< The submenu's items.
    [[nodiscard]] bool operator==(NativeMenuOutline const&) const = default;
};

/// Builds the NSMenu exactly as the Cocoa presenter's present() does, without showing it, and reads each
/// NSMenuItem back.
/// @pre Called on the GUI (main) thread.
/// @param tree The menu to build.
/// @return The built menu's items, top to bottom.
[[nodiscard]] std::vector<NativeMenuOutline> outlineNativeMenu(command::MenuTree const& tree);

} // namespace contour::platform

#endif
