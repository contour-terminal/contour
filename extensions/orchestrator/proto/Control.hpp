// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator::proto
{

/// Why a control call failed: `contour`'s own message, or what became of the process.
struct ControlFailure
{
    std::string why; ///< One line or more, untrusted.
};

/// Contour's control API, reached through `contour api call` against one explicitly named daemon, with every
/// `CONTOUR_*` variable unset: it never finds the daemon or pane the driver itself runs in.
class ControlApi
{
  public:
    /// @param contour The `contour` program.
    /// @param daemon The daemon's address, as `contour api call daemon <address>` takes it.
    /// @param environment The environment calls run under; it holds no `CONTOUR_*` variable.
    /// @param cwd The directory calls run in.
    ControlApi(std::string contour,
               std::string daemon,
               std::vector<std::string> environment,
               std::string cwd);

    /// Opens a tab in the current workspace, keeping the user's focus where it is.
    /// @param cwd Its working directory.
    /// @param label Its label.
    /// @param command The program it runs, with its arguments.
    /// @return The new pane's control-API id.
    [[nodiscard]] std::expected<std::string, ControlFailure> openTab(std::string const& cwd,
                                                                     std::string const& label,
                                                                     std::vector<std::string> command) const;
    /// Closes a pane.
    /// @param controlPane Its control-API id.
    [[nodiscard]] std::expected<void, ControlFailure> closePane(std::string const& controlPane) const;
    /// @return The `workspace.list` reply, as JSON.
    [[nodiscard]] std::expected<std::string, ControlFailure> listWorkspaces() const;

  private:
    [[nodiscard]] std::expected<std::string, ControlFailure> call(std::string_view method,
                                                                  std::string request) const;

    std::string _contour;
    std::string _daemon;
    std::vector<std::string> _environment;
    std::string _cwd;
};

} // namespace orchestrator::proto
