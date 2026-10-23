// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionComposition.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <utility>

#include <vtextension/BundledExtensions.hpp>

namespace vtextension
{

namespace
{
    /// Why an extension has no module: what its notice says, and what the diagnostic says.
    struct ModuleProblem
    {
        ExtensionError error;
        std::string reason;
    };

    /// Where an entry's module is, or why there is none.
    [[nodiscard]] std::expected<std::filesystem::path, ModuleProblem> modulePathOf(
        DeclaredExtension const& extension, ExtensionModuleLookup const& lookup)
    {
        if (!extension.path.empty())
            return extension.path;
        if (auto bundled =
                resolveBundledExtension(extension.name, lookup.bundledDirectories, lookup.fileSystem))
            return std::move(*bundled);
        return std::unexpected(ModuleProblem {
            .error = ExtensionError::ModuleUnavailable,
            .reason = std::format("no bundled extension '{}' is installed", extension.name) });
    }

    [[nodiscard]] ModuleProblem tooLarge(std::filesystem::path const& path, std::size_t limit)
    {
        return ModuleProblem { .error = ExtensionError::ModuleTooLarge,
                               .reason =
                                   std::format("{} is larger than {} bytes", core::pathToUtf8(path), limit) };
    }

    /// @return The bytes of the module at @p path, or why they could not be read. Its size is checked before
    ///         it is read, so that a huge file costs no memory, and after, for one that grew meanwhile.
    [[nodiscard]] std::expected<std::vector<std::byte>, ModuleProblem> readModule(
        std::filesystem::path const& path, core::platform::FileSystem const& fileSystem, std::size_t limit)
    {
        auto const unreadable = [&](std::string const& reason) {
            return ModuleProblem { .error = ExtensionError::ModuleUnavailable,
                                   .reason =
                                       std::format("cannot read {}: {}", core::pathToUtf8(path), reason) };
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
} // namespace

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

        auto const path = modulePathOf(extension, lookup).transform([&](std::filesystem::path const& found) {
            return lookup.fileSystem.weaklyCanonical(found);
        });
        // Without a module the host id names the configuration alone: no grant is ever asked for it.
        spec.hostId = extensionHostId(extension.name, path.value_or(std::filesystem::path {}));
        auto module = path.and_then([&](std::filesystem::path const& found) {
            return readModule(found, lookup.fileSystem, limits.maxModuleBytes);
        });
        if (module)
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
