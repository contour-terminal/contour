// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonRuntime.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <utility>

#include <vthost/SessionHost.hpp>

namespace vthost
{

namespace
{
    /// @param reason Why a Control connection asks the daemon to stop.
    /// @return The cause of the shutdown that stop is.
    [[nodiscard]] constexpr ShutdownCause shutdownCauseOf(proto::StopReason reason) noexcept
    {
        switch (reason)
        {
            case proto::StopReason::User: return ShutdownCause::StopRequested;
            case proto::StopReason::UpgradeRestart: return ShutdownCause::UpgradeRestart;
        }
        return ShutdownCause::StopRequested;
    }

    /// @param count A count, as a container states it.
    /// @return It as the status reply's `u32` field carries it, saturated rather than wrapped.
    [[nodiscard]] constexpr std::uint32_t wireCount(std::size_t count) noexcept
    {
        return static_cast<std::uint32_t>(
            std::min<std::size_t>(count, std::numeric_limits<std::uint32_t>::max()));
    }
} // namespace

DaemonRuntime::DaemonRuntime(DaemonRuntimeConfig config,
                             SessionHost const& host,
                             core::platform::IClock const& clock,
                             std::function<void(ShutdownCause)> requestShutdown):
    _config(std::move(config)),
    _host(host),
    _clock(clock),
    _startedAt(clock.now()),
    _requestShutdown(std::move(requestShutdown))
{
}

std::expected<proto::DaemonStatusReply, DaemonControlError> DaemonRuntime::status() const
{
    auto const uptime = std::chrono::duration_cast<std::chrono::seconds>(_clock.now() - _startedAt);
    return proto::DaemonStatusReply {
        .releaseVersion = _config.releaseVersion,
        .processId = _config.processId,
        .uptimeSeconds =
            static_cast<std::uint64_t>(std::max(uptime.count(), std::chrono::seconds::rep { 0 })),
        .socketPath = core::pathToUtf8(_config.socketPath),
        .executablePath = core::pathToUtf8(_config.executablePath),
        .logFile = core::pathToUtf8(_config.logFile),
        .sessions = wireCount(_host.sessionCount()),
        .clients = wireCount(_census.count()),
        .lifecycle = std::to_underlying(_config.lifecycle),
    };
}

std::string_view DaemonRuntime::releaseVersion() const noexcept
{
    return _config.releaseVersion;
}

std::expected<void, DaemonControlError> DaemonRuntime::requestStop(proto::StopReason reason)
{
    _requestShutdown(shutdownCauseOf(reason));
    return {};
}

ClientCensus::Ticket DaemonRuntime::admitClient()
{
    return _census.admit();
}

} // namespace vthost
