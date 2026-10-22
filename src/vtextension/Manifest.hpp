// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/ExtensionError.hpp>

namespace vtextension
{

/// The name of the WebAssembly custom section that carries an extension's manifest.
inline constexpr std::string_view ManifestSectionName = "terminal-extension-manifest";

/// One command an extension declares it may run.
struct DeclaredCommand
{
    /// The name the guest passes to `run-command`.
    std::string name;

    /// The command line as separate arguments. An element of the form `{placeholder}` is filled in
    /// from the call's arguments; no other element is ever changed, and no shell is involved.
    std::vector<std::string> argvTemplate;
};

/// What an extension declares about itself, read from its module before any of its code runs.
struct Manifest
{
    /// The extension's name; the key its grants are recorded under.
    std::string name;

    /// The manifest format revision the extension was written against.
    uint32_t version = 0;

    /// The commands the extension may ask the host to run.
    std::vector<DeclaredCommand> commands;

    /// SHA-256 of the manifest section's bytes, in lower-case hex. A grant is bound to it: an
    /// extension whose manifest changes needs a new grant.
    std::string digest;
};

/// Reads the name of the placeholder an argv template element stands for.
/// @param element One element of a command's argv template.
/// @return The name between the braces if the element is exactly `{name}` with a non-empty name that
///         contains no braces; `std::nullopt` for every other element.
[[nodiscard]] std::optional<std::string_view> placeholderName(std::string_view element) noexcept;

/// Reads the manifest out of a WebAssembly core module.
///
/// The module is scanned section by section for the custom section `terminal-extension-manifest`;
/// no code in it is run. Every length is checked against @p module, so truncated or malformed input
/// is an error and never an out-of-bounds read.
/// @param module The bytes of the core module.
/// @return The manifest; `ManifestMissing` if the module has no manifest section, `ManifestInvalid`
///         if the module cannot be walked, or the section's text is not a valid manifest (unknown
///         line, missing or repeated `name`/`version`, duplicate command, an empty command, or a
///         placeholder that is not a whole argv element).
[[nodiscard]] std::expected<Manifest, ExtensionError> parseManifest(std::span<std::byte const> module);

/// Whether a WebAssembly core module declares a start function, which the runtime would run while
/// instantiating it, before any deadline can guard it. No code in the module is run.
/// @param module The bytes of the core module.
/// @return Whether the module can be walked and has a start section.
[[nodiscard]] bool hasStartFunction(std::span<std::byte const> module) noexcept;

} // namespace vtextension
