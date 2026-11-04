// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonControl.hpp>

#include <exception>
#include <system_error>
#include <utility>

#include <vthost/Logging.hpp>

namespace vthost
{

std::expected<proto::DaemonStatusReply, DaemonControlError> NullDaemonControl::status() const
{
    return std::unexpected(DaemonControlError::NotServed);
}

std::string_view NullDaemonControl::releaseVersion() const noexcept
{
    return {};
}

std::expected<void, DaemonControlError> NullDaemonControl::requestStop(proto::StopReason /*reason*/)
{
    return std::unexpected(DaemonControlError::NotServed);
}

ClientCensus::Ticket NullDaemonControl::admitClient()
{
    return {};
}

core::async::Task<void> serveUpgrade(DaemonControl* daemon,
                                     handover::UpgradeTarget target,
                                     UpgradeStarted onStarted,
                                     UpgradeFailed onFailed)
{
    auto result = std::expected<void, handover::UpgradeError> {};
    try
    {
        result = co_await daemon->requestUpgrade(std::move(target), std::move(onStarted));
    }
    catch (std::exception const& failure)
    {
        errorLog()("live upgrade failed: {}", failure.what());
        result = std::unexpected(handover::UpgradeError {
            handover::ExecFailure { std::make_error_code(std::errc::not_enough_memory) } });
    }
    catch (...)
    {
        errorLog()("live upgrade failed: unknown exception");
        result = std::unexpected(handover::UpgradeError {
            handover::ExecFailure { std::make_error_code(std::errc::not_enough_memory) } });
    }
    if (!result && onFailed)
        onFailed(result.error());
}

core::async::Task<std::expected<void, handover::UpgradeError>> NullDaemonControl::requestUpgrade(
    handover::UpgradeTarget /*target*/, UpgradeStarted /*onStarted*/)
{
    co_return std::unexpected(handover::UpgradeError { handover::UpgradeRefusal::NotSupported });
}

void NullDaemonControl::subscribeUpgrade(UpgradeObserver& /*observer*/)
{
}

void NullDaemonControl::unsubscribeUpgrade(UpgradeObserver& /*observer*/)
{
}

DaemonControl& nullDaemonControl() noexcept
{
    static auto instance = NullDaemonControl {};
    return instance;
}

namespace
{
    /// One counted connection, as a free coroutine: @p ticket is held by its frame, which a coroutine's
    /// parameters live in, for as long as @p connection runs.
    core::async::Task<void> runCounted([[maybe_unused]] ClientCensus::Ticket ticket,
                                       core::async::Task<void> connection)
    {
        co_await std::move(connection);
    }
} // namespace

ConnectionHandler countedHandler(DaemonControl& control, ConnectionHandler handler)
{
    // NOT a coroutine itself, as makeNativeHandler's is not: the ticket is taken as the connection is
    // accepted and moved into the coroutine's frame.
    return [&control, handler = std::move(handler)](ConnectionId id,
                                                    std::unique_ptr<core::net::ISocket> connection) {
        auto ticket = control.admitClient();
        return runCounted(std::move(ticket), handler(std::move(id), std::move(connection)));
    };
}

} // namespace vthost
