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
#include <string>
#include <string_view>

#include <vthost/ClientCensus.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/DaemonLifecycle.hpp>
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

/// The DaemonControl of a running daemon. Loop-thread only, like the connections that ask it.
class DaemonRuntime final: public DaemonControl
{
  public:
    /// @param config What it states about its daemon.
    /// @param host The daemon's sessions, counted live (not owned; outlives this).
    /// @param clock What its uptime is measured against, from now (not owned; outlives this).
    /// @param requestShutdown Asked for the daemon's shutdown on a stop request. Called from inside the
    ///        asking connection's PDU handler, so it must only post the shutdown, never run it there.
    DaemonRuntime(DaemonRuntimeConfig config,
                  SessionHost const& host,
                  core::platform::IClock const& clock,
                  std::function<void(ShutdownCause)> requestShutdown);

    /// @return Every field of the reply, always: a running daemon has a status to state.
    [[nodiscard]] std::expected<proto::DaemonStatusReply, DaemonControlError> status() const override;
    [[nodiscard]] std::string_view releaseVersion() const noexcept override;
    /// Asks for the shutdown whose cause @p reason names; always under way once asked.
    [[nodiscard]] std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) override;
    [[nodiscard]] ClientCensus::Ticket admitClient() override;

    /// @return The census its tickets count in.
    [[nodiscard]] ClientCensus& census() noexcept { return _census; }

  private:
    DaemonRuntimeConfig _config;
    SessionHost const& _host;
    core::platform::IClock const& _clock;
    core::platform::SteadyTimePoint _startedAt;
    std::function<void(ShutdownCause)> _requestShutdown;
    ClientCensus _census;
};

} // namespace vthost
