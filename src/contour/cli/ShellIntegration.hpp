// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace contour::cli
{

/// One shell's integration script, as compiled into this binary.
struct ShellIntegrationRow
{
    std::string_view name {};   ///< Shell name, as `contour generate integration shell <name>` spells it.
    std::string_view script {}; ///< The script's bytes, verbatim.
};

/// Why a shell-integration script could not be produced.
enum class ShellIntegrationError : uint8_t
{
    UnsupportedShell = 0, ///< No script for the requested shell is compiled into this binary.
};

/// Every shell this binary carries an integration script for.
///
/// This table is the single source of truth for which shells are supported: the `generate
/// integration` dispatch and that verb's help text both derive from it, so a shell added to
/// CONTOUR_SHELL_INTEGRATION_SHELLS in CMake needs no accompanying edit in C++.
/// @return The compiled-in table, in the order the build list gives.
[[nodiscard]] std::span<ShellIntegrationRow const> supportedShells() noexcept;

/// Looks up the integration script for a shell.
/// @param shell Shell name to look up, matched exactly.
/// @return The script's bytes, or ShellIntegrationError::UnsupportedShell if none is compiled in.
[[nodiscard]] std::expected<std::string_view, ShellIntegrationError> shellIntegrationScript(
    std::string_view shell);

/// The supported shell names joined for a human-readable list, e.g. "bash, fish, tcsh, zsh".
/// @return A view of storage with process lifetime, so callers may hold it.
[[nodiscard]] std::string_view supportedShellsText();

/// A shell that marks its prompts and commands with OSC 133 by itself, so Contour ships no script for it.
struct NativeShellIntegrationRow
{
    std::string_view name {};        ///< Shell name, as a user would pass it to `generate integration shell`.
    std::string_view displayName {}; ///< How the shell names itself, e.g. "Nushell".
    std::string_view howToEnable {}; ///< One sentence on what turns its integration on.
};

/// Every shell that integrates natively. No name here is also in supportedShells(): a shell given a
/// script as well would mark every prompt twice.
/// @return The table.
[[nodiscard]] std::span<NativeShellIntegrationRow const> nativeShellIntegrations() noexcept;

/// The natively integrating shells for a human-readable list, e.g. "Nushell (nu)".
/// @return A view of storage with process lifetime, so callers may hold it.
[[nodiscard]] std::string_view nativeShellsText();

/// What `generate integration` tells a user who asked for a shell it carries no script for.
/// @param shell The shell name asked for.
/// @return For a natively integrating shell, how to turn its own integration on; otherwise which
///         shells are supported. Ends in a newline.
[[nodiscard]] std::string unsupportedShellMessage(std::string_view shell);

} // namespace contour::cli
