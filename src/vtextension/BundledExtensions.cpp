// SPDX-License-Identifier: Apache-2.0
#include <vtextension/BundledExtensions.hpp>

#include <core/Utf8Path.hpp>

#include <ranges>
#include <string>

namespace vtextension
{

namespace
{
    /// What the XDG Base Directory specification says `XDG_DATA_DIRS` is when unset or empty.
    constexpr auto DefaultXdgDataDirs = std::string_view { "/usr/local/share:/usr/share" };

    constexpr auto XdgSeparator = ':';
    constexpr auto ModuleExtension = std::string_view { ".wasm" };

    /// The directory below an XDG data directory, and below Windows' `share`, that holds the modules.
    constexpr auto ExtensionsUnderData = std::string_view { "contour/extensions" };

    /// Whether @p name can be one directory entry's stem, rather than a path.
    [[nodiscard]] bool isPlainName(std::string_view name) noexcept
    {
        constexpr auto Separators = std::string_view { "/\\\0", 3 };
        return !name.empty() && name != "." && name != ".."
               && name.find_first_of(Separators) == std::string_view::npos;
    }

    /// The extensions directory of an installation laid out as `<prefix>/bin` and `<prefix>/share`.
    [[nodiscard]] std::filesystem::path besideExecutable(std::filesystem::path const& executableDirectory)
    {
        return (executableDirectory / ".." / "share" / ExtensionsUnderData).lexically_normal();
    }

    [[nodiscard]] std::vector<std::filesystem::path> xdgDirectories(core::Environment const& environment)
    {
        auto value = environment.get("XDG_DATA_DIRS").value_or(std::string {});
        if (value.empty())
            value = DefaultXdgDataDirs;

        auto directories = std::vector<std::filesystem::path> {};
        for (auto const part: value | std::views::split(XdgSeparator))
        {
            auto const entry = std::string_view { part.begin(), part.end() };
            // Relative entries are undefined by the specification, and would make the answer depend on the
            // working directory. Absolute is POSIX's rule, a leading `/`, not the host's: on Windows
            // path::is_absolute() wants a drive too, and would refuse every entry.
            if (!entry.starts_with('/'))
                continue;
            if (auto const path = core::pathFromUtf8(entry))
                directories.push_back(*path / ExtensionsUnderData);
        }
        return directories;
    }
} // namespace

std::vector<std::filesystem::path> bundledExtensionDirectories(
    BundledExtensionPlatform platform,
    core::Environment const& environment,
    std::filesystem::path const& executableDirectory)
{
    switch (platform)
    {
        case BundledExtensionPlatform::Linux: {
            // Last, and always there: a prefix XDG_DATA_DIRS does not list still finds its own extensions.
            auto directories = xdgDirectories(environment);
            directories.push_back(besideExecutable(executableDirectory));
            return directories;
        }
        case BundledExtensionPlatform::MacOS:
            // An app bundle's resources first; then beside the executable, which is where a GUI-less build,
            // installed like any Unix command-line tool, puts them.
            return { (executableDirectory / ".." / "Resources" / "extensions").lexically_normal(),
                     besideExecutable(executableDirectory) };
        case BundledExtensionPlatform::Windows: return { besideExecutable(executableDirectory) };
    }
    return {};
}

std::optional<std::filesystem::path> resolveBundledExtension(
    std::string_view name,
    std::span<std::filesystem::path const> directories,
    core::platform::FileSystem const& fileSystem)
{
    if (!isPlainName(name))
        return std::nullopt;

    auto const module = std::string { name }.append(ModuleExtension);
    for (auto const& directory: directories)
        if (auto candidate = directory / module; fileSystem.isRegularFile(candidate))
            return candidate;
    return std::nullopt;
}

} // namespace vtextension
