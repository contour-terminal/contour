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
#include <vector>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/ExtensionPresence.hpp>
#include <vtextension/FileLocker.hpp>
#include <vtextension/ServedTabService.hpp>
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

/// Makes one extension's tab-and-action service on the host's served tabs and invoked actions: it refuses a
/// scheme other than the manifest's `serve` line (`bad-argument`), names the tabs it opens with the
/// manifest's title, and words the host's refusals as the WIT's host-error.
/// @param owner The extension, as its manifest declares it.
/// @param wiring What it reaches of the host; outlives the service.
/// @return The service.
[[nodiscard]] std::unique_ptr<vtextension::IServedTabService> servedTabServiceOf(
    vtextension::ServedTabOwner const& owner, DaemonWorldWiring wiring);

/// The production IDaemonWorldHosting: every declared daemon-scoped extension on one
/// vtextension::DaemonExtensionRuntime, composed on construction, its problems logged. Where they show
/// themselves is kept here, per extension and of its latest run only: where it stands, and the summary it
/// last published (`summary-set`). What the status line shows follows from where it stands (StatusTexts).
/// Each extension's tab and action imports reach the host's IServedTabs and IExtensionActions through an
/// adapter of its own, which checks the scheme against the manifest's `serve` line and words the host's
/// refusals as the WIT's host-error (ServedTabErrorRows).
class DaemonWorldExtensions final:
    public IDaemonWorldHosting,
    private vtextension::IExtensionPresence,
    private vtbackend::IDaemonExtensionSummaries
{
  public:
    /// Composes the daemon world, and starts its boot-time extensions.
    /// @param config What it is.
    /// @param services What it borrows.
    /// @param events What the host hears from it.
    /// @param post Hands a job to the host's loop: the owner's thread, where the runtime is asked.
    /// @param wiring What it reaches of the host: the served tabs and the invoked actions; outlives it.
    DaemonWorldExtensions(vtextension::DaemonExtensionRuntimeConfig config,
                          DaemonWorldServices const& services,
                          DaemonWorldEvents events,
                          OwnerPost post,
                          DaemonWorldWiring wiring);

    void startOnDemand(std::string_view name) override;
    [[nodiscard]] std::vector<DaemonExtensionRequest> pendingRequests() const override;
    [[nodiscard]] std::vector<DaemonExtensionListing> list() const override;
    void answer(std::string_view name, std::string_view grantDigest, GrantState answer) override;
    void revoke(std::string_view name) override;
    [[nodiscard]] std::optional<DaemonExtensionStatus> status(std::string_view name) const override;
    [[nodiscard]] vtbackend::IDaemonExtensionSummaries const& summaries() const noexcept override;
    [[nodiscard]] std::vector<ExtensionActionRows> actionRows() const override;
    void hint(std::string_view name, ServedHint hint) override;
    void restart(std::string_view name) override;
    void stop() noexcept override;

    /// @return Where the runtime and its runners report what the extensions show of themselves: the seam a
    ///         test stands in for a run's lane through.
    [[nodiscard]] vtextension::IExtensionPresence& presence() noexcept { return *this; }

    /// @param name What the configuration calls an extension.
    /// @return What it last published of itself; nullopt if nothing yet. From any thread.
    [[nodiscard]] std::optional<DaemonExtensionSummary> summary(std::string_view name) const;

  private:
    void publishSummary(std::string_view extension,
                        vtextension::DaemonRunGeneration generation,
                        std::string_view shortText,
                        std::string_view detail) override;
    void phaseChanged(std::string_view extension,
                      vtextension::DaemonRunGeneration generation,
                      vtextension::DaemonExtensionPhase phase) override;

    [[nodiscard]] std::string summaryOf(std::string_view name) const override;

    /// Draws the host's frame into @p name's tabs, if it is still disabled. On the owner's thread.
    void drawStoppedIfDisabled(std::string const& name);

    /// @return The latest fault of @p name's run, neutralized; empty if none. On the owner's thread.
    [[nodiscard]] std::string lastFaultOf(std::string_view name) const;

    /// Runs @p change on the runtime, and tells the host when the questions waiting changed with it.
    template <typename Change>
    void changingRequests(Change change);

    /// What one extension shows of itself: of its latest run only.
    struct Shown
    {
        vtextension::DaemonRunGeneration generation = 0; ///< The latest run told.
        vtextension::DaemonExtensionPhase phase =
            vtextension::DaemonExtensionPhase::Stopped; ///< Where it stands.
        std::optional<DaemonExtensionSummary> summary;  ///< What that run last published; none yet.
    };

    /// @return What @p name shows as of run @p generation, made anew for a newer run; null for an older run,
    ///         whose word is dropped. Call holding _mutex.
    [[nodiscard]] Shown* shownAs(std::string_view name, vtextension::DaemonRunGeneration generation);

    DaemonWorldEvents _events;
    OwnerPost _post;
    DaemonWorldWiring _wiring;
    mutable std::mutex _mutex;
    std::map<std::string, Shown, std::less<>> _shown; ///< Guarded by _mutex.
    /// Last: its lane publishes into what is above, from the moment it is composed.
    std::unique_ptr<vtextension::DaemonExtensionRuntime> _runtime;
};

} // namespace vthost
