// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The menu the OS offers beside the application's icon -- the macOS Dock menu, the Windows jump list -- as a
/// list of ways to open a window on a chosen target. A thin adapter: command::launcherEntries() decides what
/// the list is, this only shows it and runs what is picked.

#include <contour/command/LauncherEntries.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace contour::platform
{

/// Runs a launcher entry the application itself is asked to run, where the menu runs entries through it
/// (the Dock menu); a jump list's tasks are run by the shell, which needs no callback.
using LauncherRun = std::function<void(command::LauncherEntry const&)>;

/// The launcher menu of this OS.
class LauncherMenu
{
  public:
    virtual ~LauncherMenu() = default;

    LauncherMenu() = default;
    LauncherMenu(LauncherMenu const&) = delete;
    LauncherMenu& operator=(LauncherMenu const&) = delete;
    LauncherMenu(LauncherMenu&&) = delete;
    LauncherMenu& operator=(LauncherMenu&&) = delete;

    /// @return Which surface this is, which decides the entries it is given.
    [[nodiscard]] virtual command::LauncherSurface surface() const noexcept = 0;

    /// Replaces what the menu lists.
    /// @param entries The entries, in order.
    virtual void set(std::span<command::LauncherEntry const> entries) = 0;
};

/// @param programPath The executable a jump-list task runs.
/// @param run Runs a Dock menu entry through the application.
/// @return The OS's launcher menu; null where there is none (Linux has desktop actions, which are files).
[[nodiscard]] std::unique_ptr<LauncherMenu> makeLauncherMenu(std::string const& programPath,
                                                             LauncherRun const& run);

} // namespace contour::platform
