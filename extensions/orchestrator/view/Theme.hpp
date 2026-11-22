// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>
#include <core/tui/Theme.hpp>

#include <cstdint>

namespace orchestrator::view
{

/// What a piece of the dashboard means, which decides how it is drawn.
enum class Role : uint8_t
{
    Text,      ///< Ordinary text.
    Dim,       ///< What explains rather than informs: headings, labels, the summary, a repository prefix.
    Warning,   ///< A problem line, a run that waits for you, a kept worktree.
    Failing,   ///< Failed checks, a failed run.
    Passing,   ///< Passing checks, an approved review, a removed worktree.
    Selection, ///< The selected row.
    Border,    ///< The frames of the panels.
    Accent,    ///< What stands out without warning: the user's own entries in a transcript.
    Note,      ///< A remark about the text rather than part of it: a transcript's note.
    Working,   ///< A run at work: its glyph, its group's heading.
    Focus,     ///< The border of the place that has the keys.
};

/// How many colours the dashboard's theme may use.
enum class ColorDepth : uint8_t
{
    Palette256 = 0, ///< The xterm 256-colour palette: what every terminal the dashboard is served in draws.
    TrueColor,      ///< 24-bit colour.
};

/// The dashboard's theme: core::tui's, with each role's style set from one table, the design's palette in
/// @p depth. Ordinary text and the background stay the terminal's own, so the dashboard stays legible on a
/// light colour scheme too; the roles that stand out (muted, faint, accent, the states) take the palette.
/// @param depth How many colours it may use.
/// @return The theme the dashboard is drawn with.
[[nodiscard]] core::tui::Theme dashboardTheme(ColorDepth depth = ColorDepth::Palette256);

/// @param theme A theme the dashboard is drawn with.
/// @param role What is drawn.
/// @return The style @p theme gives @p role.
[[nodiscard]] core::tui::Style const& styleOf(core::tui::Theme const& theme, Role role) noexcept;

} // namespace orchestrator::view
