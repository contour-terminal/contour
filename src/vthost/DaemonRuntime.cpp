// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonRuntime.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <system_error>
#include <utility>

#include <vthost/DaemonControlWire.hpp>
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
            case proto::StopReason::UserKeepingWorkspaces:
                return ShutdownCause::StopRequestedKeepingWorkspaces;
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

    /// Whether an upgrade under way told its requester and its clients it started.
    enum class UpgradeAnnouncement : uint8_t
    {
        Pending = 0, ///< Not yet: a failure now is the requester's answer.
        Made,        ///< Yes: the requester's connection is closed, and a failure now is only recorded.
    };

    /// @param generation The daemon's upgrade generation.
    /// @param failure Why its last upgrade failed after the announcement, if it did.
    /// @return Both as a DaemonStatusReply states them.
    [[nodiscard]] proto::DaemonUpgradeRecord upgradeRecordOf(
        std::uint64_t generation,
        std::uint64_t attempts,
        std::optional<handover::UpgradeError> const& failure)
    {
        auto record = proto::DaemonUpgradeRecord { .generation = generation, .attempts = attempts };
        if (failure)
        {
            auto reply = upgradeFailureReply(*failure);
            record.lastFailure = reply.result;
            record.lastFailureDetail = std::move(reply.detail);
        }
        return record;
    }
} // namespace

DaemonRuntime::DaemonRuntime(DaemonRuntimeConfig config,
                             SessionHost const& host,
                             core::platform::IClock const& clock,
                             std::function<void(ShutdownCause)> requestShutdown,
                             std::optional<DaemonUpgradePort> upgrade):
    _config(std::move(config)),
    _host(host),
    _clock(clock),
    _startedAt(clock.now()),
    _requestShutdown(std::move(requestShutdown)),
    _upgrade(std::move(upgrade))
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
        .upgrade = upgradeRecordOf(_config.upgradeGeneration, _upgradeAttempts, _lastUpgradeFailure),
        .relocation = std::to_underlying(_config.relocationRequest),
        .workspaces = wireCount(_host.workspaceCount()),
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

core::async::Task<std::expected<void, handover::UpgradeError>> DaemonRuntime::requestUpgrade(
    handover::UpgradeTarget target, UpgradeStarted onStarted)
{
    if (!_upgrade)
        co_return std::unexpected(handover::UpgradeError { handover::UpgradeRefusal::NotSupported });
    if (_upgrade->state() == handover::UpgradeState::Running)
        co_return std::unexpected(handover::UpgradeError { handover::UpgradeRefusal::InProgress });
    ++_upgradeAttempts;
    _lastUpgradeFailure.reset();
    // In this frame, which the requester below refers to: it is called, if at all, inside run(). Dropped on
    // every way out of this frame, a throw included, so that it never refers to a dead frame.
    auto announcement = UpgradeAnnouncement::Pending;
    auto const dropRequester = core::Finally { [this] { _upgrade->hooks.clearRequester(); } };
    _upgrade->hooks.setRequester(
        [&announcement, onStarted = std::move(onStarted)](std::string_view toRelease) {
            announcement = UpgradeAnnouncement::Made;
            if (onStarted)
                onStarted(toRelease);
        });
    // Awaited in the try, never in a handler: the handler only keeps the exception.
    auto thrown = std::exception_ptr {};
    auto result = std::expected<void, handover::UpgradeError> {};
    try
    {
        result = co_await _upgrade->run(std::move(target));
    }
    catch (...)
    {
        thrown = std::current_exception();
    }
    if (thrown)
    {
        // The rollback guard of the upgrade resumed the daemon: to its clients, told already, that is an
        // exec that failed.
        if (announcement == UpgradeAnnouncement::Made)
            _lastUpgradeFailure = handover::UpgradeError { handover::ExecFailure {
                .cause = std::make_error_code(std::errc::io_error) } };
        std::rethrow_exception(thrown);
    }
    if (!result && announcement == UpgradeAnnouncement::Made)
        _lastUpgradeFailure = result.error();
    co_return result;
}

void DaemonRuntime::subscribeUpgrade(UpgradeObserver& observer)
{
    if (_upgrade)
        _upgrade->hooks.subscribe(observer);
}

void DaemonRuntime::unsubscribeUpgrade(UpgradeObserver& observer)
{
    if (_upgrade)
        _upgrade->hooks.unsubscribe(observer);
}

} // namespace vthost
