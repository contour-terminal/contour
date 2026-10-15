// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/GuiConfigStore.hpp>

#include <core/Utf8Path.hpp>

#include <format>
#include <system_error>
#include <utility>

namespace contour::config
{

namespace
{
    /// Removes @p path, treating an already-absent file as success (the desired post-state either way).
    /// @param path The file to remove.
    /// @return Nothing on success, or a human-readable error when removal actually failed.
    std::expected<void, std::string> removeFile(std::filesystem::path const& path)
    {
        auto ec = std::error_code {};
        std::filesystem::remove(path, ec); // a missing file yields false with ec clear: still success
        if (ec)
            return std::unexpected(
                std::format("could not remove {}: {}", core::pathToUtf8(path), ec.message()));
        return {};
    }
} // namespace

FileGuiConfigStore::FileGuiConfigStore(std::filesystem::path configDir): _configDir { std::move(configDir) }
{
}

std::expected<void, std::string> FileGuiConfigStore::saveProfile(std::string const& name,
                                                                 config::TerminalProfile const& profile)
{
    return atomicWriteFile(_configDir / "profiles" / sideFileName(name), config::emitProfileYaml(profile));
}

std::expected<void, std::string> FileGuiConfigStore::deleteProfile(std::string const& name)
{
    return removeFile(_configDir / "profiles" / sideFileName(name));
}

std::expected<void, std::string> FileGuiConfigStore::saveColorScheme(std::string const& name,
                                                                     vtbackend::ColorPalette const& palette)
{
    return atomicWriteFile(_configDir / "colorschemes" / sideFileName(name),
                           config::emitColorSchemeYaml(palette));
}

std::expected<void, std::string> FileGuiConfigStore::deleteColorScheme(std::string const& name)
{
    return removeFile(_configDir / "colorschemes" / sideFileName(name));
}

std::expected<void, std::string> FileGuiConfigStore::saveGuiSettings(
    config::GuiManagedSettings const& settings)
{
    return atomicWriteFile(_configDir / "settings.yml", config::emitGuiSettingsYaml(settings));
}

} // namespace contour::config
