// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Naming a program from the string a profile's `shell:` holds. Dependency-free, so the pane-activity
/// rule and the tab label's fallback read a shell's name the same way.

#include <string_view>

namespace contour::window
{

/// The part of @p path after its last separator, minus a leading '-' (a login shell's argv[0]).
/// @param path A program's path or name.
/// @return Its base name.
[[nodiscard]] inline std::string_view programBaseName(std::string_view path) noexcept
{
    if (auto const slash = path.find_last_of("/\\"); slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    if (path.starts_with('-'))
        path.remove_prefix(1);
    return path;
}

} // namespace contour::window
