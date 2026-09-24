// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>

namespace core::platform
{

/// @brief Returns the user's home directory.
///
/// Tries HOME (Unix), then USERPROFILE (Windows).
///
/// @param environment The environment to read the variables from: by default the process
///                    environment as it is now, and in a test a fake one.
/// @return The home directory path, or std::nullopt if neither variable is set.
[[nodiscard]] inline auto homeDirectory(core::Environment const& environment = core::LiveEnvironment {})
    -> std::optional<std::filesystem::path>
{
    if (auto const home = environment.get("HOME"))
        return std::filesystem::path(*home);
    if (auto const home = environment.get("USERPROFILE"))
        return std::filesystem::path(*home);
    return std::nullopt;
}

/// @brief Returns the current user's login name.
///
/// Tries USER, then LOGNAME (both POSIX), then USERNAME (Windows); an empty value counts as unset.
///
/// @param environment The environment to read the variables from: by default the process
///                    environment as it is now, as for homeDirectory().
/// @return The login name, or std::nullopt if none of the variables is set.
[[nodiscard]] inline auto userName(core::Environment const& environment = core::LiveEnvironment {})
    -> std::optional<std::string>
{
    for (auto const* const name: { "USER", "LOGNAME", "USERNAME" })
        if (auto value = environment.get(name); value && !value->empty())
            return value;
    return std::nullopt;
}

/// @brief Returns the user's configuration base directory.
///
/// On Unix: $XDG_CONFIG_HOME, or ~/.config if not set.
/// On Windows: $APPDATA (typically ~/AppData/Roaming).
///
/// @param environment The environment to read the variables from: by default the process
///                    environment as it is now, as for homeDirectory().
/// @return The configuration directory path, or std::nullopt if it cannot be determined.
[[nodiscard]] inline auto configHome(core::Environment const& environment = core::LiveEnvironment {})
    -> std::optional<std::filesystem::path>
{
    if (auto const xdg = environment.get("XDG_CONFIG_HOME"); xdg && !xdg->empty())
        return std::filesystem::path(*xdg);
    if (auto const appdata = environment.get("APPDATA"); appdata && !appdata->empty())
        return std::filesystem::path(*appdata);
    if (auto home = homeDirectory(environment))
        return *home / ".config";
    return std::nullopt;
}

} // namespace core::platform
