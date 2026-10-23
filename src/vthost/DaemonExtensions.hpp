// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The status-line extensions of a daemon: vtextension's runtime, behind vthost's IExtensionHosting.
///
/// Built only with extensions (CONTOUR_EXTENSIONS); nothing else in vthost includes this header, so a build
/// without them links no vtextension, and every other vthost type keeps one layout in both builds.

#include <core/async/StopToken.hpp>

#include <memory>
#include <string_view>

#include <vtextension/ExtensionComposition.hpp>
#include <vtextension/SessionExtensions.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/PermissionWire.hpp>

namespace vthost
{

/// What a daemon's status-line extensions are: what every session runs, and the runtime they run on.
struct DaemonExtensionsConfig
{
    /// What the daemon's profile declares, with its modules resolved on the daemon's side.
    vtextension::DeclaredExtensions declared;
    /// The runtime's grants file (the daemon's own, beside its configuration), bundled directories, worker
    /// count and limits.
    vtextension::ExtensionRuntimeConfig runtime;
};

/// What a daemon's status-line extensions borrow; each must outlive them.
struct DaemonExtensionServices
{
    /// What the runtime borrows: the command capture, the wall clock, the environment and the filesystem.
    vtextension::ExtensionRuntimeServices runtime;
    /// The capture's shutdown: requested by stop(), so a command in flight ends then, not at its timeout.
    core::async::StopSource& commandShutdown;
};

/// @param manifest An extension's manifest.
/// @param hostId The host id the daemon assigned the installed extension.
/// @return The question it asks, as the wire carries it; naming no session yet.
[[nodiscard]] proto::PermissionRequest permissionRequestOf(std::string_view hostId,
                                                           vtextension::Manifest const& manifest);

/// @param answer An answer as the wire spells it.
/// @return The same, as the grant store keeps it.
[[nodiscard]] vtextension::GrantState storedGrantStateOf(GrantState answer) noexcept;

/// @param state An answer as the grant store keeps it.
/// @return The same, as the wire spells it.
[[nodiscard]] GrantState wireGrantStateOf(vtextension::GrantState state) noexcept;

/// The production IExtensionHosting: every session's extensions on one vtextension::ExtensionRuntime --
/// the grants, the engine, the worker pool and the build thread -- which is created with the first session,
/// so a daemon whose sessions never start reads no grants file and starts no thread.
///
/// Its sessions' extensions post their builds, questions and frames to the loop through the hooks their
/// SessionHost hands them, and nothing on the loop ever waits for an extension (A1).
///
/// **Lifetime.** Owned by the SessionHost, which destroys it after its sessions: the runtime joins the build
/// thread, then the pool, whose workers run each runner's last job, and releases what they retired.
class DaemonExtensions final: public IExtensionHosting
{
  public:
    /// @param config What the extensions are.
    /// @param services What they borrow.
    DaemonExtensions(DaemonExtensionsConfig config, DaemonExtensionServices services);

    [[nodiscard]] std::unique_ptr<ISessionExtensions> createSession(SessionExtensionEvents events,
                                                                    OwnerPost post) override;
    void answer(std::string_view hostId, std::string_view digest, GrantState answer) override;
    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override;
    void releaseRetired() noexcept override;
    void stop() noexcept override;

    /// @return How many worker threads run; none until the first extension is started.
    [[nodiscard]] std::size_t runningWorkers() const noexcept
    {
        return _runtime ? _runtime->runningWorkers() : 0;
    }

  private:
    /// @return The runtime, made on first use.
    [[nodiscard]] vtextension::ExtensionRuntime& runtime();

    vtextension::DeclaredExtensions _declared;
    vtextension::ExtensionRuntimeConfig _runtimeConfig;
    DaemonExtensionServices _services;
    std::unique_ptr<vtextension::ExtensionRuntime> _runtime; ///< Null until the first session.
    bool _stopped = false;                                   ///< Once stop() was called.
};

} // namespace vthost
