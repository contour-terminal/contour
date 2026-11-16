// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// The daemon-scoped extensions of a daemon: vtextension's DaemonExtensionRuntime, behind vthost's
/// IDaemonWorldHosting.
///
/// Built only with extensions (CONTOUR_EXTENSIONS); nothing else in vthost but the daemon's composition
/// includes this header, so a build without them links no vtextension.

#include <vtbackend/screen/LocalTime.hpp>

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/FileSystem.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/ExtensionPresence.hpp>
#include <vtextension/FileLocker.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vthost/ExtensionHosting.hpp>

namespace vthost
{

/// What a daemon's daemon-scoped extensions borrow; each must outlive them.
struct DaemonWorldServices
{
    vtextension::wasm::IWasmBackend& backend;       ///< What the engine is created from.
    vtpty::IProcessRunner& runner;                  ///< Runs the declared processes.
    vtpty::IDirectoryOpener& directories;           ///< Opens their working directories.
    core::platform::FileSystem const& fileSystem;   ///< Where modules, grants and stores are.
    vtextension::IFileLocker const& locker;         ///< Takes each store's directory lock.
    core::platform::IWallClock const& wallClock;    ///< The wall-clock time `now` answers with.
    vtbackend::LocalTimeConverter const& localTime; ///< The time zone each `load` reads its UTC offset in.
};

/// One summary an extension published of itself.
struct DaemonExtensionSummary
{
    std::string shortText; ///< The one-line summary.
    std::string detail;    ///< The longer text; its lines are separated by `\n`.
};

/// The production IDaemonWorldHosting: every declared daemon-scoped extension on one
/// vtextension::DaemonExtensionRuntime, composed on construction, its problems logged. Where they show
/// themselves (`summary-set`) is kept here, latest per extension.
class DaemonWorldExtensions final: public IDaemonWorldHosting, private vtextension::IExtensionPresence
{
  public:
    /// Composes the daemon world, and starts its boot-time extensions.
    /// @param config What it is.
    /// @param services What it borrows.
    /// @param events What the host hears from it.
    DaemonWorldExtensions(vtextension::DaemonExtensionRuntimeConfig config,
                          DaemonWorldServices const& services,
                          DaemonWorldEvents events);

    void startOnDemand(std::string_view name) override;
    void stop() noexcept override;

    /// @param name What the configuration calls an extension.
    /// @return What it last published of itself; nullopt if nothing yet. From any thread.
    [[nodiscard]] std::optional<DaemonExtensionSummary> summary(std::string_view name) const;

  private:
    void publishSummary(std::string_view extension,
                        std::string_view shortText,
                        std::string_view detail) override;

    DaemonWorldEvents _events;
    mutable std::mutex _mutex;
    std::map<std::string, DaemonExtensionSummary, std::less<>> _summaries; ///< Guarded by _mutex.
    /// Last: its lane publishes into what is above, from the moment it is composed.
    std::unique_ptr<vtextension::DaemonExtensionRuntime> _runtime;
};

} // namespace vthost
