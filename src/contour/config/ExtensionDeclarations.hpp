// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>

#include <optional>
#include <span>
#include <string>

#include <vtextension/CommandPolicy.hpp>
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

} // namespace contour::config
