// SPDX-License-Identifier: Apache-2.0
#include <contour/ShellIntegration.h>

#include <algorithm>
#include <string>

#include <ShellIntegrationData.h>

namespace contour
{

std::span<ShellIntegrationRow const> supportedShells() noexcept
{
    return detail::ShellIntegrationTable;
}

std::expected<std::string_view, ShellIntegrationError> shellIntegrationScript(std::string_view shell)
{
    auto const row = std::ranges::find(detail::ShellIntegrationTable, shell, &ShellIntegrationRow::name);
    if (row == detail::ShellIntegrationTable.end())
        return std::unexpected { ShellIntegrationError::UnsupportedShell };

    return row->script;
}

std::string_view supportedShellsText()
{
    // Function-local static, because the one caller that most needs this is a CLI help string, and
    // crispy::cli::option::helpText is a string_view that borrows rather than owns. The list is the
    // same for every call, so building it once is also what it wants.
    static std::string const text = [] {
        auto result = std::string {};
        for (auto const& row: detail::ShellIntegrationTable)
        {
            if (!result.empty())
                result += ", ";
            result += row.name;
        }
        return result;
    }();

    return text;
}

} // namespace contour
