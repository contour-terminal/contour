// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Listen where the segment shouts, answer from an address of one's own.
///
/// Origin: fastcached `src/FastCache/Net/SharedPortDatagram.hpp`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).

#include <core/net/IDatagramSocket.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace core::net
{

/// Pairs a shared listening socket with a private answering one, as one socket.
///
/// **Sharing a port buys hearing a broadcast, and nothing else.** Two sockets on one UDP port both
/// receive what is broadcast to it; only one of them receives what is *unicast* to it, and which
/// one no platform promises — measured, Windows 11 hands it to the first-bound socket and Linux to
/// the last. So a protocol that shouts to be found and then answers point-to-point cannot do both
/// through the same socket: the moment two of its nodes run on one host, every reply lands on
/// whichever of them the kernel picked, and the pair silently never finish talking.
///
/// The fix is not to stop sharing — a node that stopped could not hear a beacon at all — but to
/// stop *sending* from what is shared. This pairs the two:
///
/// | socket    | binds                       | role                                   |
/// |-----------|-----------------------------|----------------------------------------|
/// | @p shared | the well-known port, shared | receives broadcasts; **never sends**   |
/// | @p own    | an address only it holds    | sends everything; receives the answers |
///
/// Every datagram then leaves from an address exactly one socket holds, so the sender address a
/// peer replies to names one node and not a host. Nothing about the datagrams changes, so a node
/// built this way and one built the old way still complete a handshake in either direction.
///
/// **The broadcast capability belongs to @p own, not to @p shared**, and that reads backwards until
/// the table above is taken literally: the shared socket is where a broadcast is *heard*, and
/// hearing one needs no capability at all, while sending one is what `SO_BROADCAST` gates. A pair
/// built the other way up is a node that answers every challenge and never announces itself —
/// reachable, invisible, and refused by the local stack on every beacon.
///
/// Which socket a datagram left from is a question about sockets, which is why this lives in
/// `core::net` rather than in the layer that decides what a datagram *means*. That layer goes on
/// holding one @c IDatagramSocket.
/// @param shared Where broadcasts are heard: bound to the well-known port with
///        @c PortSharing::Shared. Needs no @c BroadcastMode::On, since it sends nothing.
/// @param own Where this node is answered: bound with @c PortSharing::Exclusive, and with
///        @c BroadcastMode::On, because every send leaves by it.
/// @return The pair as one socket, or @c NetErrorCode::BadHandle when either half is null — which
///         is what a caller has after a bind it could not make.
[[nodiscard]] std::expected<std::unique_ptr<IDatagramSocket>, NetError> answerFromOwnAddress(
    std::unique_ptr<IDatagramSocket> shared, std::unique_ptr<IDatagramSocket> own);

/// Opens the two UDP sockets @c answerFromOwnAddress pairs, over the real stack.
///
/// **One place, because which socket gets which option is the whole fix** and every one of the four
/// is easy to put on the wrong half. Transposing the two ports yields a node answering where the
/// segment shouts; moving @c BroadcastMode::On to the listener yields one that is reachable and
/// never announces itself. A caller that passes two ports cannot make either mistake, and a caller
/// that spelled the four options out could make both and still pass a test suite.
///
/// | socket   | binds                    | sharing   | broadcast |
/// |----------|--------------------------|-----------|-----------|
/// | listener | @p sharedPort            | Shared    | Off       |
/// | own      | @p ownPort               | Exclusive | On        |
/// @param bindAddress Address to bind both, e.g. `0.0.0.0`.
/// @param sharedPort The well-known port the segment broadcasts to.
/// @param ownPort The port to be answered on; 0 lets the kernel choose, which is what a node wants
///        unless something outside it needs to predict the number.
/// @return The pair as one socket, or why the first half that failed could not be bound.
[[nodiscard]] std::expected<std::unique_ptr<IDatagramSocket>, NetError> openSharedPortUdpSocket(
    std::string_view bindAddress, std::uint16_t sharedPort, std::uint16_t ownPort);

} // namespace core::net
