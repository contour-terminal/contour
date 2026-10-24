// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonControl.hpp>

#include <utility>

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
