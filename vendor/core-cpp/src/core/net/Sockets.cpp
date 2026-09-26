// SPDX-License-Identifier: Apache-2.0
#include <core/net/Sockets.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ThreadedAddressResolver.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace core::net
{

/// **contour's `connect()`, re-implemented over `makeConnector` rather than beside it.**
///
/// The body it replaces called `getaddrinfo` inline on whichever thread awaited it, which on an
/// event loop is a stall of unbounded length: a DNS lookup with a dead resolver is seconds, and
/// every coroutine on that loop waits for it, including the ones with nothing to do with the
/// network. What changed is whose thread pays for the lookup, and three things a caller can see,
/// each recorded under Breaking in the CHANGELOG: the host is a `std::string` rather than a
/// `string_view`, a stop of the flow's own token throws rather than returning
/// @c NetErrorCode::Cancelled, and an unresolvable name is @c NetErrorCode::AddressError.
///
/// The connector is built per call rather than kept, and that is cheap by construction: it holds
/// two references and allocates nothing else. Keeping one would mean caching it per loop, which
/// is a lifetime question this function has no way to answer.
async::Task<SocketResult> connect(EventLoop* loop, std::string host, std::uint16_t port)
{
    co_return co_await connect(loop, std::move(host), port, &defaultAsyncResolver(), DialOptions {});
}

async::Task<SocketResult> connect(EventLoop* loop,
                                  std::string host,
                                  std::uint16_t port,
                                  IAsyncAddressResolver* resolver,
                                  DialOptions options)
{
    // The host arrives OWNED, as a parameter of the frame. A copy made in this body would come too
    // late: the task is lazy, so the body first runs when the task is awaited, and by then a view
    // the caller passed may name a temporary that died at the end of the call expression.
    auto const connector = makeConnector(*loop, *resolver);
    co_return co_await connector->connect(std::move(host), port, options);
}

} // namespace core::net
