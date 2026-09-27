// SPDX-License-Identifier: Apache-2.0
#include <core/net/BlockingConnector.hpp>

#include <core/net/BlockingSocket.hpp>
#include <core/net/ConnectFlow.hpp>
#include <core/net/detail/BlockingPrimitives.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/platform/WinsockInit.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace core::net
{

namespace
{
    /// What one candidate attempt needs beyond the endpoint.
    struct BlockingDialState
    {
        platform::IClock* clock = nullptr;
        std::chrono::milliseconds ioTimeout { 0 };
    };

    /// How long this candidate may wait: what is left of the whole call's budget.
    ///
    /// **Never zero once a budget exists**: the wait reads a non-positive timeout as "as long as the
    /// kernel takes", so handing it an exhausted budget would remove the bound at exactly the moment
    /// it matters most.
    /// @param clock The budget's clock, or null for none.
    /// @param deadline The candidate's deadline; `max()` means none.
    /// @return The wait, or zero for "no bound of ours".
    [[nodiscard]] std::chrono::milliseconds remainingBudget(platform::IClock const* clock,
                                                            platform::SteadyTimePoint deadline)
    {
        if (clock == nullptr || deadline == platform::SteadyTimePoint::max())
            return std::chrono::milliseconds { 0 };
        auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock->now());
        return left > std::chrono::milliseconds { 0 } ? left : std::chrono::milliseconds { 1 };
    }

    /// One candidate, dialled on this thread: open non-blocking, connect, wait, ask `SO_ERROR`, go
    /// blocking, arm, hand over.
    ///
    /// Synchronous under the coroutine it is called from, which is what keeps the whole flow a task
    /// that never suspends.
    [[nodiscard]] SocketResult dialOne(BlockingDialState const& dial,
                                       ResolvedEndpoint const& endpoint,
                                       platform::SteadyTimePoint deadline,
                                       detail::StreamSocketOptions const& options)
    {
        auto opened = detail::openDialSocket(endpoint);
        if (!opened.has_value())
            return std::unexpected(std::move(opened.error()));
        auto handles = *opened;
        // Every early return below closes what was opened; the success path empties `handles` in
        // `releaseBlocking`, which disarms this by leaving nothing to close.
        auto const discard =
            detail::ScopeGuard { [&handles]() noexcept { detail::closeDialSocket(nullptr, handles); } };

        // Before the connect, while the window scale can still take the receive buffer into account.
        detail::applySocketBufferSizes(handles.socket, options.buffers);
        auto const started = detail::beginConnect(handles, endpoint);
        if (!started.has_value())
            return std::unexpected(started.error());

        if (*started == detail::ConnectProgress::Pending)
        {
            if (auto const waited = detail::waitDialled(handles, remainingBudget(dial.clock, deadline));
                !waited)
                return std::unexpected(waited.error());
            // Resolved is not connected: a refused connect wakes the wait too, and SO_ERROR is the
            // only thing that tells them apart (Ruling R101).
            if (auto const settled = detail::pendingSocketError(handles); !settled)
                return std::unexpected(settled.error());
        }

        detail::applyStreamSocketOptions(handles.socket, options.keepAlive);
        auto released = detail::releaseBlocking(handles);
        if (!released.has_value())
            return std::unexpected(std::move(released.error()));

        auto socket = std::make_unique<BlockingSocket>(*released, formatPeerAddress(endpoint));
        // Armed BEFORE the socket is handed over, so there is no window in which it is reachable
        // and unbounded.
        socket->setReceiveDeadline(dial.ioTimeout);
        socket->setSendDeadline(dial.ioTimeout);
        return SocketResult { std::move(socket) };
    }

    /// @c detail::DialStep over @c dialOne: a coroutine that never suspends.
    async::Task<SocketResult> blockingDial(void* state,
                                           ResolvedEndpoint endpoint,
                                           platform::SteadyTimePoint deadline,
                                           detail::StreamSocketOptions options)
    {
        co_return dialOne(*static_cast<BlockingDialState const*>(state), endpoint, deadline, options);
    }
} // namespace

BlockingConnector::BlockingConnector(IAddressResolver& resolver,
                                     BlockingConnectorOptions options,
                                     platform::IClock* clock) noexcept:
    _resolver { resolver }, _options { options }, _clock { clock != nullptr ? *clock : _ownClock }
{
}

async::Task<SocketResult> BlockingConnector::connect(std::string host,
                                                     std::uint16_t port,
                                                     DialOptions options)
{
    platform::ensureWinsockInitialized();

    auto state = BlockingDialState { .clock = &_clock, .ioTimeout = _options.ioTimeout };

    // A null loop is what tells the shared flow and the inline resolver never to suspend, which is
    // the property `syncRun` rests on. `state` lives in THIS frame, which outlives the await.
    co_return co_await detail::runConnectFlow(
        &_resolver, nullptr, &_clock, std::move(host), port, options, &blockingDial, &state);
}

} // namespace core::net
