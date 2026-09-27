// SPDX-License-Identifier: Apache-2.0
#include <core/net/ConnectFlow.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ReadinessDial.hpp>
#include <core/net/detail/DialPrimitives.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace core::net
{

namespace
{
    /// The one connector this library ships.
    ///
    /// **One rather than one per backend.** fastcached had an `EpollConnector` and a
    /// `KqueueConnector` whose bodies were identical but for the reactor type they named, held
    /// together by a template; core-cpp has one @c EventLoop over an @c IoBackend, so the
    /// platform difference is four OS calls in `detail/DialPrimitives.hpp` and the connector has
    /// nothing left to vary over.
    ///
    /// The loop is a CONSTRUCTOR parameter rather than a per-call one: the socket handed back is
    /// pinned to one loop, so which loop is a property of the connector. Passing one per call
    /// would let a caller obtain a socket wired to a loop other than the one its coroutine runs
    /// on — a data race with no symptom until load.
    class LoopConnector final: public IConnector
    {
      public:
        /// @param loop The loop the returned sockets are pinned to; not owned.
        /// @param resolver The name-resolution seam; not owned.
        LoopConnector(EventLoop& loop, IAsyncAddressResolver& resolver) noexcept:
            _loop(loop), _resolver(resolver)
        {
        }

        /// @copydoc IConnector::connect
        [[nodiscard]] async::Task<SocketResult> connect(std::string host,
                                                        std::uint16_t port,
                                                        DialOptions options) override
        {
            co_return co_await detail::runConnectFlow(
                &_resolver, &_loop, &_loop.clock(), std::move(host), port, options, &dialStep, &_loop);
        }

      private:
        /// The @c detail::DialStep: a completion-port dial where the loop lends a port, the
        /// readiness dial everywhere else.
        ///
        /// **Asked of the loop, per candidate, rather than chosen at compile time**, because the
        /// loop's backend decides and a test double lends no port. The two dials
        /// differ in the two ways `detail::dialCompletion` states — the outcome is the completion's
        /// status rather than `SO_ERROR`, and a deadline cancels the operation and lets the
        /// completion report rather than settling the dial itself — and a dial that carried the
        /// readiness rules onto a port would get both wrong.
        /// @param state The loop, as a `void*`.
        /// @param endpoint The candidate to dial.
        /// @param deadline When to give up on it.
        /// @param options Keepalive and buffer sizes for the connected socket.
        /// @return The connected socket, or why this candidate did not produce one.
        static async::Task<SocketResult> dialStep(void* state,
                                                  ResolvedEndpoint endpoint,
                                                  platform::SteadyTimePoint deadline,
                                                  detail::StreamSocketOptions options)
        {
            auto* const loop = static_cast<EventLoop*>(state);
            if (loop->completionPort() != nullptr)
                co_return co_await detail::dialCompletion(loop, endpoint, deadline, options);
            co_return co_await detail::dialReadiness(loop, endpoint, deadline, options);
        }

        EventLoop& _loop;
        IAsyncAddressResolver& _resolver;
    };

} // namespace

std::unique_ptr<IConnector> makeConnector(EventLoop& loop, IAsyncAddressResolver& resolver)
{
    return std::make_unique<LoopConnector>(loop, resolver);
}

} // namespace core::net
