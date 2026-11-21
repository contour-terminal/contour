// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ExtensionDeclarations.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace contour::config
{

namespace
{
    /// One row of the configuration-to-runtime permission table.
    struct PermissionMapping
    {
        Permission configured;
        vtextension::CommandPermission runtime;
    };

    /// One row of the configuration-to-runtime autostart table.
    struct AutostartMapping
    {
        DaemonExtensionAutostart configured;
        vtextension::ExtensionAutostart runtime;
    };

    constexpr auto AutostartMappings = std::array {
        AutostartMapping { .configured = DaemonExtensionAutostart::OnDemand,
                           .runtime = vtextension::ExtensionAutostart::OnDemand },
        AutostartMapping { .configured = DaemonExtensionAutostart::AtDaemonStart,
                           .runtime = vtextension::ExtensionAutostart::AtDaemonStart },
    };

    constexpr auto PermissionMappings = std::array {
        PermissionMapping { .configured = Permission::Deny, .runtime = vtextension::CommandPermission::Deny },
        PermissionMapping { .configured = Permission::Allow,
                            .runtime = vtextension::CommandPermission::Allow },
        PermissionMapping { .configured = Permission::Ask, .runtime = vtextension::CommandPermission::Ask },
    };
} // namespace

vtextension::CommandPermission toCommandPermission(Permission permission) noexcept
{
    auto const mapping = std::ranges::find(PermissionMappings, permission, &PermissionMapping::configured);
    // Unreachable for a valid Permission; Deny is the safe answer for anything else.
    return mapping != PermissionMappings.end() ? mapping->runtime : vtextension::CommandPermission::Deny;
}

vtextension::DeclaredExtension declaredExtensionOf(ExtensionConfig const& extension)
{
    return vtextension::DeclaredExtension {
        .name = extension.name,
        .path = extension.path,
        .config = vtextension::ConfigMap { extension.config.begin(), extension.config.end() },
        .runCommand = toCommandPermission(extension.runCommand),
    };
}

vtextension::DeclaredExtensions declaredExtensionsOf(std::span<ExtensionConfig const> extensions,
                                                     std::optional<std::string> takeOver)
{
    auto declared = vtextension::DeclaredExtensions { .extensions = {}, .takeOver = std::move(takeOver) };
    declared.extensions.reserve(extensions.size());
    for (auto const& extension: extensions)
        declared.extensions.push_back(declaredExtensionOf(extension));
    return declared;
}

vtextension::DeclaredExtensions declaredExtensionsOf(TerminalProfile const& profile)
{
    return declaredExtensionsOf(profile.extensions.value(), profile.statusLine.value().extension);
}

vtextension::ExtensionAutostart toExtensionAutostart(DaemonExtensionAutostart autostart) noexcept
{
    auto const mapping = std::ranges::find(AutostartMappings, autostart, &AutostartMapping::configured);
    // Unreachable for a valid value; on demand is the safe answer for anything else.
    return mapping != AutostartMappings.end() ? mapping->runtime : vtextension::ExtensionAutostart::OnDemand;
}

vtextension::DaemonExtensionDeclaration daemonExtensionDeclarationOf(DaemonExtensionConfig const& extension,
                                                                     std::filesystem::path storeDirectory)
{
    return vtextension::DaemonExtensionDeclaration { .name = extension.name,
                                                     .modulePath = extension.path,
                                                     .autostart = toExtensionAutostart(extension.autostart),
                                                     .configJson = extension.configJson,
                                                     .storeDirectory = std::move(storeDirectory) };
}

} // namespace contour::config
