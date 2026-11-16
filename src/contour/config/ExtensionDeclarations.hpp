// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/ExtensionAutostart.hpp>
#include <vtextension/ExtensionComposition.hpp>

namespace contour::config
{

/// @param permission What the configuration says about an extension's `run_command`.
/// @return The same, as the extension runtime takes it.
[[nodiscard]] vtextension::CommandPermission toCommandPermission(Permission permission) noexcept;

/// @param extension One entry of a profile's `extensions:`.
/// @return The same, as the extension runtime takes it.
[[nodiscard]] vtextension::DeclaredExtension declaredExtensionOf(ExtensionConfig const& extension);

/// @param extensions A profile's `extensions:`.
/// @param takeOver Its `status_line.extension`.
/// @return The status-line extensions they declare, as the extension runtime takes them.
[[nodiscard]] vtextension::DeclaredExtensions declaredExtensionsOf(
    std::span<ExtensionConfig const> extensions, std::optional<std::string> takeOver);

/// @param profile A profile.
/// @return The status-line extensions it declares, as the extension runtime takes them: what a GUI pane and
///         a daemon-hosted session each run.
[[nodiscard]] vtextension::DeclaredExtensions declaredExtensionsOf(TerminalProfile const& profile);

/// @param autostart What the configuration says about a daemon extension's `autostart`.
/// @return The same, as the extension runtime takes it.
[[nodiscard]] vtextension::ExtensionAutostart toExtensionAutostart(
    DaemonExtensionAutostart autostart) noexcept;

/// @param extension One entry of `daemon.extensions`.
/// @param storeDirectory Where its durable store is; empty when none can be named.
/// @return The same, as the daemon world's runtime takes it.
[[nodiscard]] vtextension::DaemonExtensionDeclaration daemonExtensionDeclarationOf(
    DaemonExtensionConfig const& extension, std::filesystem::path storeDirectory);

} // namespace contour::config
