// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Notices that the daemon's configuration file changed and loads it again, for the daemon's reload hook.

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <vthost/ProfileCatalog.hpp>

namespace contour::cli
{

/// What one poll found.
enum class ReloadOutcome : uint8_t
{
    Unchanged = 0, ///< The file did not change since it was last seen; nothing was loaded.
    Reloaded = 1,  ///< It changed and loaded: here is the new catalog.
    Failed = 2,    ///< It changed and did not load; the old catalog stays, until the file changes again.
};

/// Polls a configuration file's last-write time and reloads the profile catalog when it moves.
class ConfigReloadPoller
{
  public:
    /// Loads the configuration into a fresh catalog, or says why it could not.
    using Loader = std::function<std::expected<std::shared_ptr<vthost::ProfileCatalog const>, std::string>()>;

    /// Reads the file's last-write time once, so the configuration the daemon started from is not loaded a
    /// second time.
    /// @param fileSystem Where the file's time is read (not owned; outlives this).
    /// @param configFile The configuration file.
    /// @param load What loads it into a catalog.
    ConfigReloadPoller(core::platform::FileSystem const& fileSystem,
                       std::filesystem::path configFile,
                       Loader load);

    /// @return Whether the file changed and loaded since the last poll, and the new catalog when it did
    ///         (null otherwise). A failed load is logged and retried only once the file changes again.
    [[nodiscard]] std::pair<ReloadOutcome, std::shared_ptr<vthost::ProfileCatalog const>> poll();

  private:
    core::platform::FileSystem const& _fileSystem;
    std::filesystem::path _configFile;
    Loader _load;
    /// The last-write time last seen; nullopt while the file could not be read, as when it is absent.
    std::optional<std::filesystem::file_time_type> _lastSeen;
};

} // namespace contour::cli
