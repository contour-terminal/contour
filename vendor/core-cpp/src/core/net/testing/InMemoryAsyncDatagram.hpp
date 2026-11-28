// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `InMemoryAsyncDatagramNetwork` — UDP between @c IAsyncDatagramSocket s in one process, for a
/// consumer that tests a datagram protocol without the kernel.

#include <core/net/IAsyncDatagramSocket.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>

#include <cstddef>
#include <expected>
#include <memory>

namespace core::net::testing
{

/// A network of in-memory datagram sockets.
///
/// **The state machine is the real socket's.** Every socket this opens runs the same
/// @c IAsyncDatagramSocket implementation as @c openAsyncDatagramSocket, over an in-memory channel,
/// so each refusal (closed, unbound, unconnected, wrong family) is answered exactly as the real one
/// answers it. What differs is only what the kernel would do:
///
/// - **Addresses.** A bind to port 0 takes the next free port from 49152 up. Two sockets of one
///   family may not hold the same port when their addresses are equal or either is the wildcard
///   (@c NetErrorCode::AddressInUse). A datagram is delivered to the socket bound to its exact
///   destination, else to the one bound to the wildcard on that port.
/// - **Senders.** `from` is the sender's bound address, or, when the sender bound the wildcard,
///   the destination's address at the sender's port: what a loopback route picks.
/// - **Loss.** A datagram to nobody, from someone a connected receiver does not accept, or to a
///   receiver whose queue holds @c inboxCapacity datagrams already, is dropped and the send still
///   succeeds, as UDP's does.
/// - **Sizes.** A payload above the family's largest (@c MaxIpv4DatagramPayload,
///   @c MaxIpv6DatagramPayload) is @c NetErrorCode::MessageTooLarge at the send.
///
/// **What it does not model, and what that produces.** Options: every @c option and @c setOption is
/// @c NetErrorCode::Unsupported (a refusal, never a false pass). ICMP: a send to a dead port is
/// silent, as an unconnected real socket's is. Several loops: a send delivers through the
/// receiving socket's loop, so sockets on different threads work, but one socket still belongs to
/// one loop.
///
/// The network may be destroyed before its sockets; they keep what they need.
class CORE_NET_API InMemoryAsyncDatagramNetwork
{
  public:
    /// The queue depth a receiver holds when the caller names none.
    static constexpr std::size_t DefaultInboxCapacity = 64;

    /// @param inboxCapacity How many datagrams one socket queues before it drops the newest.
    explicit InMemoryAsyncDatagramNetwork(std::size_t inboxCapacity = DefaultInboxCapacity);
    ~InMemoryAsyncDatagramNetwork();
    InMemoryAsyncDatagramNetwork(InMemoryAsyncDatagramNetwork const&) = delete;
    InMemoryAsyncDatagramNetwork& operator=(InMemoryAsyncDatagramNetwork const&) = delete;
    InMemoryAsyncDatagramNetwork(InMemoryAsyncDatagramNetwork&&) = delete;
    InMemoryAsyncDatagramNetwork& operator=(InMemoryAsyncDatagramNetwork&&) = delete;

    /// Opens an unbound socket on this network.
    /// @param loop The loop its receives resume on; must outlive the socket.
    /// @param family The address family.
    /// @return The socket. Never fails today; the result type is @c openAsyncDatagramSocket 's, so
    ///         a consumer can inject either.
    [[nodiscard]] std::expected<std::unique_ptr<IAsyncDatagramSocket>, NetError> open(EventLoop& loop,
                                                                                      AddressFamily family);

    /// The network's shared state; opaque, and public only so the implementation can name it.
    struct State;

  private:
    std::shared_ptr<State> _state;
};

} // namespace core::net::testing
