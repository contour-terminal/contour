// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The shells Contour offers to integrate with, and where each keeps its startup file, as one table.
/// Qt-free: the editor that writes those files and the offer that asks to are both built on it.

#include <contour/config/ConfigEnumInfo.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace contour::config
{

/// A shell the install offer knows.
enum class ShellKind : uint8_t
{
    Unknown = 0, ///< Not a shell Contour installs integration for.
    Bash,
    Zsh,
    Fish,
    Pwsh,              ///< PowerShell 7 and later.
    WindowsPowerShell, ///< Windows PowerShell 5.1, which ships with Windows.
};

/// Whether a shell's startup file already loads Contour's integration.
enum class RcState : uint8_t
{
    Unknown = 0, ///< Not inspected, or not inspectable.
    Absent,      ///< The file does not load it.
    Installed,   ///< The file loads it.
    Partial,     ///< The file ends in a cut-short copy of Contour's line, which breaks the shell.
};

/// The language a shell's startup file is written in, which decides how the line is quoted.
enum class RcSyntax : uint8_t
{
    Posix = 0, ///< bash and zsh.
    Fish,
    PowerShell, ///< pwsh and Windows PowerShell.
};

/// The directory a shell's startup file is named relative to.
enum class RcBase : uint8_t
{
    Home = 0,  ///< `$HOME`.
    ZDotDir,   ///< `$ZDOTDIR`, else `$HOME`.
    XdgConfig, ///< `$XDG_CONFIG_HOME`, else `$HOME/.config`.
    Documents, ///< The Windows Documents known folder.
};

/// Which hosts a shell is offered on.
enum class ShellHosts : uint8_t
{
    PosixOnly = 0, ///< Listed everywhere, but only installable off Windows.
    Everywhere,    ///< Listed and installable on every host.
    WindowsOnly,   ///< Listed and installable on Windows only.
};

/// How a shell reads a startup file that has no byte-order mark.
enum class BomlessText : uint8_t
{
    Utf8 = 0,     ///< As UTF-8.
    AnsiCodePage, ///< As the system's ANSI code page.
};

/// What a startup file Contour creates begins with.
enum class NewRcFile : uint8_t
{
    Plain = 0, ///< Nothing before the appended line.
    Utf8Bom,   ///< A UTF-8 byte-order mark.
};

/// Where a shell keeps its startup file on one host.
struct RcLocation
{
    RcBase base = RcBase::Home;
    std::string_view file {}; ///< Relative to @c base; empty when the shell has none on that host.
};

/// One shell's row: its name, syntax and the startup file Contour appends to.
struct ShellRow
{
    ShellKind kind = ShellKind::Unknown;
    std::string_view name {};        ///< As `contour generate integration shell <name>` spells it.
    std::string_view displayName {}; ///< As the user reads it.
    std::string_view script {};      ///< The integration script the shell is given.
    RcSyntax syntax = RcSyntax::Posix;
    std::string_view guard {}; ///< Opens the appended line: runs the rest only in a shell Contour started.
    std::string_view continuation {}; ///< What follows the guard in Contour's line, up to the quoted path.
    ShellHosts hosts = ShellHosts::PosixOnly;
    RcLocation posix {};   ///< On Linux and other POSIX hosts.
    RcLocation macOS {};   ///< On macOS.
    RcLocation windows {}; ///< On Windows.
    BomlessText bomless = BomlessText::Utf8;
    NewRcFile newFile = NewRcFile::Plain;
};

/// Every shell, in enumerator order from ShellKind::Bash.
inline constexpr auto ShellRows = std::array {
    ShellRow { .kind = ShellKind::Bash,
               .name = "bash",
               .displayName = "bash",
               .script = "bash",
               .syntax = RcSyntax::Posix,
               .guard = R"(if [ "$TERM_PROGRAM" = contour ]; then )",
               .continuation = "eval \"$('",
               .hosts = ShellHosts::PosixOnly,
               .posix = { .base = RcBase::Home, .file = ".bashrc" },
               .macOS = { .base = RcBase::Home, .file = ".bash_profile" },
               .windows = {},
               .bomless = BomlessText::Utf8,
               .newFile = NewRcFile::Plain },
    ShellRow { .kind = ShellKind::Zsh,
               .name = "zsh",
               .displayName = "zsh",
               .script = "zsh",
               .syntax = RcSyntax::Posix,
               .guard = R"(if [ "$TERM_PROGRAM" = contour ]; then )",
               .continuation = "eval \"$('",
               .hosts = ShellHosts::PosixOnly,
               .posix = { .base = RcBase::ZDotDir, .file = ".zshrc" },
               .macOS = { .base = RcBase::ZDotDir, .file = ".zshrc" },
               .windows = {},
               .bomless = BomlessText::Utf8,
               .newFile = NewRcFile::Plain },
    ShellRow { .kind = ShellKind::Fish,
               .name = "fish",
               .displayName = "fish",
               .script = "fish",
               .syntax = RcSyntax::Fish,
               .guard = R"(if test "$TERM_PROGRAM" = contour; )",
               .continuation = "'",
               .hosts = ShellHosts::PosixOnly,
               .posix = { .base = RcBase::XdgConfig, .file = "fish/config.fish" },
               .macOS = { .base = RcBase::XdgConfig, .file = "fish/config.fish" },
               .windows = {},
               .bomless = BomlessText::Utf8,
               .newFile = NewRcFile::Plain },
    ShellRow {
        .kind = ShellKind::Pwsh,
        .name = "pwsh",
        .displayName = "PowerShell",
        .script = "pwsh",
        .syntax = RcSyntax::PowerShell,
        .guard = "if ($env:TERM_PROGRAM -eq 'contour') { ",
        .continuation = ". ([scriptblock]::Create((& '",
        .hosts = ShellHosts::Everywhere,
        .posix = { .base = RcBase::XdgConfig, .file = "powershell/Microsoft.PowerShell_profile.ps1" },
        .macOS = { .base = RcBase::XdgConfig, .file = "powershell/Microsoft.PowerShell_profile.ps1" },
        .windows = { .base = RcBase::Documents, .file = "PowerShell/Microsoft.PowerShell_profile.ps1" },
        .bomless = BomlessText::Utf8,
        .newFile = NewRcFile::Utf8Bom },
    ShellRow { .kind = ShellKind::WindowsPowerShell,
               .name = "powershell",
               .displayName = "Windows PowerShell",
               .script = "pwsh",
               .syntax = RcSyntax::PowerShell,
               .guard = "if ($env:TERM_PROGRAM -eq 'contour') { ",
               .continuation = ". ([scriptblock]::Create((& '",
               .hosts = ShellHosts::WindowsOnly,
               .posix = {},
               .macOS = {},
               .windows = { .base = RcBase::Documents,
                            .file = "WindowsPowerShell/Microsoft.PowerShell_profile.ps1" },
               .bomless = BomlessText::AnsiCodePage,
               .newFile = NewRcFile::Utf8Bom },
};

/// Where @p row keeps its startup file on @p host.
/// @param row The shell's row.
/// @param host The host.
/// @return The location; its file is empty when the shell has none there.
[[nodiscard]] constexpr RcLocation const& rcLocationOn(ShellRow const& row, HostPlatform host) noexcept
{
    switch (host)
    {
        case HostPlatform::Windows: return row.windows;
        case HostPlatform::MacOS: return row.macOS;
        case HostPlatform::Other:
        case HostPlatform::KdePlasma: break;
    }
    return row.posix;
}

/// Whether the install list shows @p row on @p host.
/// @param row The shell's row.
/// @param host The host.
/// @return False for a Windows-only shell anywhere else.
[[nodiscard]] constexpr bool isListedOn(ShellRow const& row, HostPlatform host) noexcept
{
    return row.hosts != ShellHosts::WindowsOnly || host == HostPlatform::Windows;
}

/// Whether Contour can install integration for @p row on @p host.
/// @param row The shell's row.
/// @param host The host.
/// @return False for a POSIX-only shell on Windows, and for a Windows-only shell elsewhere.
[[nodiscard]] constexpr bool isInstallableOn(ShellRow const& row, HostPlatform host) noexcept
{
    switch (row.hosts)
    {
        case ShellHosts::PosixOnly: return host != HostPlatform::Windows;
        case ShellHosts::Everywhere: return true;
        case ShellHosts::WindowsOnly: return host == HostPlatform::Windows;
    }
    return false;
}

/// The comment that ends the line Contour appends, and that marks an rc file as already integrated.
inline constexpr std::string_view ShellRcMarker = "# Contour shell integration";

/// The row of a shell.
/// @param kind The shell.
/// @return Its row, or nullptr for ShellKind::Unknown.
[[nodiscard]] constexpr ShellRow const* shellRow(ShellKind kind) noexcept
{
    auto const index = static_cast<std::size_t>(kind);
    if (index == 0 || index > ShellRows.size())
        return nullptr;
    return &ShellRows[index - 1];
}

/// Whether @p path names a place from the root of the file system, which is what a startup file's directory
/// or the binary an rc line runs must do. Not `path::is_absolute()`: that also demands a drive on Windows,
/// where the POSIX-style paths these are spelled in (`/home/me`, `/usr/bin/contour`) have none, so the same
/// path would be accepted on one host and refused on another.
/// @param path The path.
/// @return Whether it starts at a root directory.
[[nodiscard]] inline bool isRootedPath(std::filesystem::path const& path)
{
    return path.has_root_directory();
}

/// Names the shell a foreground process is.
/// @param processName The process name or path; the part after the last `/` or `\` counts, one leading
///        `-` (a login shell) and a trailing `.exe` are dropped.
/// @return The shell whose name matches, ignoring ASCII case, else ShellKind::Unknown.
[[nodiscard]] constexpr ShellKind shellKindOf(std::string_view processName) noexcept
{
    constexpr auto Exe = std::string_view { ".exe" };
    if (auto const separator = processName.find_last_of("/\\"); separator != std::string_view::npos)
        processName.remove_prefix(separator + 1);
    if (processName.starts_with('-'))
        processName.remove_prefix(1);
    if (processName.size() > Exe.size()
        && detail::equalsIgnoreCase(processName.substr(processName.size() - Exe.size()), Exe))
        processName.remove_suffix(Exe.size());
    auto const row = std::ranges::find_if(ShellRows, [&](ShellRow const& candidate) {
        return detail::equalsIgnoreCase(candidate.name, processName);
    });
    return row == ShellRows.end() ? ShellKind::Unknown : row->kind;
}

} // namespace contour::config
