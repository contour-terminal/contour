// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConfigReloadPoller.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <vthost/Logging.hpp>

namespace contour::cli
{

namespace
{
    /// @return @p configFile's last-write time, or nullopt when it cannot be read.
    [[nodiscard]] std::optional<std::filesystem::file_time_type> lastWriteTimeOf(
        core::platform::FileSystem const& fileSystem, std::filesystem::path const& configFile)
    {
        auto const time = fileSystem.lastWriteTime(configFile);
        return time ? std::optional { *time } : std::nullopt;
    }
} // namespace

ConfigReloadPoller::ConfigReloadPoller(core::platform::FileSystem const& fileSystem,
                                       std::filesystem::path configFile,
                                       Loader load):
    _fileSystem { fileSystem },
    _configFile { std::move(configFile) },
    _load { std::move(load) },
    _lastSeen { lastWriteTimeOf(_fileSystem, _configFile) }
{
}

std::pair<ReloadOutcome, std::shared_ptr<vthost::ProfileCatalog const>> ConfigReloadPoller::poll()
{
    // A file that vanished mid-save is not a change to load; its reappearance is.
    auto const seen = lastWriteTimeOf(_fileSystem, _configFile);
    if (!seen || seen == _lastSeen)
        return { ReloadOutcome::Unchanged, nullptr };
    _lastSeen = seen;
    auto loaded = _load();
    if (!loaded)
    {
        errorLog()("{}: not reloaded, keeping the running configuration: {}",
                   core::pathToUtf8(_configFile),
                   loaded.error());
        return { ReloadOutcome::Failed, nullptr };
    }
    vthost::daemonLog()("{}: configuration reloaded", core::pathToUtf8(_configFile));
    return { ReloadOutcome::Reloaded, *std::move(loaded) };
}

} // namespace contour::cli
