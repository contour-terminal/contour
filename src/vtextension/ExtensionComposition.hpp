// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>

#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/Instance.hpp>

namespace vtextension
{

/// The identity an installed extension's grants are recorded under: chosen by the host, never by the module
/// itself, so that a module cannot claim another one's grant by naming itself after it.
/// @param configKey The extension's key under `extensions:`.
/// @param canonicalModule The canonical path of its module.
/// @return `<config key>@<canonical module path>`.
[[nodiscard]] std::string extensionHostId(std::string_view configKey,
                                          std::filesystem::path const& canonicalModule);

/// One status-line extension as a configuration declares it, in no configuration's own types: what the GUI
/// and the daemon each build from theirs.
struct DeclaredExtension
{
    std::string name;           ///< The key under `extensions:`; what `{Extension:<name>}` refers to.
    std::filesystem::path path; ///< The module on disk; empty names a bundled extension, found by @ref name.
    ConfigMap config;           ///< Values the extension reads through its host `config` call.
    CommandPermission runCommand = CommandPermission::Deny; ///< What the user allows its commands.

    [[nodiscard]] bool operator==(DeclaredExtension const&) const = default;
};

/// The status-line extensions a profile declares: what a change of, on a configuration reload, reloads them.
struct DeclaredExtensions
{
    std::vector<DeclaredExtension> extensions; ///< In the configuration's order.
    std::optional<std::string> takeOver;       ///< `status_line.extension`.

    [[nodiscard]] bool operator==(DeclaredExtensions const&) const = default;
};

/// @param declared A profile's extensions.
/// @return The diagnostic for a `status_line.extension` that names none of them; none if it is unset or does.
[[nodiscard]] std::optional<std::string> undeclaredTakeOver(DeclaredExtensions const& declared);

/// Where the modules of extensions are found.
struct ExtensionModuleLookup
{
    core::platform::FileSystem const& fileSystem;              ///< Where modules are read.
    std::span<std::filesystem::path const> bundledDirectories; ///< Where a name-only entry is looked for.
};

/// Why an extension has no module: what its notice says, and what the diagnostic says.
struct ExtensionModuleProblem
{
    ExtensionError error; ///< ModuleUnavailable or ModuleTooLarge.
    std::string reason;   ///< Why, for the configuration's log.
};

/// Finds a declared extension's module.
/// @param name What the configuration calls the extension: a bundled one's name.
/// @param path The module the configuration names; empty for the bundled one called @p name.
/// @param lookup Where modules are found.
/// @return @p path if given, else the bundled module; or why there is none.
[[nodiscard]] std::expected<std::filesystem::path, ExtensionModuleProblem> findExtensionModule(
    std::string_view name, std::filesystem::path const& path, ExtensionModuleLookup const& lookup);

/// Reads a module. Its size is checked before it is read, so that a huge file costs no memory, and after, for
/// one that grew meanwhile.
/// @param path The module.
/// @param fileSystem Where it is read.
/// @param limit The most bytes it may have.
/// @return Its bytes; or why they could not be read.
[[nodiscard]] std::expected<std::vector<std::byte>, ExtensionModuleProblem> readExtensionModule(
    std::filesystem::path const& path, core::platform::FileSystem const& fileSystem, std::size_t limit);

/// What one pane's extensions are, and what was wrong with the configuration that declared them.
struct ComposedExtensions
{
    ExtensionHostConfig host;             ///< What the pane runs.
    std::vector<std::string> diagnostics; ///< One line per problem, for the configuration's log.
};

/// Turns what a profile declares into what a pane runs: reads each module, assigns its host id, and places
/// the one `status_line.extension` names as the take-over.
///
/// A module that is not found (a bundled name not installed, a path that does not exist or cannot be read)
/// is passed on without bytes, so that its place shows the extension's disabled notice; it is diagnosed too.
/// A module whose manifest places it in the daemon world is diagnosed, naming `daemon.extensions.<name>`, and
/// passed on without bytes too (WorldMismatch). A take-over naming no declared extension is diagnosed, and
/// nothing takes the status line over.
/// @param declared The profile's extensions.
/// @param lookup Where modules are found.
/// @param cellPixelSize One cell of the status line, in pixels; empty while unknown.
/// @param limits What each extension may cost.
/// @return What the pane runs, and the diagnostics.
[[nodiscard]] ComposedExtensions composeExtensions(DeclaredExtensions const& declared,
                                                   ExtensionModuleLookup const& lookup,
                                                   vtbackend::ImageSize cellPixelSize,
                                                   ExtensionLimits limits);

} // namespace vtextension
