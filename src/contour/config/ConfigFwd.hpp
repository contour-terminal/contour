// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

// The small enums many files name, apart from the Config struct in Config.hpp:
// a file that needs one of these must not parse the whole configuration model.

namespace contour::config
{

struct Config;

/// The unit Contour reports pixel sizes to applications in.
///
/// Applications learn the cell size by dividing a reported pixel extent by the grid
/// (`ws_ypixel / ws_row`, or a `CSI 14 t` reply over `CSI 18 t`) and size image canvases from it.
/// On a display with a content scale other than 1 the two answers differ by that scale, so this
/// decides how large a sixel an application draws into a given area.
enum class PixelReporting : uint8_t
{
    Logical, //!< Report logical pixels: what every other terminal reports.
    Device,  //!< Report device pixels: images land 1:1 on the display's own pixels (default).
};

/// Selects the light/dark appearance of the Qt GUI chrome (title bar, tab strip, command
/// palette, settings pages, dialogs) independently of the OS.
///
/// This affects the GUI elements only; the terminal grid keeps following the OS light/dark
/// preference through its per-profile color scheme. @c System defers to the operating
/// system's color scheme (the historical behavior); @c Terminal follows the terminal's own
/// background (the default).
/// @note Spelled in the configuration and in Settings by its ConfigEnum table, which the reader parses
///       case-insensitively.
enum class GuiTheme : uint8_t
{
    System,   //!< Follow the operating system's light/dark color scheme.
    Dark,     //!< Force a dark GUI appearance regardless of the OS.
    Light,    //!< Force a light GUI appearance regardless of the OS.
    Terminal, //!< Follow the luminance of the terminal's background (default).
};

namespace detail
{
    /// GuiTheme's rows, in the order Settings offers them: the terminal first (Phase 0 R7). The order is
    /// the table's own; the enumerator values are not.
    inline constexpr auto GuiThemeTable = std::array {
        ConfigEnumInfo<GuiTheme> { GuiTheme::Terminal, "terminal", "Match terminal" },
        ConfigEnumInfo<GuiTheme> { GuiTheme::System, "system", "Match system" },
        ConfigEnumInfo<GuiTheme> { GuiTheme::Dark, "dark", "Dark" },
        ConfigEnumInfo<GuiTheme> { GuiTheme::Light, "light", "Light" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<GuiTheme> const> configEnumValues() noexcept
{
    return detail::GuiThemeTable;
}

enum class Permission : uint8_t
{
    Deny,
    Allow,
    Ask
};

/// How a permission is spelled in the configuration file.
/// @param value The permission.
/// @return `allow`, `deny` or `ask`.
[[nodiscard]] constexpr std::string_view permissionToken(Permission value) noexcept
{
    switch (value)
    {
        case Permission::Allow: return "allow";
        case Permission::Deny: return "deny";
        case Permission::Ask: return "ask";
    }
    return "ask";
}

enum class SelectionAction : uint8_t
{
    Nothing,
    CopyToSelectionClipboard,
    CopyToClipboard,
};

/// Where a named profile or color scheme was defined, so the GUI can decide whether it may edit it.
///
/// contour.yml-defined and built-in entities are shown but read-only in the settings page (editing
/// them would mean the GUI silently shadowing the hand-maintained file); only @ref SideFile entities,
/// which the GUI itself created, are editable.
enum class SettingsOrigin : uint8_t
{
    Builtin,    //!< A built-in default (e.g. the "default" color palette shipped with Contour).
    MainConfig, //!< Defined inline in the hand-maintained contour.yml.
    SideFile,   //!< Stored in a GUI-managed side file (profiles/, colorschemes/ or hosts/<name>.yml).
};

/// Whether the loader could use the `settings.yml` beside `contour.yml`.
///
/// Recorded because a GUI that rewrites `settings.yml` from what the loader found would, over a file
/// that did not parse, replace everything in it with the few keys it knows.
enum class GuiSettingsFileHealth : uint8_t
{
    Usable = 0, //!< Absent, or parsed and applied.
    Unparsable, //!< Present, but not YAML the loader could read; nothing from it was applied.
};

/// Selects which Qt RHI graphics API drives the terminal display.
///
/// @c Auto lets Qt resolve the platform-native backend (Direct3D 11 on Windows, Metal on macOS,
/// OpenGL on Linux); the remaining values force a specific backend, and @c Software forces a
/// software-emulated OpenGL rasterizer. A backend that the running platform cannot provide
/// (e.g. Metal on Windows) falls back to @c Auto with a warning.
enum class RenderingBackend : uint8_t
{
    Auto,
    OpenGL,
    Vulkan,
    Direct3D11,
    Direct3D12,
    Metal,
    Software,
};

/// Whether the terminal hosts its sessions in a daemon process, and how hard it tries to.
enum class DaemonMode : uint8_t
{
    InProcess = 0, ///< Sessions live in the GUI process; no daemon is involved.
    Auto = 1,      ///< Use a daemon; when none can be reached, fall back to in-process with a notice.
    Required = 2,  ///< Use a daemon; when none can be reached, fail loudly.
};

/// The `daemon.mode` spellings, single-sourced: the reader, the writer and the diagnostic read this table.
constexpr auto DaemonModeNames = std::to_array<std::pair<std::string_view, DaemonMode>>({
    { "in_process", DaemonMode::InProcess },
    { "auto", DaemonMode::Auto },
    { "required", DaemonMode::Required },
});

/// @param mode The mode to name.
/// @return Its configuration spelling.
[[nodiscard]] constexpr std::string_view nameOf(DaemonMode mode) noexcept
{
    for (auto const& [name, value]: DaemonModeNames)
        if (value == mode)
            return name;
    return "in_process";
}

/// @param name A configuration spelling, as written by a user.
/// @return The mode it names, or nullopt if it names none.
[[nodiscard]] constexpr std::optional<DaemonMode> daemonModeFrom(std::string_view name) noexcept
{
    for (auto const& [candidate, value]: DaemonModeNames)
        if (candidate == name)
            return value;
    return std::nullopt;
}

} // namespace contour::config

// {{{ fmt custom formatters

template <>
struct std::formatter<contour::config::Permission>: formatter<std::string_view>
{
    auto format(contour::config::Permission value, auto& ctx) const
    {
        return formatter<string_view>::format(contour::config::permissionToken(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::SelectionAction>: formatter<std::string_view>
{
    auto format(contour::config::SelectionAction value, auto& ctx) const
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::SelectionAction::CopyToClipboard: name = "CopyToClipboard"; break;
            case contour::config::SelectionAction::CopyToSelectionClipboard:
                name = "CopyToSelectionClipboard";
                break;
            case contour::config::SelectionAction::Nothing: name = "Waiting"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::config::PixelReporting>: formatter<std::string_view>
{
    auto format(contour::config::PixelReporting value, auto& ctx) const
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::PixelReporting::Logical: name = "Logical"; break;
            case contour::config::PixelReporting::Device: name = "Device"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::config::RenderingBackend>: formatter<std::string_view>
{
    auto format(contour::config::RenderingBackend const& val, auto& ctx) const
    {
        std::string_view name;
        switch (val)
        {
            case contour::config::RenderingBackend::Auto: name = "auto"; break;
            case contour::config::RenderingBackend::OpenGL: name = "OpenGL"; break;
            case contour::config::RenderingBackend::Vulkan: name = "vulkan"; break;
            case contour::config::RenderingBackend::Direct3D11: name = "direct3d11"; break;
            case contour::config::RenderingBackend::Direct3D12: name = "direct3d12"; break;
            case contour::config::RenderingBackend::Metal: name = "metal"; break;
            case contour::config::RenderingBackend::Software: name = "software"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

/// Formats a daemon mode as its configuration spelling, so diagnostics quote what the user would write.
template <>
struct std::formatter<contour::config::DaemonMode>: std::formatter<std::string_view>
{
    auto format(contour::config::DaemonMode mode, auto& ctx) const
    {
        return std::formatter<std::string_view>::format(contour::config::nameOf(mode), ctx);
    }
};

// }}}
