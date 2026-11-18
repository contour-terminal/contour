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
};

/// The dashboard's theme: core::tui's, with each role's style set from one table. The colours are the
/// terminal's own (its default foreground and its palette's first eight), so the dashboard follows the
/// user's colour scheme rather than imposing one.
/// @return The theme the dashboard is drawn with by default.
[[nodiscard]] core::tui::Theme dashboardTheme();

/// @param theme A theme the dashboard is drawn with.
/// @param role What is drawn.
/// @return The style @p theme gives @p role.
[[nodiscard]] core::tui::Style const& styleOf(core::tui::Theme const& theme, Role role) noexcept;

} // namespace orchestrator::view
