// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ShellIntegration.hpp>

#include <crispy/utils.hpp>

#include <algorithm>
#include <ranges>
#include <string>

#include <ShellIntegrationData.hpp>

namespace contour::cli
{

std::span<ShellIntegrationRow const> supportedShells() noexcept
{
    return detail::ShellIntegrationTable;
}

std::expected<std::string_view, ShellIntegrationError> shellIntegrationScript(std::string_view shell)
{
    auto const shells = supportedShells();
    auto const row = std::ranges::find(shells, shell, &ShellIntegrationRow::name);
    if (row == shells.end())
        return std::unexpected { ShellIntegrationError::UnsupportedShell };

    return row->script;
}

std::string_view supportedShellsText()
{
    // Function-local static, because the one caller that most needs this is a CLI help string, and
    // crispy::cli::option::helpText is a string_view that borrows rather than owns. The list is the
    // same for every call, so building it once is also what it wants.
    static std::string const text =
        crispy::joinHumanReadable(supportedShells() | std::views::transform(&ShellIntegrationRow::name));

    return text;
}

} // namespace contour::cli
