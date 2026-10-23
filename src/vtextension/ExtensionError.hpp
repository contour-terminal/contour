// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <utility>

namespace vtextension
{

/// Why an extension could not be loaded, called, or allowed to do what it asked.
enum class ExtensionError : uint8_t
{
    RuntimeUnavailable, ///< The WebAssembly runtime could not be initialized.
    InvalidComponent,   ///< The module failed to load or to instantiate.
    WorldMismatch,      ///< The module does not export or import what the extension world requires.
    Trapped,            ///< The guest trapped (unreachable, out-of-bounds access, ...).
    DeadlineExceeded,   ///< The call ran past its deadline and was terminated.
    OutOfMemory,        ///< The guest asked for more linear memory than its cap allows.
    ManifestMissing,    ///< The module embeds no extension manifest.
    ManifestInvalid,    ///< The embedded manifest could not be parsed.
    PermissionPending,  ///< A permission the call needs has not been answered yet.
    PermissionDenied,   ///< A permission the call needs was refused.
    CommandNotDeclared, ///< The guest ran a command its manifest does not declare.
    OutputTooLarge,     ///< A render produced more bytes than the limit allows.
    ArgumentRejected,   ///< A command argument could be taken for an option, or holds a NUL.
    CommandFailed,      ///< A command could not be started, or was stopped before it finished.
    ModuleUnavailable,  ///< The host found no module to load: not installed, or not readable.
    ModuleTooLarge,     ///< The module file is larger than ExtensionLimits::maxModuleBytes.
};

/// One row of the error-description table.
struct ExtensionErrorText
{
    ExtensionError error;  ///< The error described.
    std::string_view text; ///< What a diagnostic says for it.
};

/// What a diagnostic says for each error, indexed by the enumerator's value.
inline constexpr auto ExtensionErrorTexts = std::array {
    ExtensionErrorText { .error = ExtensionError::RuntimeUnavailable,
                         .text = "WebAssembly runtime unavailable" },
    ExtensionErrorText { .error = ExtensionError::InvalidComponent, .text = "invalid WebAssembly module" },
    ExtensionErrorText { .error = ExtensionError::WorldMismatch,
                         .text = "module does not match the extension world" },
    ExtensionErrorText { .error = ExtensionError::Trapped, .text = "extension trapped" },
    ExtensionErrorText { .error = ExtensionError::DeadlineExceeded,
                         .text = "extension call exceeded its deadline" },
    ExtensionErrorText { .error = ExtensionError::OutOfMemory,
                         .text = "extension exceeded its memory limit" },
    ExtensionErrorText { .error = ExtensionError::ManifestMissing, .text = "extension manifest missing" },
    ExtensionErrorText { .error = ExtensionError::ManifestInvalid, .text = "extension manifest invalid" },
    ExtensionErrorText { .error = ExtensionError::PermissionPending, .text = "extension permission pending" },
    ExtensionErrorText { .error = ExtensionError::PermissionDenied, .text = "extension permission denied" },
    ExtensionErrorText { .error = ExtensionError::CommandNotDeclared,
                         .text = "command not declared in the extension manifest" },
    ExtensionErrorText { .error = ExtensionError::OutputTooLarge, .text = "extension output too large" },
    ExtensionErrorText { .error = ExtensionError::ArgumentRejected,
                         .text = "command argument rejected: it could be taken for an option" },
    ExtensionErrorText { .error = ExtensionError::CommandFailed, .text = "command could not be run" },
    ExtensionErrorText { .error = ExtensionError::ModuleUnavailable, .text = "extension module not found" },
    ExtensionErrorText { .error = ExtensionError::ModuleTooLarge, .text = "extension module too large" },
};

static_assert(ExtensionErrorTexts.size() == std::to_underlying(ExtensionError::ModuleTooLarge) + 1u,
              "every ExtensionError has a row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ExtensionErrorTexts.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(ExtensionErrorTexts.at(index).error) == index;
                                  }),
              "ExtensionErrorTexts is indexed by the enumerator's value");

/// Describes an extension error for diagnostics.
/// @param error The error to describe.
/// @return A short, human-readable description.
[[nodiscard]] constexpr std::string_view errorText(ExtensionError error) noexcept
{
    return ExtensionErrorTexts.at(std::to_underlying(error)).text;
}

} // namespace vtextension
