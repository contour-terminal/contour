// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `DaemonRuntime` — the daemon behind a running `contour daemon`'s endpoints: what its Control connections
/// are told about it, what a stop request does, and the census of its clients.

#include <core/platform/Clock.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/ClientCensus.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/DaemonLifecycle.hpp>
#include <vthost/DaemonUpgradeHooks.hpp>
#include <vthost/handover/LiveUpgrade.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

class SessionHost;

/// What a DaemonRuntime states about its daemon, as fixed at the daemon's start.
struct DaemonRuntimeConfig
{
    std::string releaseVersion;                              ///< The daemon's release; empty when unknown.
    std::uint64_t processId = 0;                             ///< The daemon's process id.
    std::filesystem::path socketPath;                        ///< The control socket it serves.
    std::filesystem::path executablePath;                    ///< The image it runs from.
    std::filesystem::path logFile;                           ///< Where it logs; empty for stderr.
    DaemonLifecycle lifecycle = DaemonLifecycle::Persistent; ///< When it ends without being asked to.
};

/// What a daemon needs to upgrade itself, wired by whoever builds its handover::LiveUpgrade.
struct DaemonUpgradePort
{
    /// The hooks the LiveUpgrade was built with: where the requester is told, and the observers subscribe.
    /// Not owned; outlives the runtime.
    DaemonUpgradeHooks& hooks;
    /// Runs the upgrade (handover::LiveUpgrade::run).
    std::function<core::async::Task<std::expected<void, handover::UpgradeError>>(handover::UpgradeTarget)>
        run;
    /// Whether one is under way (handover::LiveUpgrade::state).
    std::function<handover::UpgradeState()> state;
};

/// The DaemonControl of a running daemon. Loop-thread only, like the connections that ask it.
class DaemonRuntime final: public DaemonControl
{
  public:
    /// @param config What it states about its daemon.
    /// @param host The daemon's sessions, counted live (not owned; outlives this).
    /// @param clock What its uptime is measured against, from now (not owned; outlives this).
    /// @param requestShutdown Asked for the daemon's shutdown on a stop request. Called from inside the
    ///        asking connection's PDU handler, so it must only post the shutdown, never run it there.
    /// @param upgrade What upgrades the daemon in place; nullopt for a daemon that cannot (an upgrade request
    ///        is then refused NotSupported).
    DaemonRuntime(DaemonRuntimeConfig config,
                  SessionHost const& host,
                  core::platform::IClock const& clock,
                  std::function<void(ShutdownCause)> requestShutdown,
                  std::optional<DaemonUpgradePort> upgrade = std::nullopt);

    /// @return Every field of the reply, always: a running daemon has a status to state.
    [[nodiscard]] std::expected<proto::DaemonStatusReply, DaemonControlError> status() const override;
    [[nodiscard]] std::string_view releaseVersion() const noexcept override;
    /// Asks for the shutdown whose cause @p reason names; always under way once asked.
    [[nodiscard]] std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) override;
    [[nodiscard]] ClientCensus::Ticket admitClient() override;
    /// Refuses InProgress while an upgrade runs, NotSupported without a DaemonUpgradePort; otherwise runs it
    /// with @p onStarted as the requester's answer (@see DaemonUpgradeHooks::setRequester).
    [[nodiscard]] core::async::Task<std::expected<void, handover::UpgradeError>> requestUpgrade(
        handover::UpgradeTarget target, std::function<void()> onStarted) override;
    void subscribeUpgrade(UpgradeObserver& observer) override;
    void unsubscribeUpgrade(UpgradeObserver& observer) override;

    /// @return Whether it upgrades its daemon in place when asked: whether it was given a DaemonUpgradePort.
    [[nodiscard]] bool canUpgrade() const noexcept { return _upgrade.has_value(); }

    /// @return The census its tickets count in.
    [[nodiscard]] ClientCensus& census() noexcept { return _census; }

  private:
    DaemonRuntimeConfig _config;
    SessionHost const& _host;
    core::platform::IClock const& _clock;
    core::platform::SteadyTimePoint _startedAt;
    std::function<void(ShutdownCause)> _requestShutdown;
    std::optional<DaemonUpgradePort> _upgrade;
    ClientCensus _census;
};

} // namespace vthost
