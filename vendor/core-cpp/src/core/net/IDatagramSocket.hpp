// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IDatagramSocket` — a connectionless, message-oriented socket, and the two value types its
/// operations take and return.
///
/// Implementations: the real UDP socket behind `openUdpSocket` (`<core/net/UdpSocket.hpp>`), the
/// shared/private pair behind `answerFromOwnAddress` (`<core/net/SharedPortDatagram.hpp>`), and
/// `testing::DatagramBus`, which is a whole network segment in one process
/// (`<core/net/testing/InMemoryDatagram.hpp>`).
///
/// Origin: fastcached `src/FastCache/Net/IDatagramSocket.hpp`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).

#include <core/net/NetError.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace core::net
{

/// Where a datagram came from, or is going to.
///
/// **Host and port kept apart, and that pairing is the whole reason this type exists** rather than
/// a `std::string` holding `host:port`. Joining the two is a grammar — an IPv6 literal has to be
/// bracketed or the next `rfind(':')` takes the wrong colon — so a socket that took the joined text
/// would have to own a copy of that grammar in order to hand the halves to `getaddrinfo`. The
/// stream side already answers this the same way: `connect(loop, host, port)` takes the pair apart,
/// for exactly this reason. A caller that wants text joins it one layer up.
struct DatagramAddress
{
    /// Address or hostname, unbracketed — `192.0.2.7`, `::1`, `broadcast`.
    std::string host;

    /// Port in host byte order. Zero is a legitimate value here: it is what a socket asks for when
    /// the kernel should choose, and what a bus address that names no port carries.
    std::uint16_t port { 0 };

    /// Ordered, not merely comparable, so this type can BE a map key rather than being flattened
    /// into one. A defaulted `<=>` implicitly declares `==` too, which is what the equality
    /// comparisons against a sentinel rely on.
    /// @param other Address to compare against.
    /// @return Host first, then port.
    [[nodiscard]] auto operator<=>(DatagramAddress const& other) const = default;
};

/// One datagram, and who sent it.
struct ReceivedDatagram
{
    std::vector<std::byte> payload; ///< The bytes, exactly as they arrived.

    /// The sender's address, as the kernel reports it.
    ///
    /// What a peer *claims* about itself travels in the payload; this is what the kernel says, and
    /// the two are deliberately separate — a peer that advertises an address it does not answer on
    /// is the failure a discovery protocol exists to make visible, not one to paper over by
    /// trusting the payload.
    DatagramAddress from;
};

/// Why a receive returned nothing.
enum class DatagramWait : std::uint8_t
{
    TimedOut, ///< Nothing arrived within the deadline. Ordinary, not a fault.
    Closed,   ///< The socket was shut down; the loop should end.

    /// A datagram arrived larger than the receive buffer and was dropped rather than handed back
    /// cut short. The loop may go on: the next receive answers the next datagram. With the buffer
    /// @c openUdpSocket sizes by address family this cannot happen to a legal datagram, so seeing it
    /// means the sender is not speaking UDP as the family defines it.
    MessageTooLarge,
};

/// A connectionless socket.
///
/// **Deliberately not `ISocket`, and the asymmetry is the reason.** A datagram socket has no
/// connection, no stream and no partial read: a receive either yields one whole message or nothing,
/// and a send either places one whole message or fails. Modelling that through an interface built
/// for byte streams would mean every implementation and every test double carrying framing that
/// datagrams already have — and it would inherit `ISocket`'s single-read-slot and buffer-lifetime
/// contracts, neither of which has anything to say here.
///
/// **Blocking rather than reactor-driven, also deliberately.** Its callers send a handful of
/// datagrams a minute on a thread of their own, so parking on an `EventLoop` would buy nothing and
/// cost every implementation a park table.
class IDatagramSocket
{
  public:
    IDatagramSocket() = default;
    IDatagramSocket(IDatagramSocket const&) = delete;
    IDatagramSocket(IDatagramSocket&&) = delete;
    IDatagramSocket& operator=(IDatagramSocket const&) = delete;
    IDatagramSocket& operator=(IDatagramSocket&&) = delete;
    virtual ~IDatagramSocket() = default;

    /// Sends one datagram.
    ///
    /// **A datagram that does not arrive is not an error here and never will be:** this reports
    /// only that the local stack accepted it. A protocol built on this is built to survive loss, so
    /// a caller that treated a successful send as delivery would be wrong about the one thing UDP
    /// does not promise.
    /// @param payload The whole message.
    /// @param to Destination.
    /// @return Nothing, or why the local stack refused it.
    [[nodiscard]] virtual std::expected<void, NetError> send(std::span<std::byte const> payload,
                                                             DatagramAddress const& to) = 0;

    /// Waits for one datagram.
    ///
    /// **Bounded by @p timeout, and that is load-bearing rather than a convenience:** POSIX does
    /// not unblock a parked receive when another thread closes the socket, so a poll timeout is the
    /// only portable way a receive loop ever observes a shutdown. This lineage has already paid for
    /// the equivalent omission on `accept()` — a `systemctl stop` that hung until the supervisor
    /// escalated to SIGKILL.
    /// @param timeout How long to wait.
    /// @return The datagram, or why none was returned.
    [[nodiscard]] virtual std::expected<ReceivedDatagram, DatagramWait> receive(
        std::chrono::milliseconds timeout) = 0;

    /// Stops the socket, so a parked @c receive returns @c DatagramWait::Closed at its next poll.
    virtual void close() noexcept = 0;

    /// The address this socket actually bound.
    ///
    /// **Read back from the socket rather than echoed from what was asked for:** a bind to port 0
    /// means *the kernel chooses*, and a caller that has to tell a peer where to answer needs the
    /// answer.
    /// @return The bound address; an empty host when it is not bound.
    [[nodiscard]] virtual DatagramAddress boundAddress() const = 0;
};

} // namespace core::net
