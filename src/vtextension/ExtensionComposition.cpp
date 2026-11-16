// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionComposition.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <utility>

#include <vtextension/BundledExtensions.hpp>
#include <vtextension/Manifest.hpp>

namespace vtextension
{

namespace
{
    [[nodiscard]] ExtensionModuleProblem tooLarge(std::filesystem::path const& path, std::size_t limit)
    {
        return ExtensionModuleProblem { .error = ExtensionError::ModuleTooLarge,
                                        .reason = std::format(
                                            "{} is larger than {} bytes", core::pathToUtf8(path), limit) };
    }

    /// @return Whether @p module embeds a manifest that places it in the daemon world.
    [[nodiscard]] bool isDaemonModule(std::span<std::byte const> module)
    {
        auto const manifest = parseManifest(module);
        return manifest && manifest->placement == ManifestPlacement::Daemon;
    }
} // namespace

std::expected<std::filesystem::path, ExtensionModuleProblem> findExtensionModule(
    std::string_view name, std::filesystem::path const& path, ExtensionModuleLookup const& lookup)
{
    if (!path.empty())
        return path;
    if (auto bundled = resolveBundledExtension(name, lookup.bundledDirectories, lookup.fileSystem))
        return std::move(*bundled);
    return std::unexpected(
        ExtensionModuleProblem { .error = ExtensionError::ModuleUnavailable,
                                 .reason = std::format("no bundled extension '{}' is installed", name) });
}

std::expected<std::vector<std::byte>, ExtensionModuleProblem> readExtensionModule(
    std::filesystem::path const& path, core::platform::FileSystem const& fileSystem, std::size_t limit)
{
    auto const unreadable = [&](std::string const& reason) {
        return ExtensionModuleProblem { .error = ExtensionError::ModuleUnavailable,
                                        .reason = std::format(
                                            "cannot read {}: {}", core::pathToUtf8(path), reason) };
    };
    if (auto const size = fileSystem.fileSize(path); !size)
        return std::unexpected(unreadable(size.error()));
    else if (*size > limit)
        return std::unexpected(tooLarge(path, limit));
    auto text = fileSystem.readFile(path);
    if (!text)
        return std::unexpected(unreadable(text.error()));
    if (text->size() > limit)
        return std::unexpected(tooLarge(path, limit));
    auto bytes = std::vector<std::byte>(text->size());
    std::ranges::transform(*text, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

std::string extensionHostId(std::string_view configKey, std::filesystem::path const& canonicalModule)
{
    return std::format("{}@{}", configKey, core::pathToUtf8(canonicalModule));
}

std::optional<std::string> undeclaredTakeOver(DeclaredExtensions const& declared)
{
    if (!declared.takeOver
        || std::ranges::find(declared.extensions, *declared.takeOver, &DeclaredExtension::name)
               != declared.extensions.end())
        return std::nullopt;
    return std::format("status_line.extension names '{}', which the profile's extensions do not declare; "
                       "nothing takes the status line over.",
                       *declared.takeOver);
}

ComposedExtensions composeExtensions(DeclaredExtensions const& declared,
                                     ExtensionModuleLookup const& lookup,
                                     vtbackend::ImageSize cellPixelSize,
                                     ExtensionLimits limits)
{
    auto composed = ComposedExtensions {};
    composed.host.cellPixelSize = cellPixelSize;
    composed.host.limits = limits;
    composed.host.extensions.reserve(declared.extensions.size());

    if (auto diagnostic = undeclaredTakeOver(declared))
        composed.diagnostics.push_back(std::move(*diagnostic));

    for (auto const& extension: declared.extensions)
    {
        auto spec = ExtensionSpec {};
        spec.name = extension.name;
        spec.config = extension.config;
        spec.placement = extension.name == declared.takeOver ? Placement::TakeOver : Placement::Segment;
        spec.runCommand = extension.runCommand;

        auto const path = findExtensionModule(extension.name, extension.path, lookup)
                              .transform([&](std::filesystem::path const& found) {
                                  return lookup.fileSystem.weaklyCanonical(found);
                              });
        // Without a module the host id names the configuration alone: no grant is ever asked for it.
        spec.hostId = extensionHostId(extension.name, path.value_or(std::filesystem::path {}));
        auto module = path.and_then([&](std::filesystem::path const& found) {
            return readExtensionModule(found, lookup.fileSystem, limits.maxModuleBytes);
        });
        if (module && isDaemonModule(*module))
        {
            // Passed on without bytes, as a module that is not found is: its place shows the disabled notice.
            spec.moduleError = ExtensionError::WorldMismatch;
            composed.diagnostics.push_back(
                std::format("Extension '{0}' is disabled: `{0}` is a daemon extension: configure it under "
                            "`daemon.extensions.{0}`.",
                            extension.name));
        }
        else if (module)
            spec.module = std::move(*module);
        else
        {
            spec.moduleError = module.error().error;
            composed.diagnostics.push_back(
                std::format("Extension '{}' is disabled: {}.", extension.name, module.error().reason));
        }
        composed.host.extensions.push_back(std::move(spec));
    }
    return composed;
}

} // namespace vtextension
