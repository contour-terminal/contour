// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ShellIntegration.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
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
    // core::cli::Option::helpText is a string_view that borrows rather than owns. The list is the
    // same for every call, so building it once is also what it wants.
    static std::string const text =
        core::joinHumanReadable(supportedShells() | std::views::transform(&ShellIntegrationRow::name));

    return text;
}

namespace
{
    /// Shells that speak OSC 133 by themselves.
    constexpr auto NativeShellIntegrationTable = std::array {
        NativeShellIntegrationRow {
            .name = "nu",
            .displayName = "Nushell",
            .howToEnable = "Nushell 0.111 or later marks its prompts and commands itself while "
                           "$env.config.shell_integration.osc133 is true, which is the default.",
        },
    };
} // namespace

std::span<NativeShellIntegrationRow const> nativeShellIntegrations() noexcept
{
    return NativeShellIntegrationTable;
}

std::string_view nativeShellsText()
{
    // Function-local static for the reason supportedShellsText() gives: a help string borrows.
    static std::string const text = core::joinHumanReadable(
        nativeShellIntegrations() | std::views::transform([](NativeShellIntegrationRow const& row) {
            return std::format("{} ({})", row.displayName, row.name);
        }));
    return text;
}

std::string unsupportedShellMessage(std::string_view shell)
{
    auto const natives = nativeShellIntegrations();
    if (auto const row = std::ranges::find(natives, shell, &NativeShellIntegrationRow::name);
        row != natives.end())
        return std::format(
            "{} needs no integration script from Contour. {}\n", row->displayName, row->howToEnable);

    return std::format(
        "Cannot generate shell integration for an unsupported shell, {}. Supported shells: {}.\n",
        shell,
        supportedShellsText());
}

} // namespace contour::cli
