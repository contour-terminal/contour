// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

// The Win32 path rules a directory handle obeys. Win32 addresses some names by spellings other than
// their own (device names such as CON, the stream syntax after a colon), and a path longer than MAX_PATH
// needs the verbatim prefix. These are pure, and compiled on every platform, so they are tested everywhere.

namespace vtpty
{

/// @param name One path component, UTF-8.
/// @return Whether Win32 addresses @p name by exactly this spelling, as one component of an ordinary
///         directory: not empty, not `.` or `..`; none of `<>:"/\|?*` or U+0000..U+001F; not ending in `.`
///         or ` `; its stem (up to the first `.`, trailing spaces dropped) not a reserved device name,
///         compared ASCII-case-insensitively.
[[nodiscard]] bool isPlainWindowsName(std::string_view name) noexcept;

/// @param finalPath A path as GetFinalPathNameByHandleW(VOLUME_NAME_DOS) spells it, UTF-8.
/// @return It without the verbatim prefix: `\\?\C:\a` → `C:\a`, `\\?\UNC\s\sh\a` → `\\s\sh\a`; anything
///         else unchanged.
[[nodiscard]] std::string dosPathOf(std::string_view finalPath);

/// @param fullPath An absolute path as GetFullPathNameW spells it, UTF-8.
/// @return It with the verbatim prefix, so no MAX_PATH limit applies: `C:\a` → `\\?\C:\a`,
///         `\\s\sh\a` → `\\?\UNC\s\sh\a`; a path already starting `\\?\` or `\\.\` unchanged.
[[nodiscard]] std::string verbatimPathOf(std::string_view fullPath);

} // namespace vtpty
