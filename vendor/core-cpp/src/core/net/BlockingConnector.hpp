// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `BlockingConnector` — an @c IConnector for threads that may block.
///
/// Origin: fastcached `src/FastCache/Net/BlockingConnector.{hpp,cpp}`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), over core-cpp's shared dial flow and dial
/// primitives rather than a copy of either.

#include <core/async/Task.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace core::net
{

/// What a @c BlockingConnector arms on every socket it hands over.
///
/// At namespace scope rather than nested, so a defaulted constructor parameter can name it.
struct BlockingConnectorOptions
{
    /// How long one later read or write may block, armed as `SO_RCVTIMEO` and `SO_SNDTIMEO` BEFORE
    /// the socket is handed over, so there is no window in which it is reachable and unbounded.
    /// Non-positive leaves it unbounded.
    ///
    /// **Here and not on @c IConnector::connect**, because only a blocking socket has such a
    /// property: a loop socket's reads suspend rather than block, so the option is inert there, and
    /// an interface carrying a parameter one implementation can honour hands every other caller a
    /// bound that does not exist (`.agent/rules/async-and-net.md`).
    ///
    /// Per CALL, not per exchange: a peer dribbling a byte just inside the bound can still take
    /// arbitrarily long. It bounds the failure that matters, a peer that stops entirely.
    std::chrono::milliseconds ioTimeout { 0 };
};

/// Dials over the platform socket API on the calling thread and hands back a @c BlockingSocket.
///
/// **The dial is issued non-blocking and then waited on**, rather than as a plain blocking
/// `connect()`: that is what makes the budget mean anything. A blocking connect to a black-holed
/// address is governed by the kernel's retry schedule, measured in minutes, and no caller can
/// shorten it.
///
/// **A coroutine that never suspends, and that is the point.** @c connect returns a `Task` because
/// the interface does, but the resolver is inline and the wait is a syscall, so the task is never
/// left suspended -- precisely the precondition `core::async::syncRun` states. A caller on a loop
/// thread takes @c makeConnector instead; helpers meant only for threads that may block take a
/// `BlockingConnector&` rather than an `IConnector&`, so the precondition is enforced by the type.
///
/// Name resolution, the total budget across candidates, and "report the LAST failure" are the
/// shared flow's (`ConnectFlow.hpp`), so this connector and the loop's answer those identically.
class BlockingConnector final: public IConnector
{
  public:
    /// @param resolver Name resolution; the process-wide `getaddrinfo` one by default. Injected so a
    ///        test can dial a scripted endpoint without a DNS lookup. Must outlive this.
    /// @param options What every returned socket is armed with.
    /// @param clock The source of the total budget, or null for a @c platform::SteadyClock of this
    ///        connector's own. Injected so the budget is a `ManualClock` test rather than a sleep.
    explicit BlockingConnector(IAddressResolver& resolver = defaultAddressResolver(),
                               BlockingConnectorOptions options = {},
                               platform::IClock* clock = nullptr) noexcept;

    /// @copydoc IConnector::connect
    [[nodiscard]] async::Task<SocketResult> connect(std::string host,
                                                    std::uint16_t port,
                                                    DialOptions options) override;

  private:
    /// Declared before @c _clock, which may bind to it: the order is load-bearing and the compiler
    /// checks it here, which is why it is a member rather than a local.
    platform::SteadyClock _ownClock;

    InlineAddressResolver _resolver;
    BlockingConnectorOptions _options;
    platform::IClock& _clock;
};

} // namespace core::net
