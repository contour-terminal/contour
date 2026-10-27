// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The shells Contour offers to integrate with, and where each keeps its startup file, as one table.
/// Qt-free: the editor that writes those files and the offer that asks to are both built on it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
};

/// The directory a shell's startup file is named relative to.
enum class RcBase : uint8_t
{
    Home = 0, ///< `$HOME`.
    ZDotDir,  ///< `$ZDOTDIR`, else `$HOME`.
    XdgConfig ///< `$XDG_CONFIG_HOME`, else `$HOME/.config`.
};

/// One shell's row: its name, syntax and the startup file Contour appends to.
struct ShellRow
{
    ShellKind kind = ShellKind::Unknown;
    std::string_view name {}; ///< As `contour generate integration shell <name>` spells it.
    RcSyntax syntax = RcSyntax::Posix;
    std::string_view guard {}; ///< Opens the appended line: runs the rest only in a shell Contour started.
    std::string_view continuation {}; ///< What follows the guard in Contour's line, up to the quoted path.
    RcBase base = RcBase::Home;
    std::string_view rcFile {};      ///< Relative to @c base.
    std::string_view rcFileMacOS {}; ///< Relative to @c base, on macOS.
};

/// Every shell, in enumerator order from ShellKind::Bash.
inline constexpr auto ShellRows = std::array {
    ShellRow { .kind = ShellKind::Bash,
               .name = "bash",
               .syntax = RcSyntax::Posix,
               .guard = R"(if [ "$TERM_PROGRAM" = contour ]; then )",
               .continuation = "eval \"$('",
               .base = RcBase::Home,
               .rcFile = ".bashrc",
               .rcFileMacOS = ".bash_profile" },
    ShellRow { .kind = ShellKind::Zsh,
               .name = "zsh",
               .syntax = RcSyntax::Posix,
               .guard = R"(if [ "$TERM_PROGRAM" = contour ]; then )",
               .continuation = "eval \"$('",
               .base = RcBase::ZDotDir,
               .rcFile = ".zshrc",
               .rcFileMacOS = ".zshrc" },
    ShellRow { .kind = ShellKind::Fish,
               .name = "fish",
               .syntax = RcSyntax::Fish,
               .guard = R"(if test "$TERM_PROGRAM" = contour; )",
               .continuation = "'",
               .base = RcBase::XdgConfig,
               .rcFile = "fish/config.fish",
               .rcFileMacOS = "fish/config.fish" },
};

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

/// Names the shell a foreground process is.
/// @param processName The process name or path; the part after the last `/` counts, and one leading
///        `-` (a login shell) is dropped.
/// @return The shell whose name matches exactly, else ShellKind::Unknown.
[[nodiscard]] constexpr ShellKind shellKindOf(std::string_view processName) noexcept
{
    if (auto const slash = processName.find_last_of('/'); slash != std::string_view::npos)
        processName.remove_prefix(slash + 1);
    if (processName.starts_with('-'))
        processName.remove_prefix(1);
    auto const row = std::ranges::find(ShellRows, processName, &ShellRow::name);
    return row == ShellRows.end() ? ShellKind::Unknown : row->kind;
}

} // namespace contour::config
